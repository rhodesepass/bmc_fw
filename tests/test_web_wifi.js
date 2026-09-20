'use strict';
const assert=require('assert'),fs=require('fs'),vm=require('vm'),crypto=require('crypto'),path=require('path');
const elements=new Map(), el=id=>{if(!elements.has(id)) elements.set(id,{value:'',files:[],hidden:false,removeAttribute(name){delete this[name];}});return elements.get(id);};
let httpReply, cleared=false, fetched=[];
const context={document:{getElementById:el},navigator:{},crypto:{getRandomValues:a=>crypto.webcrypto.getRandomValues(a)},TextEncoder,TextDecoder,DataView,Uint8Array,Uint32Array,Set,Date,Error,Promise,Number,Array,Math,URL,URLSearchParams,AbortController,setTimeout,clearTimeout,
location:{protocol:'http:',hash:'#token=secret-token',pathname:'/',search:''},history:{replaceState:()=>{cleared=true;}},
fetch:async(url,options)=>{fetched.push([url,options]);return {ok:true,arrayBuffer:async()=>httpReply};}};
vm.createContext(context);
const source=fs.readFileSync(path.join(__dirname,'../web/ota.js'),'utf8').replace('  controls();\n})();','  controls();globalThis.api={sha256,send,sendBatch,frame,upload,control,load};\n})();');
vm.runInContext(source,context);
function ack(session,seq,consumed,state=3,durable=0,stage=2){const b=new ArrayBuffer(32),v=new DataView(b);[0x31555042,session,seq,consumed,durable,state,0,stage].forEach((n,i)=>v.setUint32(i*4,n,true));return b;}
(async()=>{
assert(cleared);assert(el('wifiSetup').hidden);assert.equal(el('start').disabled,false);
const fileOf=size=>({size,arrayBuffer:async()=>new Uint8Array(size).buffer});
for(const size of [4,63488]) assert.equal((await context.api.load(fileOf(size),6)).data.length,size);
for(const size of [0,1,3,5,63487,63489,63492]) await assert.rejects(context.api.load(fileOf(size),6));
await assert.rejects(context.api.load(fileOf(4),7));
const bmcFile=size=>({size,arrayBuffer:async()=>{const bytes=new Uint8Array(size);bytes[0]=0xe9;return bytes.buffer;}});
for(const size of [288,1536<<10]) assert.equal((await context.api.load(bmcFile(size),7)).data.length,size);
for(const size of [287,(1536<<10)+1]) await assert.rejects(context.api.load(bmcFile(size),7));
await assert.rejects(context.api.load(fileOf(288),7));
for(const count of [0,1,3,55,56,63,64,65,1000,131072]){const bytes=Uint8Array.from({length:count},(_,i)=>i%251);assert.equal(Buffer.from(await context.api.sha256(bytes)).toString('hex'),crypto.createHash('sha256').update(bytes).digest('hex'));}
httpReply=ack(42,2,3);await context.api.send(context.api.frame(11,42,2,0,Uint8Array.from([1,2,3])));
assert.equal(fetched.length,1);assert.equal(fetched[0][0],'/v1/channels/ota');assert.equal(fetched[0][1].headers.Authorization,'Bearer secret-token');assert.equal(fetched[0][1].body.length,2052);assert.equal(fetched[0][1].redirect,'error');
for(const response of [ack(43,2,3),ack(42,3,3),ack(42,2,2),ack(42,2,3,5),new ArrayBuffer(0)]){httpReply=response;await assert.rejects(context.api.send(context.api.frame(11,42,2,0,Uint8Array.from([1,2,3]))));}
httpReply=ack(42,3,3,3);await assert.rejects(context.api.send(context.api.frame(12,42,3,3)),/END/);
let events=[];context.fetch=async(url,options)=>{events.push(url);if(url==='/v1/channels/ota'){const v=new DataView(options.body.buffer),op=v.getUint32(4,true);return{ok:true,arrayBuffer:async()=>ack(v.getUint32(8,true),v.getUint32(12,true),v.getUint32(16,true)+(op===11?v.getUint32(20,true):0),op===12?4:3)}};throw Error('unexpected')};
await assert.rejects(context.api.upload({data:Uint8Array.from([1,2,3]),hash:new Uint8Array(32)},3),/持久化/);
await assert.rejects(context.api.upload({data:new Uint8Array(4),hash:new Uint8Array(32)},6),/持久化/);
let replay=[];const packet=context.api.frame(11,42,2,0,Uint8Array.from([1,2,3]));
context.fetch=async(url,options)=>{if(url==='/v1/status')return{ok:true,arrayBuffer:async()=>ack(42,1,0)};replay.push(Buffer.from(options.body));if(replay.length===1)throw new TypeError('response lost after ACK');return{ok:true,arrayBuffer:async()=>ack(42,2,3)}};
await context.api.send(packet);assert.equal(replay.length,2);assert(replay[0].equals(replay[1]));
for(const status of [401,409,504]) {replay=[];context.fetch=async(url,options)=>{if(url==='/v1/status')return{ok:true,arrayBuffer:async()=>ack(42,1,0)};replay.push(Buffer.from(options.body));return{ok:false,status}};await assert.rejects(context.api.send(packet));assert.equal(replay.length,status===504?3:1);for(const bytes of replay)assert(bytes.equals(Buffer.from(packet)));}
const second=context.api.frame(11,42,3,3,Uint8Array.from([4,5,6]));replay=[];
context.fetch=async(url,options)=>{if(url==='/v1/status')return{ok:true,arrayBuffer:async()=>ack(42,2,3)};replay.push(Buffer.from(options.body));if(replay.length===1)throw new TypeError('partial batch ACK');return{ok:true,arrayBuffer:async()=>ack(42,3,6)}};
await context.api.sendBatch([packet,second]);assert.equal(replay.length,2);assert(replay[0].equals(Buffer.concat([Buffer.from(packet),Buffer.from(second)])));assert(replay[1].equals(Buffer.from(second)));
for(const bad of [ack(43,2,3),ack(42,2,2),ack(42,4,6)]){context.fetch=async(url,options)=>{if(url==='/v1/status')return{ok:true,arrayBuffer:async()=>bad};throw new TypeError('lost')};await assert.rejects(context.api.sendBatch([packet,second]));}
let lengths=[];context.fetch=async(url,options)=>{const body=options.body, view=new DataView(body.buffer,body.byteLength-2052),op=view.getUint32(4,true),consumed=view.getUint32(16,true)+(op===11?view.getUint32(20,true):0);lengths.push(body.length/2052);return{ok:true,arrayBuffer:async()=>ack(view.getUint32(8,true),view.getUint32(12,true),consumed,op===12?4:3,consumed)}};
await context.api.upload({data:new Uint8Array(2016*17+5),hash:new Uint8Array(32)},3);assert.deepEqual(lengths,[1,16,2,1]);
context.fetch=async(url,options)=>{const view=new DataView(options.body.buffer,options.body.byteLength-2052),op=view.getUint32(4,true),consumed=view.getUint32(16,true)+(op===11?view.getUint32(20,true):0);return{ok:true,arrayBuffer:async()=>ack(view.getUint32(8,true),view.getUint32(12,true),consumed,op===12?4:3,op===12?consumed:0)}};
await context.api.upload({data:new Uint8Array(63488),hash:new Uint8Array(32)},6);
function setupUpdate(target,extra={}) {
  el('target').value=String(target);el('file').files=[target===7?bmcFile(288):fileOf(4)];el('hash').value='';el('ubootFit').files=[];
  for(const id of ['extraBoot','extraRootfs','extraUboot','extraTouch','extraBmc']) el(id).files=extra[id]?[extra[id]]:[];
}
let updateEvents=[],updateStage=0,updateTarget=0,failTarget=0;
context.fetch=async(url,options)=>{
  let reply;
  if(url==='/v1/control') {const command=options.body;updateEvents.push(command);if(command==='ota-bmc')updateStage=3;if(command==='ota')updateStage=1;reply=new TextEncoder().encode('OK '+command).buffer;}
  else if(url==='/v1/status') reply=ack(0,0,0,0,0,updateStage);
  else {const view=new DataView(options.body.buffer,options.body.byteLength-2052),op=view.getUint32(4,true),consumed=view.getUint32(16,true)+(op===11?view.getUint32(20,true):0);
    if(op===10){updateTarget=view.getUint32(36,true);updateEvents.push('target'+updateTarget);}
    if(op===12){updateEvents.push('end'+updateTarget);if(updateTarget===failTarget)return{ok:false,status:409};if(updateTarget===4)updateStage=2;}
    reply=ack(view.getUint32(8,true),view.getUint32(12,true),consumed,op===12?4:3,[4,5].includes(updateTarget)?0:consumed,updateStage);
  }
  return{ok:true,arrayBuffer:async()=>reply};
};
setupUpdate(7);await el('start').onclick();
assert.deepEqual(updateEvents,['ota-bmc','target7','end7','ota-finish']);assert.equal(el('boot').disabled,false);assert(el('notice').textContent.includes('尚未重启'));
await el('boot').onclick();assert.equal(updateEvents.at(-1),'ota-reboot');
updateEvents=[];setupUpdate(1,{extraTouch:fileOf(4),extraBmc:bmcFile(288)});el('ubootFit').files=[fileOf(4)];await el('start').onclick();
assert.deepEqual(updateEvents,['ota-bmc','target7','end7','ota','target4','end4','target1','end1','target6','end6','ota-finish']);await el('boot').onclick();assert.equal(updateEvents.at(-1),'ota-reboot');
updateEvents=[];setupUpdate(1);el('ubootFit').files=[fileOf(4)];await el('start').onclick();await el('boot').onclick();assert.equal(updateEvents.at(-1),'boot');
for(const [target,extras] of [[7,{extraBmc:bmcFile(288)}],[5,{extraBmc:bmcFile(288)}],[7,{extraTouch:fileOf(3)}],[7,{extraTouch:fileOf(4)}]]) {
  updateEvents=[];setupUpdate(target,extras);await el('start').onclick();assert.deepEqual(updateEvents,[]);assert.equal(el('boot').disabled,true);
}
updateEvents=[];failTarget=6;setupUpdate(7,{extraTouch:fileOf(4)});el('ubootFit').files=[fileOf(4)];await el('start').onclick();assert(updateEvents.includes('end7'));assert(!updateEvents.includes('ota-finish'));assert.equal(el('boot').disabled,true);
const bleElements=new Map(), bleEl=id=>{if(!bleElements.has(id))bleElements.set(id,{value:'',files:[],removeAttribute(name){delete this[name];}});return bleElements.get(id);};
let commands=[],response='';const ready={mode:'sta',state:'ready',ssid:'网络',ip:'192.0.2.1',url:'http://192.0.2.1',token:'session-secret',ap_password:''};
const chars={4:{writeValueWithResponse:async data=>{const command=new TextDecoder().decode(data);commands.push(command);response='OK '+(command==='wifi-status'?JSON.stringify(ready):'starting');},readValue:async()=>new TextEncoder().encode(response)},7:{readValue:async()=>new DataView(ack(0,0,0))},6:{}};
const device={addEventListener:()=>{},gatt:{connected:true,connect:async()=>({getPrimaryService:async()=>({getCharacteristic:async uuid=>chars[Number(uuid[7])]})})}};
const bleContext={...context,document:{getElementById:bleEl},navigator:{bluetooth:{requestDevice:async()=>device}},location:{protocol:'https:',hash:'',pathname:'/',search:''}};
vm.createContext(bleContext);vm.runInContext(source,bleContext);await bleEl('connect').onclick();
bleEl('wifiSsid').value='网络';bleEl('wifiPassword').value='';await bleEl('wifiSta').onclick();
assert.deepEqual(commands,['wifi-sta e7bd91e7bb9c -','wifi-status']);assert.equal(bleEl('wifiOpen').href,'http://192.0.2.1/#token=session-secret');assert.equal(bleEl('wifiOpen').hidden,false);
commands=[];ready.mode='ap';ready.ap_password='random-pass';await bleEl('wifiAp').onclick();assert.deepEqual(commands,['wifi-ap','wifi-status']);assert.equal(bleEl('apPassword').value,'random-pass');assert(bleEl('notice').textContent.includes('系统 Wi-Fi 设置'));
await bleEl('wifiStop').onclick();assert.equal(bleEl('wifiOpen').href,undefined);assert.equal(bleEl('apPassword').value,'');
console.log('HTTP 浏览器：SHA-256 回退、令牌、ACK、END 持久化边界通过');
})().catch(e=>{console.error(e);process.exitCode=1;});
