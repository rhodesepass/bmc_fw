(() => {
  'use strict';
  const $ = id => document.getElementById(id), uuid = n => `6e40000${n}-b5a3-f393-e0a9-e50e24dcca9e`;
  const MAGIC = 0x31555042, limits = {1: 10<<20, 2: 28<<20, 3: 1<<20, 4: 4<<20, 5: 32<<20, 6: 63488, 7: 1536<<10};
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  let device, chars, httpToken = null, busy = false, cancelled = false, completed = false, completedBmc = false;
  const connected = () => Boolean(httpToken || device?.gatt.connected);
  function notice(text) { $('notice').textContent = text; }
  function controls() { $('connect').disabled = busy || Boolean(httpToken); for (const id of ['wifiSta','wifiAp','wifiStop']) $(id).disabled = busy || !device?.gatt.connected; $('start').disabled = busy || !connected(); $('abort').disabled = !busy; $('boot').disabled = busy || !completed || !connected(); }
  function crc32(bytes) { let crc = 0xffffffff; for (const byte of bytes) { crc ^= byte; for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1)); } return (crc ^ 0xffffffff) >>> 0; }
  function frame(op, session, seq, offset, payload = new Uint8Array()) {
    if (payload.length > 2016) throw Error('帧载荷过长');
    const packet = new Uint8Array(2052), view = new DataView(packet.buffer);
    [MAGIC, op, session, seq, offset, payload.length, 0, 0].forEach((v, i) => view.setUint32(i * 4, v, true));
    packet.set(payload, 32); view.setUint32(2048, crc32(packet.subarray(0, 2048)), true); return packet;
  }
  async function bind() { const server = await device.gatt.connect(), service = await server.getPrimaryService(uuid(1)); chars = {}; for (const n of [4,6,7]) chars[n] = await service.getCharacteristic(uuid(n)); }
  async function read() {
    const data = httpToken ? new DataView(await request('/v1/status')) : await chars[7].readValue();
    return decodeStatus(data);
  }
  function decodeStatus(data) {
    if (data.byteLength !== 32 || data.getUint32(0, true) !== MAGIC) throw Error('OTA 状态格式不兼容');
    const s = {}; ['magic','session','seq','consumed','durable','state','error','stage'].forEach((key,i) => s[key] = data.getUint32(i*4,true));
    if (s.error || s.state === 5) throw Error(`设备错误 ${s.error}，stage=${s.stage} seq=${s.seq}`);
    if (s.state > 5 || s.stage > 3 || s.durable > s.consumed) throw Error('OTA 状态字段错误');
    return s;
  }
  async function control(command) {
    if(httpToken) { const reply=new TextDecoder().decode(await request('/v1/control',command)); if(!reply.startsWith('OK ')) throw Error('设备未确认控制命令'); return reply.slice(3).trim(); }
    await chars[4].writeValueWithResponse(new TextEncoder().encode(command));
    const deadline = Date.now()+60000;
    while (Date.now()<deadline) { const reply = new TextDecoder().decode(await chars[4].readValue()); if (reply.startsWith('OK ')) return reply.slice(3).trim(); if (/^(ERR|ERROR)/.test(reply)) throw Error(reply); await sleep(100); }
    throw Error(`${command} 控制回复超时`);
  }
  async function stage(expected) { const deadline = Date.now()+60000; while(Date.now()<deadline) { if(cancelled) throw Error('用户中止'); if((await read()).stage===expected) return; await sleep(100); } throw Error(`等待 stage ${expected} 超时`); }
  async function send(packet) {
    const v = new DataView(packet.buffer), op=v.getUint32(4,true), session=v.getUint32(8,true), seq=v.getUint32(12,true), expected=v.getUint32(16,true)+(op===11?v.getUint32(20,true):0);
    if(httpToken) return sendBatch([packet]);
    let sent=false; const deadline=Date.now()+60000;
    while(Date.now()<deadline) {
      if(cancelled) throw Error('用户中止');
      try {
        const s=await read();
        if(s.session===session && s.seq===seq) { if(s.consumed!==expected || (op===12 && s.state!==4)) throw Error('ACK 进度或 END 状态不符'); showProgress(s,session,seq); return s; }
        if(s.session===session && s.seq>seq) throw Error('设备序号超过当前帧');
        if(!sent && s.state!==2) {
          // Web Bluetooth 不提供协商 MTU，使用兼容的 20 字节 ATT 载荷。
          for(let offset=0;offset<packet.length;offset+=16) { if(cancelled) throw Error('用户中止'); const bytes=packet.subarray(offset,offset+16), part=new Uint8Array(4+bytes.length); new DataView(part.buffer).setUint32(0,offset,true); part.set(bytes,4); await chars[6].writeValueWithResponse(part); }
          sent=true;
        }
        await sleep(50);
      } catch(error) {
        if(device.gatt.connected || cancelled) throw error;
        notice('连接中断，重连后查询同一会话状态…'); await sleep(500); await bind(); const s=await read();
        if(s.session!==session) throw Error('会话已丢失，请重新开始，从偏移 0 重刷'); sent=s.state===2;
      }
    }
    throw Error(`等待 seq ${seq} ACK 超时；尚未确认成功`);
  }
  function checkPrefix(packets,s,complete=false) {
    const headers=packets.map(packet=>new DataView(packet.buffer,packet.byteOffset,packet.byteLength)), first=headers[0], last=headers[headers.length-1], start=first.getUint32(12,true);
    if(s.session!==first.getUint32(8,true) || s.seq<start-1 || s.seq>last.getUint32(12,true)) throw Error('HTTP ACK 会话或序号不符');
    const count=s.seq-start+1;
    if(complete && count!==packets.length) throw Error('HTTP 回复没有确认整个批次');
    const header=count?headers[count-1]:first, op=header.getUint32(4,true), expected=header.getUint32(16,true)+(count && op===11?header.getUint32(20,true):0);
    if(s.consumed!==expected || (count && op===12 && s.state!==4)) throw Error('ACK 进度或 END 状态不符');
    return count;
  }
  async function sendBatch(packets) {
    if(packets.length<1 || packets.length>16) throw Error('HTTP 批次必须为 1..16 帧');
    const first=new DataView(packets[0].buffer), session=first.getUint32(8,true), seq=first.getUint32(12,true);
    packets.forEach((packet,i)=>{const view=new DataView(packet.buffer);if(packet.length!==2052 || view.getUint32(8,true)!==session || view.getUint32(12,true)!==seq+i || (packets.length>1 && view.getUint32(4,true)!==11)) throw Error('HTTP 批次必须为同会话连续 DATA 帧');});
    const deadline=Date.now()+180000;let recovering=false, pending=packets, confirmed=0;
    for(let attempt=0;attempt<3;attempt++) {
      if(cancelled) throw Error('用户中止');
      const remaining=deadline-Date.now();if(remaining<=0) break;
      let reply;
      try {
        if(recovering) {
          const current=decodeStatus(new DataView(await request('/v1/status',undefined,Math.min(60000,remaining)))), count=checkPrefix(packets,current);
          if(count<confirmed) throw Error('HTTP ACK 进度倒退');confirmed=count;
          if(count===packets.length) {showProgress(current,session,current.seq);return current;}
          pending=packets.slice(count);
        }
        const body=new Uint8Array(pending.length*2052);pending.forEach((packet,i)=>body.set(packet,i*2052));
        reply=await request('/v1/channels/ota',body,Math.min(60000,Math.max(1,deadline-Date.now())));
      }catch(error) {
        if(!error.retryable) throw error;
        if(attempt===2) throw Error('HTTP 批次重试耗尽；尚未确认，设备可能仍在处理');
        recovering=true;notice('HTTP 响应丢失，查询已确认进度后重传未确认帧');await sleep(Math.min(250,Math.max(0,deadline-Date.now())));continue;
      }
      const current=decodeStatus(new DataView(reply));checkPrefix(packets,current,true);showProgress(current,session,current.seq);return current;
    }
    throw Error('HTTP 批次超时；尚未确认，设备可能仍在处理');
  }
  async function load(file,target) { if(!Object.hasOwn(limits,target) || !file || file.size<1 || file.size>limits[target]) throw Error('请选择文件，且大小不得超过目标限制'); if(target===2 && file.size % (128<<10)) throw Error('rootfs UBI 镜像必须按 128 KiB 对齐'); if(target===6 && file.size % 4) throw Error('touch raw 固件必须按 4 字节对齐'); const data=new Uint8Array(await file.arrayBuffer()); if(target===7 && (data.length<288 || data[0]!==0xe9)) throw Error('BMC 固件必须是至少 288 字节、以 E9 开头的 ESP raw app 镜像'); const hash=await sha256(data); return {data,hash}; }
  async function upload(image,target) {
    const {data,hash}=image; let session=0; while(!session) session=crypto.getRandomValues(new Uint32Array(1))[0];
    $('received').max=$('durable').max=data.length; $('received').value=$('durable').value=0;
    const payload=new Uint8Array(40), view=new DataView(payload.buffer); view.setUint32(0,data.length,true); view.setUint32(4,target,true); payload.set(hash,8);
    await send(frame(10,session,1,0,payload)); let seq=2;
    let batch=[];
    for(let offset=0;offset<data.length;offset+=2016) {
      const packet=frame(11,session,seq++,offset,data.subarray(offset,offset+2016));
      if(httpToken) {batch.push(packet);if(batch.length===16){await sendBatch(batch);batch=[];}} else await send(packet);
    }
    if(batch.length) await sendBatch(batch);
    const end=await send(frame(12,session,seq,data.length));
    if(![4,5].includes(target) && end.durable!==data.length) throw Error('END 持久化字节数不符');
  }

  function showProgress(s,session,seq) { $('received').value=s.consumed; $('durable').value=s.durable; $('status').textContent=`session ${session.toString(16)} / seq ${seq}\n已接收 ${s.consumed} B\n已持久化 ${s.durable} B`; }
  async function request(path,body,timeoutMs=65000) {
    const controller=new AbortController(), timer=setTimeout(()=>controller.abort(),timeoutMs);
    try {
      const response=await fetch(path,{method:body===undefined?'GET':'POST',body,headers:{Authorization:`Bearer ${httpToken}`,'Content-Type':typeof body==='string'?'text/plain':'application/octet-stream'},signal:controller.signal,cache:'no-store',redirect:'error'});
      if(!response.ok) {const error=Error(`设备 HTTP ${response.status}；更新未确认`);error.retryable=response.status===504;throw error;}
      return await response.arrayBuffer();
    } catch(error) {if(error.name==='TypeError' || error.name==='AbortError') error.retryable=true;throw error;} finally { clearTimeout(timer); }
  }
  async function sha256(bytes) {
    if(crypto.subtle) return new Uint8Array(await crypto.subtle.digest('SHA-256',bytes));
    // 普通局域网 HTTP 没有 WebCrypto digest；本地摘要仍必须在开始刷写前完成。
    const k=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
    const h=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]), w=new Uint32Array(64);
    const padded=new Uint8Array(Math.ceil((bytes.length+9)/64)*64);padded.set(bytes);padded[bytes.length]=128;
    const view=new DataView(padded.buffer);view.setUint32(padded.length-8,Math.floor(bytes.length/0x20000000));view.setUint32(padded.length-4,(bytes.length*8)>>>0);
    const r=(x,n)=>(x>>>n)|(x<<(32-n));
    for(let offset=0;offset<padded.length;offset+=64) {
      for(let i=0;i<16;i++)w[i]=view.getUint32(offset+i*4);
      for(let i=16;i<64;i++) {const a=w[i-15],b=w[i-2];w[i]=w[i-16]+(r(a,7)^r(a,18)^(a>>>3))+w[i-7]+(r(b,17)^r(b,19)^(b>>>10));}
      let [a,b,c,d,e,f,g,j]=h;
      for(let i=0;i<64;i++){const t1=(j+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[i]+w[i])>>>0,t2=((r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c)))>>>0;j=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;}
      [a,b,c,d,e,f,g,j].forEach((v,i)=>h[i]=(h[i]+v)>>>0);
    }
    const out=new Uint8Array(32), result=new DataView(out.buffer);h.forEach((v,i)=>result.setUint32(i*4,v));return out;
  }
  const hexUtf8=text=>Array.from(new TextEncoder().encode(text),b=>b.toString(16).padStart(2,'0')).join('');
  async function wifiStart(ap) {
    busy=true;controls();
    try {
      if(!device?.gatt.connected) throw Error('请先通过蓝牙连接 BMC');
      const ssid=$('wifiSsid').value, password=$('wifiPassword').value, encoder=new TextEncoder();
      if(!ap && (ssid.includes('\0') || password.includes('\0'))) throw Error('SSID 和密码不能包含 NUL');
      if(!ap && (encoder.encode(ssid).length<1 || encoder.encode(ssid).length>32 || (password && (encoder.encode(password).length<8 || encoder.encode(password).length>63)))) throw Error('SSID 必须为 1..32 UTF-8 字节；密码为空或 8..63 字节');
      await control(ap?'wifi-ap':`wifi-sta ${hexUtf8(ssid)} ${hexUtf8(password)||'-'}`);
      $('wifiPassword').value='';notice('正在启动 Wi-Fi…'); const deadline=Date.now()+60000;
      while(Date.now()<deadline) {
        const info=JSON.parse(await control('wifi-status'));
        if(info.state==='ready') {
          if(!info.url || !info.token) throw Error('Wi-Fi 回复缺少地址或令牌');
          const destination=new URL(info.url);
          if(destination.protocol!=='http:' || destination.username || destination.password || destination.search || destination.hash || destination.pathname!=='/') throw Error('Wi-Fi 地址格式错误');
          $('wifiInfo').textContent=`${info.mode} · ${info.ssid} · ${info.ip}`;
          $('apPassword').value=info.ap_password||'';
          const link=$('wifiOpen');link.href=destination.href+'#token='+encodeURIComponent(info.token);link.hidden=false;
          notice(ap?'热点已开启。请在系统 Wi-Fi 设置中连接此热点，再打开上传页。':'已接入现有 Wi-Fi。请让电脑处于可访问此设备的网络，再打开上传页。');return;
        }
        if(info.state==='error' || info.error) throw Error(`Wi-Fi 失败：${info.error||'unknown'}`);
        await sleep(500);
      }
      throw Error('等待 Wi-Fi 超时');
    }catch(e){notice(e.message);}finally{busy=false;controls();}
  }
  $('showApPassword').onclick=()=>{const field=$('apPassword');field.type=field.type==='password'?'text':'password';};
  $('wifiSta').onclick=()=>wifiStart(false);$('wifiAp').onclick=()=>wifiStart(true);
  $('wifiStop').onclick=async()=>{busy=true;controls();try{await control('wifi-stop');$('wifiOpen').hidden=true;$('wifiOpen').removeAttribute('href');$('apPassword').value='';$('wifiInfo').textContent='Wi-Fi 已停止';notice('Wi-Fi 已停止');}catch(e){notice(e.message);}finally{busy=false;controls();}};
  if(typeof location!=='undefined' && location.protocol==='http:') {
    const params=new URLSearchParams(location.hash.slice(1)), token=params.get('token');
    if(token) {httpToken=token;history.replaceState(null,'',location.pathname+location.search);notice('已接入设备 Wi-Fi 上传页；请选择更新镜像，更新 D1s 或 CH32 时还需完整 U-Boot。');$('wifiSetup').hidden=true;}
  }
  $('connect').onclick=async()=>{ try { if(!navigator.bluetooth) throw Error('此浏览器不支持 Web Bluetooth，请使用支持的 Chrome / Edge'); device=await navigator.bluetooth.requestDevice({filters:[{services:[uuid(1)]}]}); device.addEventListener('gattserverdisconnected',()=>{notice('蓝牙已断开');controls();}); await bind(); await read(); notice('BMC 已连接'); } catch(e){notice(e.message);} finally{controls();} };
  $('start').onclick=async()=>{
    busy=true;cancelled=false;completed=false;completedBmc=false;controls();
    try {
      const target=Number($('target').value), image=await load($('file').files[0],target);
      const images=[{image,target}], seen=new Set([target]);
      for(const [id,extraTarget] of [['extraBoot',1],['extraRootfs',2],['extraUboot',3],['extraTouch',6],['extraBmc',7]]) {const file=$(id).files[0];if(!file) continue;if(target===5 || seen.has(extraTarget)) throw Error('仅校验不能追加目标，且更新目标不能重复');images.push({image:await load(file,extraTarget),target:extraTarget});seen.add(extraTarget);}
      const bmc=images.find(entry=>entry.target===7), others=images.filter(entry=>entry.target!==7), uboot=others.length?await load($('ubootFit').files[0],4):null;
      const hex=Array.from(image.hash,b=>b.toString(16).padStart(2,'0')).join(''), expected=$('hash').value.trim().toLowerCase();
      if(expected && (!/^[0-9a-f]{64}$/.test(expected) || expected!==hex)) throw Error('预期 SHA256 与文件不符');
      notice(`SHA256 ${hex} · 正在进入更新模式`);
      if(bmc) {await control('ota-bmc');await stage(3);notice('正在写入 BMC 备用槽；END 后暂存，不切槽、不重启');await upload(bmc.image,7);}
      if(others.length) {await control('ota');await stage(1);notice('正在上传完整 U-Boot FIT 至 RAM');await upload(uboot,4);await stage(2);for(const entry of others) {notice(`正在传输目标 ${entry.target}，共 ${images.length} 个镜像`);await upload(entry.image,entry.target);}}
      if(target!==5) {await control('ota-finish');completed=true;completedBmc=Boolean(bmc);notice('全部 END 已确认，更新已提交，尚未重启；可点击启动已更新系统。');} else notice('END 已确认，传输及 SHA256 校验通过；未写入 NAND，保持更新模式。');
    } catch(e){notice(`更新未完成：${e.message}`);} finally{
      if(cancelled && connected()) { try{await control('ota-abort');notice('已中止，设备保持更新状态');}catch(e){notice(e.message);} }
      cancelled=false;busy=false;controls();
    }
  };
  $('abort').onclick=()=>{cancelled=true;notice('正在停止发送…');};
  $('boot').onclick=async()=>{busy=true;controls();try{await control(completedBmc?'ota-reboot':'boot');completed=false;notice('启动命令已确认；请重新连接设备验证新固件运行状态');}catch(e){notice(e.message);}finally{busy=false;controls();}};
  controls();
})();
