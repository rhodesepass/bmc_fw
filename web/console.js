(() => {
  'use strict';
  const uuid = n => `6e40000${n}-b5a3-f393-e0a9-e50e24dcca9e`;
  const enc = new TextEncoder();
  const $ = id => document.getElementById(id);
  const state = { device: null, chars: null, epoch: 0, connected: false, connecting: false, queue: Promise.resolve(), busy: 0, recovery: null, expectedDisconnect: false, uartPending: 0 };
  const logLimit = 2 * 1024 * 1024;
  let refreshPending = false, traceReading = false;
  let unsupported = new Set();
  const fieldsOf = reply => Object.fromEntries([...reply.matchAll(/([a-z_]+)=([^\s]+)/g)].map(match => [match[1], match[2]]));
  const num = (fields, key) => fields[key] !== undefined && Number.isFinite(Number(fields[key])) ? Number(fields[key]).toLocaleString('zh-CN') : '—';
  const bytesOf = (fields, key) => fields[key] !== undefined && Number.isFinite(Number(fields[key])) ? byteLabel(Number(fields[key])) : '—';
  const choice = (value, yes, no) => value === '1' ? yes : value === '0' ? no : '—';
  const theme = { background: '#0c151d', foreground: '#d3e1e5', cursor: '#77d5c3', selectionBackground: '#31505e', black: '#15232c', red: '#f17b78', green: '#8bd5a6', yellow: '#e6bd76', blue: '#7fb2e5', magenta: '#c6a1d9', cyan: '#7cd6d1', white: '#e2e9ec', brightBlack: '#6d8390', brightRed: '#ffa19a', brightGreen: '#b4e9bd', brightYellow: '#f9d99c', brightBlue: '#a3cdf5', brightMagenta: '#dfbdec', brightCyan: '#acece3', brightWhite: '#ffffff' };
  const channels = {};
  const byteLabel = n => n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KiB` : `${(n / 1048576).toFixed(2)} MiB`;

  function event(message, kind = 'info') {
    const li = document.createElement('li');
    const time = document.createElement('time');
    time.textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false });
    const text = document.createElement('span');
    text.textContent = message;
    li.dataset.kind = kind;
    li.append(time, text);
    $('events').prepend(li);
    while ($('events').children.length > 60) $('events').lastElementChild.remove();
  }
  function notice(message, kind = 'info') {
    $('notice').textContent = message;
    $('notice').dataset.kind = kind;
    $('notice').dataset.level = kind === 'warn' ? 'warning' : kind;
    event(message, kind);
  }
  function report(error) {
    if (error.name === 'AbortError') return;
    let message = error.message || String(error);
    if (error.name === 'NotFoundError') message = '未选择设备，或未发现 EPASS-BMC。请确认蓝牙已打开，且手机未占用连接。';
    if (error.name === 'SecurityError') message = '浏览器拒绝了蓝牙访问。请使用 localhost 或 HTTPS，并允许 Web Bluetooth。';
    notice(message, 'error');
  }
  function updateControls() {
    $('connectBtn').disabled = state.connecting || state.connected || !navigator.bluetooth || !window.isSecureContext;
    $('disconnectBtn').disabled = !state.connected;
    document.querySelectorAll('[data-connected]').forEach(el => { el.disabled = !state.connected || state.expectedDisconnect; });
    if (traceReading) $('readTraceBtn').disabled = true;
    for (const [name, channel] of Object.entries(channels)) {
      channel.term.options.disableStdin = name !== 'app' || !state.connected || state.expectedDisconnect;
      document.querySelectorAll(`[data-save="${name}"], [data-clear="${name}"]`).forEach(el => { el.disabled = channel.size === 0; });
    }
    document.body.dataset.connected = String(state.connected);
    $('connectionState').dataset.state = state.connected ? 'connected' : 'disconnected';
    $('connectionState').textContent = state.connecting ? '连接中' : state.connected ? '已连接' : '未连接';
  }
  function addChannel(name) {
    const term = new Terminal({ theme, fontFamily: '"JetBrains Mono", "Noto Sans Mono", "DejaVu Sans Mono", monospace', fontSize: 12, lineHeight: 1.3, cursorBlink: name === 'app', cursorInactiveStyle: 'outline', scrollback: 4000, convertEol: true, disableStdin: true, allowProposedApi: false, windowOptions: {} });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open($(name + 'Terminal'));
    const channel = { term, fit, parts: [], size: 0, total: 0, trimmed: false, follow: true };
    channels[name] = channel;
    new ResizeObserver(() => requestAnimationFrame(() => { try { fit.fit(); } catch (_) {} })).observe($(name + 'Terminal'));
    if (name === 'app') term.onData(data => { if (state.connected && !state.expectedDisconnect) sendUart(data).catch(report); });
    requestAnimationFrame(() => fit.fit());
  }
  function append(name, value) {
    const channel = channels[name];
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
    channel.parts.push(bytes);
    channel.size += bytes.length;
    channel.total += bytes.length;
    while (channel.size > logLimit) {
      const excess = channel.size - logLimit;
      const first = channel.parts[0];
      if (first.length <= excess) { channel.parts.shift(); channel.size -= first.length; }
      else { channel.parts[0] = first.slice(excess); channel.size -= excess; }
      if (!channel.trimmed) { channel.trimmed = true; event(`${name === 'app' ? 'APP' : 'BMC'} 本地日志超过 2 MiB，开始淘汰最早的数据。`, 'warn'); }
    }
    const top = channel.term.buffer.active.viewportY;
    channel.term.write(bytes, () => { if (channel.follow) channel.term.scrollToBottom(); else channel.term.scrollToLine(top); });
    $(name + 'Empty').hidden = true;
    $(name + 'Bytes').textContent = byteLabel(channel.total);
    document.querySelectorAll(`[data-save="${name}"], [data-clear="${name}"]`).forEach(el => { el.disabled = false; });
  }
  function clear(name) {
    const c = channels[name];
    c.term.clear(); c.parts = []; c.size = c.total = 0; c.trimmed = false;
    $(name + 'Bytes').textContent = '0 B';
    updateControls();
  }
  function save(name) {
    const blob = new Blob(channels[name].parts, { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url; link.download = `${name === 'app' ? 'd1s' : 'bmc'}-${new Date().toISOString().replace(/[:.]/g, '-')}.log`;
    link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function exclusive(fn) {
    const epoch = state.epoch;
    const next = state.queue.catch(() => {}).then(async () => {
      if (!state.connected || epoch !== state.epoch) throw new DOMException('连接已结束', 'AbortError');
      state.busy++;
      try { return await fn(epoch); } finally { state.busy--; }
    });
    state.queue = next;
    return next;
  }
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  function assertEpoch(epoch) {
    if (!state.connected || epoch !== state.epoch) throw new DOMException('设备已断开', 'AbortError');
  }
  async function control(command, { quiet = false, recovery = false } = {}) {
    return exclusive(async epoch => {
      const chars = state.chars;
      await chars.control.writeValueWithResponse(enc.encode(command));
      const deadline = Date.now() + 7000;
      while (Date.now() < deadline) {
        assertEpoch(epoch);
        const result = new TextDecoder().decode(await chars.control.readValue());
        assertEpoch(epoch);
        if (result.startsWith('OK ')) {
          const reply = result.slice(3).trim();
          if (command === 'status') renderStatus(reply);
          else if (!quiet) event(`${command} · ${reply}`, 'success');
          return reply;
        }
        if (result.startsWith('ERROR ')) {
          const error = new Error(result.slice(6));
          error.unsupported = /unknown command/i.test(result);
          throw error;
        }
        if (!result.startsWith('BUSY ')) throw new Error(`无法识别控制回复：${result.slice(0, 180)}`);
        await delay(100);
      }
      throw new Error(`${command} 等待设备回复超时；请先刷新状态确认结果。`);
    }).catch(error => {
      if (recovery && state.expectedDisconnect && !state.connected) return '设备已断开，请按后续步骤验证下载模式。';
      throw error;
    });
  }
  function sendUart(text) {
    const bytes = enc.encode(text);
    if (state.uartPending + bytes.length > 65536) return Promise.reject(new Error('终端待发送数据超过 64 KiB，请等待发送完成。'));
    state.uartPending += bytes.length;
    return exclusive(async epoch => {
      const rx = state.chars.rx;
      // Web Bluetooth does not expose ATT MTU; 20-byte writes work at the minimum MTU.
      for (let offset = 0; offset < bytes.length; offset += 20) {
        assertEpoch(epoch);
        await rx.writeValueWithResponse(bytes.slice(offset, offset + 20));
      }
    }).finally(() => { state.uartPending -= bytes.length; });
  }
  function renderStatus(reply) {
    const fields = fieldsOf(reply);
    const number = key => { const n = Number(fields[key]); return Number.isFinite(n) ? n.toLocaleString('zh-CN') : '—'; };
    $('modeValue').textContent = fields.mode ? fields.mode.toUpperCase() : '—';
    $('resetValue').textContent = fields.held === '1' ? '保持复位' : fields.held === '0' ? '已释放' : '—';
    $('resetValue').dataset.state = fields.held === '1' ? 'warn' : 'good';
    $('spiValue').textContent = fields.spi === 'ESP_OK' ? (fields.mode === 'fel' ? '静默' : '就绪') : fields.spi || '—';
    $('spiValue').dataset.state = fields.spi === 'ESP_OK' ? 'good' : 'warn';
    $('pagesValue').textContent = number('page');
    $('cacheValue').textContent = number('cache');
    $('unexpectedValue').textContent = number('unexpected');
    $('unexpectedValue').dataset.state = Number(fields.unexpected) > 0 ? 'warn' : 'good';
    $('uartCountValue').textContent = fields.uart ? byteLabel(Number(fields.uart)) : '—';
    $('droppedValue').textContent = `${number('lost')} / ${number('ble_drop')}`;
    $('cacheMissValue').textContent = number('cache_miss');
    $('queueValue').textContent = fields.queue || '—';
    $('queueValue').dataset.state = fields.queue === 'ESP_OK' ? 'good' : 'warn';
    $('heapValue').textContent = `${bytesOf(fields, 'heap_free')} / ${bytesOf(fields, 'heap_largest')}`;
    $('resetReasonValue').textContent = number('reset_reason');
    $('logCountValue').textContent = bytesOf(fields, 'log');
    $('updatedAt').textContent = `状态更新 ${new Date().toLocaleTimeString('zh-CN', { hour12: false })}`;
  }
  function renderPower(reply) {
    const f = fieldsOf(reply);
    $('powerStateValue').textContent = ({0: '恢复 / 等待 READY', 1: '运行中', 2: '关机中', 3: '已关机'})[f.state] || (f.state === undefined ? '—' : `未知 (${f.state})`);
    $('powerStateValue').dataset.state = f.state === '1' ? 'good' : 'warn';
    $('armedValue').textContent = choice(f.armed, '已授权', '未授权');
    $('mainsysValue').textContent = choice(f.mainsys, '开启', '关闭');
    $('epochValue').textContent = `${num(f, 'epoch')} / ${num(f, 'boot_attempts')}`;
    $('keyValue').textContent = `${num(f, 'key_gpio')} / ${num(f, 'key_level')}`;
    $('keyDetailValue').textContent = `${choice(f.key_stable, '按下', '松开')} / ${choice(f.key_released, '是', '否')} / ${num(f, 'key_edges')}`;
  }
  function renderOta(reply) {
    const f = fieldsOf(reply);
    $('otaPendingValue').textContent = `${choice(f.pending, '待完成', '无待完成')} / ${choice(f.requested, '是', '否')}`;
    $('otaPendingValue').dataset.state = f.pending === '1' ? 'warn' : 'good';
    $('otaStateValue').textContent = `${num(f, 'stage')} / ${num(f, 'state')}`;
    $('otaSessionValue').textContent = `${num(f, 'session')} / ${num(f, 'ack')}`;
    $('otaBytesValue').textContent = `${bytesOf(f, 'consumed')} / ${bytesOf(f, 'durable')}`;
    $('otaErrorValue').textContent = num(f, 'error');
    if (f.pending === '1') $('otaSupport').textContent = '更新未完成，boot/reset 会被固件拒绝；请到更新页恢复上传。';
  }
  function renderWifi(reply) {
    const f = JSON.parse(reply);
    $('wifiStateValue').textContent = `${f.mode ?? '—'} / ${f.state ?? '—'}`;
    $('wifiSsidValue').textContent = f.ssid || '—';
    $('wifiIpValue').textContent = f.ip || '—';
    $('wifiErrorValue').textContent = f.error ?? '—';
  }
  async function refreshStatus() {
    if (refreshPending || traceReading) return;
    refreshPending = true;
    const epoch = state.epoch;
    try {
      await control('status', { quiet: true });
      for (const [command, prefix, render] of [['power-status', 'power', renderPower], ['ota-status', 'ota', renderOta], ['wifi-status', 'wifi', renderWifi]]) {
        assertEpoch(epoch);
        if (unsupported.has(command)) continue;
        try {
          const reply = await control(command, { quiet: true });
          assertEpoch(epoch);
          $(prefix + 'Support').textContent = `已更新 ${new Date().toLocaleTimeString('zh-CN', { hour12: false })}`;
          render(reply);
        } catch (error) {
          assertEpoch(epoch);
          if (error.unsupported) {
            unsupported.add(command);
            $(prefix + 'Support').textContent = '当前固件不支持此查询';
          } else {
            $(prefix + 'Support').textContent = '查询失败，显示值可能过期';
            report(error);
          }
        }
      }
    } finally { if (epoch === state.epoch) refreshPending = false; }
  }
  async function readTrace() {
    if (traceReading) return;
    traceReading = true;
    const epoch = state.epoch;
    $('readTraceBtn').disabled = true;
    $('traceDetails').open = true;
    $('traceOutput').textContent = '';
    $('traceCount').textContent = '读取中';
    const rows = [];
    try {
      for (let index = 0; index < 256;) {
        assertEpoch(epoch);
        const reply = await control(`trace ${index}`, { quiet: true });
        assertEpoch(epoch);
        if (reply.trim() === 'end') break;
        const batch = reply.trim().split(/\r?\n/);
        if (!batch.length || batch.length > 8 || batch.some((row, i) => !/^\d+ [\da-f]{8} \d+ [\da-f]{8} [\da-f]{8}$/i.test(row) || Number(row.split(' ')[0]) !== index + i)) throw new Error('无法识别 SPI trace 回复');
        rows.push(...batch);
        index += batch.length;
        $('traceOutput').textContent = rows.join('\n');
      }
      $('traceCount').textContent = `${rows.length} 条`;
      if (!rows.length) $('traceOutput').textContent = '无事务记录。启用跟踪后重新启动 APP，再读取。';
    } catch (error) {
      if (epoch === state.epoch) $('traceCount').textContent = `读取中断，已获取 ${rows.length} 条`;
      throw error;
    } finally { if (epoch === state.epoch) { traceReading = false; updateControls(); } }
  }
  function cleanup() {
    if (state.device && state.disconnectListener) state.device.removeEventListener('gattserverdisconnected', state.disconnectListener);
    state.disconnectListener = null;
    const old = state.chars;
    if (old) {
      old.tx.removeEventListener('characteristicvaluechanged', old.onApp);
      old.log.removeEventListener('characteristicvaluechanged', old.onLog);
    }
    state.chars = null; state.connected = false; state.connecting = false; state.epoch++;
    refreshPending = false; traceReading = false; unsupported = new Set();
    $('updatedAt').textContent = '已断开 · 状态为历史快照';
    for (const prefix of ['power', 'ota', 'wifi']) $(prefix + 'Support').textContent = '已断开 · 状态为历史快照';
    $('traceToggle').checked = false;
    $('traceToggle').indeterminate = true;
    $('traceState').textContent = '设备不回报开关状态；切换后生效。';
    if ($('traceCount').textContent === '读取中') $('traceCount').textContent = '已断开 · 记录不完整';
    state.queue = Promise.resolve();
    updateControls();
  }
  function disconnected() {
    const expected = state.expectedDisconnect;
    cleanup();
    notice(expected ? 'BLE 已断开。下载模式尚需通过电脑 USB/FEL 工具确认；网页不会自动执行这些操作。' : '设备已断开，已收到的日志仍保留在当前页面。', expected ? 'info' : 'warn');
    state.expectedDisconnect = false;
    updateControls();
  }
  async function connect() {
    if (!navigator.bluetooth || !window.isSecureContext) return;
    state.connecting = true; state.expectedDisconnect = false; updateControls();
    try {
      const device = await navigator.bluetooth.requestDevice({ filters: [{ namePrefix: 'EPASS-BMC' }], optionalServices: [uuid(1)] });
      state.device = device;
      document.querySelectorAll('.metric strong, .diagnostic-grid dd').forEach(el => { el.textContent = '—'; delete el.dataset.state; });
      for (const prefix of ['power', 'ota', 'wifi']) $(prefix + 'Support').textContent = '等待设备查询';
      const epoch = ++state.epoch;
      state.disconnectListener = () => { if (state.device === device && state.epoch === epoch) disconnected(); };
      device.addEventListener('gattserverdisconnected', state.disconnectListener);
      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(uuid(1));
      const rx = await service.getCharacteristic(uuid(2));
      const tx = await service.getCharacteristic(uuid(3));
      const ctrl = await service.getCharacteristic(uuid(4));
      const log = await service.getCharacteristic(uuid(5));
      const onApp = e => { if (state.epoch === epoch) append('app', e.target.value); };
      const onLog = e => { if (state.epoch === epoch) append('bmc', e.target.value); };
      state.chars = { rx, tx, control: ctrl, log, onApp, onLog };
      tx.addEventListener('characteristicvaluechanged', onApp);
      log.addEventListener('characteristicvaluechanged', onLog);
      await tx.startNotifications();
      await log.startNotifications();
      if (!device.gatt.connected || epoch !== state.epoch) throw new Error('建立连接时设备已断开。');
      state.connected = true; state.connecting = false;
      $('deviceName').textContent = device.name || 'EPASS-BMC';
      $('deviceId').textContent = device.id || '浏览器授权设备';
      updateControls();
      notice('已连接。APP 终端和 BMC 日志分别显示；点击 APP 终端可直接输入。', 'success');
      await refreshStatus();
      await control('uart-replay', { quiet: true });
      await control('log-replay', { quiet: true });
    } catch (error) {
      if (!state.connected) {
        if (state.device?.gatt.connected) state.device.gatt.disconnect();
        cleanup();
      }
      report(error);
    }
  }
  const recoveryInfo = {
    'c3-download': {
      title: '让 C3 进入下载模式',
      text: 'C3 将在约 1 秒后关闭 BLE 并进入 ROM 下载。请先确认 APP 已运行 uopbridge；如果尚未准备好，请取消并使用“救援下载”。本网页不能加载 USB 桥接程序。',
      command: 'python -m esptool --chip esp32c3 --port /dev/ttyACM0 --before no-reset --after no-reset chip-id'
    },
    'rescue-download': {
      title: '启动救援下载',
      text: '这会中断 APP：C3 保持 APP 复位后进入 ROM，APP 随后在 SPI 静默状态下回退 FEL。BLE 会断开。请在 epass_bmc 目录的电脑终端执行下面步骤；只有实际检测到 FEL 后才能继续。',
      command: 'sleep 2\nxfel version\nbash ../uopbridge/boot.sh\npython -m esptool --chip esp32c3 --port /dev/ttyACM0 --before no-reset --after no-reset chip-id'
    }
  };
  function showRecovery(command) {
    state.recovery = command;
    const info = recoveryInfo[command];
    $('recoveryTitle').textContent = info.title; $('recoveryText').textContent = info.text;
    $('recoveryCommand').textContent = info.command;
    $('recoveryConfirm').disabled = !state.connected;
    $('recoveryDialog').showModal();
  }
  async function confirmRecovery() {
    const command = state.recovery;
    $('recoveryConfirm').disabled = true;
    state.expectedDisconnect = true; updateControls();
    try {
      const reply = await control(command, { recovery: true });
      notice(`${reply} 请保留下面的电脑操作步骤。`, 'warn');
      setTimeout(() => { if (state.connected && state.expectedDisconnect) { state.expectedDisconnect = false; updateControls(); notice('设备仍在线，不能确认已进入下载模式；请刷新状态。', 'warn'); } }, 6000);
    } catch (error) {
      state.expectedDisconnect = false; updateControls();
      $('recoveryConfirm').disabled = !state.connected; report(error);
    }
  }
  addChannel('app'); addChannel('bmc');
  $('connectBtn').addEventListener('click', connect);
  $('disconnectBtn').addEventListener('click', () => { state.expectedDisconnect = false; state.device?.gatt.disconnect(); });
  $('refreshBtn').addEventListener('click', () => refreshStatus().catch(report));
  $('readTraceBtn').addEventListener('click', () => readTrace().catch(report));
  $('openOtaBtn').addEventListener('click', () => { state.device?.gatt.disconnect(); window.location.assign('ota.html'); });
  document.querySelectorAll('[data-command]').forEach(el => el.addEventListener('click', async () => { try { await control(el.dataset.command); await refreshStatus(); } catch (error) { report(error); } }));
  document.querySelectorAll('[data-recovery]').forEach(el => el.addEventListener('click', () => showRecovery(el.dataset.recovery)));
  document.querySelectorAll('[data-replay]').forEach(el => el.addEventListener('click', () => control(el.dataset.replay === 'app' ? 'uart-replay' : 'log-replay').catch(report)));
  document.querySelectorAll('[data-clear]').forEach(el => el.addEventListener('click', () => clear(el.dataset.clear)));
  document.querySelectorAll('[data-save]').forEach(el => el.addEventListener('click', () => save(el.dataset.save)));
  document.querySelectorAll('[data-follow]').forEach(el => el.addEventListener('click', () => { const c = channels[el.dataset.follow]; c.follow = !c.follow; el.setAttribute('aria-pressed', String(c.follow)); if (c.follow) c.term.scrollToBottom(); }));
  const keys = { 'ctrl-c': '\x03', 'ctrl-d': '\x04', tab: '\t', escape: '\x1b', enter: '\r' };
  document.querySelectorAll('[data-key]').forEach(el => el.addEventListener('click', () => { const value = el.dataset.key === 'resize' ? `stty cols ${channels.app.term.cols} rows ${channels.app.term.rows}\r` : keys[el.dataset.key]; if (value) sendUart(value).catch(report); }));
  $('traceToggle').indeterminate = true;
  $('traceToggle').addEventListener('change', async e => { const enabled = e.target.checked; try { await control(enabled ? 'trace-on' : 'trace-off'); $('traceState').textContent = enabled ? '本次已请求启用；重启 APP 后读取。' : '本次已请求关闭跟踪。'; } catch (error) { e.target.checked = false; e.target.indeterminate = true; report(error); } });
  const history = []; let historyIndex = 0; let draft = '';
  $('commandForm').addEventListener('submit', async e => {
    e.preventDefault();
    const input = $('commandInput'); const value = input.value;
    if (enc.encode(value).length > 8192) return notice('单条命令最多 8 KiB。', 'error');
    if (value && history.at(-1) !== value) { history.push(value); if (history.length > 50) history.shift(); }
    historyIndex = history.length; draft = ''; input.value = '';
    try { await sendUart(value + '\r'); input.focus(); } catch (error) { report(error); }
  });
  $('commandInput').addEventListener('keydown', e => {
    if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
    e.preventDefault(); if (historyIndex === history.length) draft = e.target.value;
    historyIndex = Math.max(0, Math.min(history.length, historyIndex + (e.key === 'ArrowUp' ? -1 : 1)));
    e.target.value = historyIndex === history.length ? draft : history[historyIndex];
  });
  $('recoveryCancel').addEventListener('click', () => $('recoveryDialog').close());
  $('recoveryConfirm').addEventListener('click', confirmRecovery);
  $('copyRecoveryBtn').addEventListener('click', async () => { try { await navigator.clipboard.writeText($('recoveryCommand').textContent); event('已复制电脑操作步骤。', 'success'); } catch (_) { notice('无法访问剪贴板，请手动选择并复制上面的命令。', 'warn'); } });
  setInterval(() => { if (state.connected && !state.expectedDisconnect && !state.busy && !state.uartPending && !document.hidden) refreshStatus().catch(report); }, 3000);
  if (!navigator.bluetooth || !window.isSecureContext) {
    $('capabilityNotice').hidden = false;
    $('capabilityNotice').textContent = !window.isSecureContext ? '蓝牙访问需要安全页面：请通过 localhost 或 HTTPS 打开。' : '此浏览器未开放 Web Bluetooth。请使用支持它的 Chrome/Edge；Linux 可能需要启用 experimental-web-platform-features。';
  }
  updateControls();
  $('events').replaceChildren();
  event('控制台就绪，等待选择 EPASS-BMC。');
})();
