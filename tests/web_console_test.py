"""Run with a local HTTP server; tests use a simulated GATT peer, not hardware."""
import tempfile
import unittest
from pathlib import Path
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''
(() => {
 const encoder = new TextEncoder();
 const api = { commands: [], completed: [], writes: [], active: 0, maxActive: 0, connects: 0, oldChars: [], replies: {}, traceLength: 10, disconnectOn: null };
 const defaults = {
  status: 'OK mode=spl held=0 spi=ESP_OK page=45 cache=45 cache_miss=7 unexpected=0 queue=ESP_OK uart=24680 lost=0 log=1024 ble_drop=0 heap_free=65536 heap_largest=32768 reset_reason=1',
  'power-status': 'OK state=1 armed=1 mainsys=1 epoch=3 boot_attempts=2 key_gpio=9 key_level=1 key_stable=0 key_released=1 key_edges=4',
  'ota-status': 'OK pending=0 requested=0 stage=0 state=0 session=0 ack=0 consumed=0 durable=0 error=0',
  'wifi-status': 'OK {"mode":"sta","state":"connected","ssid":"Lab WiFi","ip":"192.168.1.42","error":0,"token":"private-token-123","password":"private-password-456"}'
 };
 const value = text => new DataView(encoder.encode(text).buffer);
 async function operation(fn) {
  api.active++; api.maxActive = Math.max(api.maxActive, api.active);
  try { await new Promise(r => setTimeout(r, 5)); return fn(); } finally { api.active--; }
 }
 class Characteristic extends EventTarget {
  constructor(id) { super(); this.id = id; this.value = value(''); this.response = 'OK ready'; this.reads = 0; }
  async startNotifications() { return operation(() => this); }
  async readValue() { return operation(() => {
   if (!device.gatt.connected) throw new DOMException('Disconnected', 'NetworkError');
   if (this.reads++ === 0) return value('BUSY command');
   api.completed.push(this.command); return value(this.response);
  }); }
  async writeValueWithResponse(bytes) { return operation(() => {
   if (!device.gatt.connected) throw new DOMException('Disconnected', 'NetworkError');
   const data = Array.from(new Uint8Array(bytes.buffer || bytes, bytes.byteOffset || 0, bytes.byteLength));
   if (this.id === 2) { api.writes.push(data); return; }
   const cmd = new TextDecoder().decode(Uint8Array.from(data)); api.commands.push(cmd); this.reads = 0;
   this.command = cmd;
   this.response = api.replies[cmd] ?? defaults[cmd] ?? `OK ${cmd}: ESP_OK`;
   if (cmd.startsWith('trace ') && !(cmd in api.replies)) {
    const start = Number(cmd.split(' ')[1]);
    this.response = start >= api.traceLength ? 'OK end' : 'OK ' + Array.from({length: Math.min(8, api.traceLength - start)}, (_, i) => `${start + i} 00000003 4 00000000 00000001`).join('\n');
   }
   if (api.disconnectOn === cmd) setTimeout(() => device.gatt.disconnect(), 30);
   if (cmd === 'rescue-download') setTimeout(() => device.gatt.disconnect(), 350);
  }); }
  emit(bytes) {
   const padded = new Uint8Array(bytes.length + 4); padded.set(bytes, 2);
   this.value = new DataView(padded.buffer, 2, bytes.length);
   this.dispatchEvent(new Event('characteristicvaluechanged'));
  }
 }
 const device = new EventTarget(); device.name = 'EPASS-BMC'; device.id = 'test-device-01';
 device.gatt = {
  connected: false,
  async connect() {
   this.connected = true; api.connects++;
   if (api.chars) api.oldChars.push(api.chars);
   api.chars = Object.fromEntries([2,3,4,5].map(id => [id, new Characteristic(id)]));
   return { getPrimaryService: async () => ({ getCharacteristic: async uuid => api.chars[Number(uuid[7])] }) };
  },
  disconnect() { this.connected = false; sessionStorage.setItem('mockGattDisconnected', 'yes'); device.dispatchEvent(new Event('gattserverdisconnected')); }
 };
 api.device = device;
 api.emit = (id, bytes) => api.chars[id].emit(bytes);
 Object.defineProperty(navigator, 'bluetooth', { configurable: true, value: { requestDevice: async options => { api.options = options; return device; } } });
 window.bleMock = api;
})();
'''

class WebConsoleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch(headless=True, executable_path='/usr/bin/google-chrome', args=['--no-sandbox', '--disable-dev-shm-usage'])
        (ROOT / 'build/web-test').mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.pw.stop()

    def setUp(self):
        self.context = self.browser.new_context(viewport={'width': 1480, 'height': 1040}, accept_downloads=True)
        self.page = self.context.new_page()
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.page.add_init_script(MOCK)
        self.page.goto('http://127.0.0.1:8877/', wait_until='networkidle')

    def tearDown(self):
        self.context.close()
        self.assertEqual(self.errors, [])

    def connect(self):
        before = self.page.evaluate("bleMock.completed.filter(x=>x==='log-replay').length")
        self.page.click('#connectBtn')
        self.page.wait_for_function("before => bleMock.completed.filter(x=>x==='log-replay').length > before", arg=before)
        self.assertEqual(self.page.locator('#connectionState').inner_text(), '已连接')

    def refresh(self, last_command='wifi-status'):
        before = self.page.evaluate('(cmd) => bleMock.completed.filter(x=>x===cmd).length', last_command)
        self.page.click('#refreshBtn')
        self.page.wait_for_function('([cmd, before]) => bleMock.completed.filter(x=>x===cmd).length > before', arg=[last_command, before])

    def test_diagnostics_and_state_changes(self):
        self.connect()
        expected = {
            'powerStateValue': '运行中', 'armedValue': '已授权', 'mainsysValue': '开启',
            'epochValue': '3 / 2', 'keyValue': '9 / 1', 'keyDetailValue': '松开 / 是 / 4',
            'cacheMissValue': '7', 'heapValue': '64.0 KiB / 32.0 KiB',
            'otaPendingValue': '无待完成 / 否', 'wifiSsidValue': 'Lab WiFi',
            'wifiIpValue': '192.168.1.42',
        }
        for field, value in expected.items():
            self.assertEqual(self.page.locator('#' + field).text_content(), value)
        self.assertEqual(self.page.evaluate('bleMock.commands.slice(0, 4)'), ['status', 'power-status', 'ota-status', 'wifi-status'])
        self.page.evaluate("""bleMock.replies['power-status'] = 'OK state=3 armed=0 mainsys=0 epoch=4 boot_attempts=3 key_gpio=9 key_level=0 key_stable=1 key_released=0 key_edges=5';
            bleMock.replies['ota-status'] = 'OK pending=1 requested=1 stage=2 state=3 session=42 ack=7 consumed=4096 durable=2048 error=0';""")
        self.refresh()
        self.assertEqual(self.page.locator('#powerStateValue').text_content(), '已关机')
        self.assertEqual(self.page.locator('#armedValue').text_content(), '未授权')
        self.assertEqual(self.page.locator('#mainsysValue').text_content(), '关闭')
        self.assertEqual(self.page.locator('#otaPendingValue').text_content(), '待完成 / 是')
        self.assertEqual(self.page.locator('#otaBytesValue').text_content(), '4.0 KiB / 2.0 KiB')
        self.assertIn('boot/reset', self.page.locator('#otaSupport').text_content())
        self.page.evaluate("bleMock.replies['ota-status'] = 'OK pending=0 requested=0 stage=0 state=0 session=42 ack=7 consumed=4096 durable=4096 error=0'")
        self.refresh()
        self.assertNotIn('boot/reset', self.page.locator('#otaSupport').text_content())
        self.assertEqual(self.page.evaluate('bleMock.maxActive'), 1)

    def test_missing_fields_old_firmware_and_reconnect(self):
        self.connect()
        self.page.click('#disconnectBtn')
        self.assertIn('历史快照', self.page.locator('#updatedAt').text_content())
        self.assertEqual(self.page.locator('#powerStateValue').text_content(), '运行中')
        self.page.evaluate("""bleMock.replies.status = 'OK mode=fel held=1 spi=ESP_OK future_field=99';
            for (const cmd of ['power-status', 'ota-status', 'wifi-status']) bleMock.replies[cmd] = 'ERROR unknown command';""")
        self.connect()
        self.assertEqual(self.page.locator('#modeValue').text_content(), 'FEL')
        self.assertEqual(self.page.locator('#resetValue').text_content(), '保持复位')
        for field in ['powerStateValue', 'wifiSsidValue', 'otaPendingValue', 'cacheMissValue']:
            self.assertEqual(self.page.locator('#' + field).text_content(), '—')
        for prefix in ['power', 'ota', 'wifi']:
            self.assertIn('不支持', self.page.locator('#' + prefix + 'Support').text_content())
        before = self.page.evaluate("bleMock.commands.filter(x => ['power-status', 'ota-status', 'wifi-status'].includes(x))")
        self.refresh('status')
        self.page.wait_for_timeout(200)
        self.assertEqual(self.page.evaluate("bleMock.commands.filter(x => ['power-status', 'ota-status', 'wifi-status'].includes(x))"), before)

    def test_unknown_values_and_wifi_secrets_stay_private(self):
        self.page.evaluate("bleMock.replies['power-status'] = 'OK state=99 armed=9 future=hello'")
        self.connect()
        self.assertEqual(self.page.locator('#powerStateValue').text_content(), '未知 (99)')
        self.assertEqual(self.page.locator('#armedValue').text_content(), '—')
        self.assertEqual(self.page.locator('#epochValue').text_content(), '— / —')
        self.refresh()
        for secret in ['private-token-123', 'private-password-456']:
            self.assertNotIn(secret, self.page.locator('body').text_content())
            self.assertNotIn(secret, self.page.locator('#events').text_content())

    def test_trace_pagination_and_disconnect(self):
        self.connect()
        self.page.click('#readTraceBtn')
        self.page.wait_for_function("document.getElementById('traceCount').textContent === '10 条'")
        self.assertEqual(self.page.evaluate("bleMock.commands.filter(x=>x.startsWith('trace '))"), ['trace 0', 'trace 8', 'trace 10'])
        self.assertEqual(len(self.page.locator('#traceOutput').text_content().splitlines()), 10)
        self.page.evaluate("bleMock.disconnectOn = 'trace 8'; bleMock.commands = []")
        self.page.click('#readTraceBtn')
        self.page.wait_for_function("document.getElementById('connectionState').textContent === '未连接'")
        self.page.wait_for_timeout(200)
        self.assertEqual(self.page.evaluate("bleMock.commands.filter(x=>x.startsWith('trace '))"), ['trace 0', 'trace 8'])
        self.assertEqual(len(self.page.locator('#traceOutput').text_content().splitlines()), 8)
        self.assertIn('不完整', self.page.locator('#traceCount').text_content())
        self.assertTrue(self.page.locator('#readTraceBtn').is_disabled())

    def test_trace_caps_records_and_rejects_bad_pages(self):
        self.connect()
        self.page.evaluate('bleMock.traceLength = 300')
        self.page.click('#readTraceBtn')
        self.page.wait_for_function("document.getElementById('traceCount').textContent === '256 条'")
        self.assertNotIn('trace 256', self.page.evaluate('bleMock.commands'))
        self.assertEqual(len(self.page.locator('#traceOutput').text_content().splitlines()), 256)
        self.page.evaluate("bleMock.replies['trace 0'] = 'OK <img src=x onerror=alert(1)>'")
        self.page.click('#readTraceBtn')
        self.page.wait_for_function("document.getElementById('notice').textContent.includes('无法识别 SPI trace')")
        self.assertEqual(self.page.locator('img[src=x]').count(), 0)
        self.assertIn('已获取 0 条', self.page.locator('#traceCount').text_content())

    def test_ota_navigation_disconnects_ble(self):
        self.connect()
        self.page.click('details.diagnostics > summary')
        self.page.click('#openOtaBtn')
        self.page.wait_for_url('**/ota.html')
        self.assertEqual(self.page.evaluate("sessionStorage.getItem('mockGattDisconnected')"), 'yes')

    def test_layout_and_mobile(self):
        self.assertFalse(self.page.locator('#connectBtn').is_disabled())
        self.assertTrue(self.page.locator('#disconnectBtn').is_disabled())
        self.page.screenshot(path=str(ROOT / 'build/web-test/desktop.png'), full_page=True)
        self.page.set_viewport_size({'width': 390, 'height': 844})
        self.page.wait_for_timeout(200)
        self.assertLessEqual(self.page.evaluate('document.documentElement.scrollWidth'), 390)
        self.page.screenshot(path=str(ROOT / 'build/web-test/mobile.png'), full_page=True)

    def test_data_uart_download_and_reconnect(self):
        self.connect()
        self.assertEqual(self.page.locator('#pagesValue').inner_text(), '45')
        self.assertEqual(self.page.locator('#resetValue').inner_text(), '已释放')
        raw = '你好 D1s\r\n<img src=x onerror=alert(1)>'.encode()
        self.page.evaluate('(b) => { bleMock.emit(3,b.slice(0,2)); bleMock.emit(3,b.slice(2)); bleMock.emit(5,[66,77,67,13,10]); }', list(raw))
        self.page.wait_for_function("document.getElementById('appBytes').textContent !== '0 B'")
        self.assertEqual(self.page.locator('img[src=x]').count(), 0)
        command = 'echo ' + '中文' * 25
        self.page.fill('#commandInput', command)
        self.page.click('#sendBtn')
        self.page.wait_for_function('bleMock.writes.flat().includes(13)')
        sent = bytes(self.page.evaluate('bleMock.writes.flat()'))
        self.assertEqual(sent, (command + '\r').encode())
        self.assertLessEqual(max(self.page.evaluate('bleMock.writes.map(x=>x.length)')), 20)
        self.page.click('[data-key="ctrl-c"]')
        self.page.wait_for_function('bleMock.writes.flat().at(-1) === 3')
        with self.page.expect_download() as download:
            self.page.click('[data-save="app"]')
        self.assertEqual(Path(download.value.path()).read_bytes(), raw)
        self.page.click('#refreshBtn')
        self.page.wait_for_timeout(250)
        self.assertEqual(self.page.evaluate('bleMock.maxActive'), 1)
        self.page.screenshot(path=str(ROOT / 'build/web-test/connected.png'), full_page=True)
        self.page.click('#disconnectBtn')
        self.page.wait_for_function("document.getElementById('connectionState').textContent === '未连接'")
        self.assertFalse(self.page.locator('[data-save="app"]').is_disabled())
        self.connect()
        before = self.page.locator('#appBytes').inner_text()
        self.page.evaluate('bleMock.oldChars[0][3].emit([88,88,88])')
        self.assertEqual(self.page.locator('#appBytes').inner_text(), before)

    def test_recovery_requires_explicit_action_and_reports_disconnect(self):
        self.connect()
        self.page.click('[data-recovery="rescue-download"]')
        self.assertTrue(self.page.locator('#recoveryDialog').is_visible())
        self.assertIn('xfel version', self.page.locator('#recoveryCommand').inner_text())
        self.assertNotIn('rescue-download', self.page.evaluate('bleMock.commands'))
        self.page.click('#recoveryConfirm')
        self.page.wait_for_function('bleMock.commands.includes("rescue-download")')
        self.page.wait_for_function("document.getElementById('connectionState').textContent === '未连接'")
        self.assertIn('确认', self.page.locator('#notice').inner_text())
        self.assertNotIn('恢复完成', self.page.locator('#notice').inner_text())

    def test_queued_operations_stop_after_disconnect(self):
        self.connect()
        self.page.evaluate("document.querySelector('[data-command=boot]').click(); document.querySelector('[data-command=reset]').click(); document.getElementById('disconnectBtn').click();")
        self.page.wait_for_timeout(250)
        self.assertNotIn('reset', self.page.evaluate('bleMock.commands'))
        self.assertEqual(self.page.locator('#connectionState').inner_text(), '未连接')

    def test_unsupported_browser(self):
        self.page.add_init_script("Object.defineProperty(navigator,'bluetooth',{configurable:true,value:undefined});")
        self.page.reload(wait_until='networkidle')
        self.assertTrue(self.page.locator('#capabilityNotice').is_visible())
        self.assertTrue(self.page.locator('#connectBtn').is_disabled())

if __name__ == '__main__':
    unittest.main()
