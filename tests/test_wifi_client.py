import asyncio
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import threading
import types
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('bmc_ota_wifi', Path(__file__).resolve().parents[1] / 'tools/bmc_ota.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


def ack(session=42, seq=1, consumed=0, durable=0, state=3):
    return struct.pack('<8I', ota.MAGIC, session, seq, consumed, durable, state, 0, 2)


class WifiTests(unittest.IsolatedAsyncioTestCase):
    def test_provision_encoding_and_limits(self):
        self.assertEqual(ota.wifi_command('网络', ''), 'wifi-sta e7bd91e7bb9c -')
        self.assertLessEqual(len(ota.wifi_command('a'*32, 'b'*63)), 244)
        for ssid, password in [('', ''), ('网'*11, ''), ('test', '1234567'), ('test', 'x'*64), ('ssid\0', ''), ('ssid', 'password\0')]:
            with self.assertRaises(ValueError):
                ota.wifi_command(ssid, password)

    async def test_provision_waits_until_ready(self):
        commands = []
        responses = ['starting', json.dumps({'state': 'connecting'}), json.dumps({'state': 'ready', 'mode': 'sta', 'url': 'http://192.0.2.1', 'token': 'secret'})]
        class Client:
            mtu_size = 247
            async def write_gatt_char(self, uuid, data, response):
                commands.append(data.decode())
            async def read_gatt_char(self, uuid):
                return ('OK '+responses.pop(0)).encode()
        args = types.SimpleNamespace(wifi_ap=False, wifi_sta='wifi', wifi_password='', timeout=2)
        with patch.object(ota.asyncio, 'sleep', return_value=None):
            self.assertEqual(await ota.provision_wifi(Client(), args), ('http://192.0.2.1', 'secret'))
        self.assertEqual(commands, ['wifi-sta 77696669 -', 'wifi-status', 'wifi-status'])

    async def test_http_ack_must_match(self):
        updater = ota.HttpUploader('http://127.0.0.1', 'test', progress=lambda _: None)
        packet = ota.frame(11, 42, 2, 0, b'abc')
        for response in [ack(session=43, seq=2, consumed=3), ack(seq=1, consumed=3), ack(seq=2, consumed=2), b'OK']:
            async def request(*args):
                return response
            updater.request = request
            with self.assertRaises((ValueError, RuntimeError)):
                await updater.send(packet)
        async def request(*args):
            return ack(seq=2, consumed=3)
        updater.request = request
        with patch.object(ota.asyncio, 'sleep', side_effect=AssertionError('HTTP ACK must not sleep')):
            self.assertEqual((await updater.send(packet))['consumed'], 3)
        updater.close()

    async def test_end_must_complete_and_be_durable(self):
        updater = ota.HttpUploader('http://127.0.0.1', 'test', progress=lambda _: None)
        async def request(method, path, packet):
            op, session, seq, offset, length = struct.unpack_from('<5I', packet, 4)
            return ack(session, seq, offset+(length if op==11 else 0), state=3)
        updater.request = request
        with self.assertRaisesRegex(RuntimeError, 'END'):
            await updater.upload(b'abc', 3)
        async def request(method, path, packet):
            op, session, seq, offset, length = struct.unpack_from('<5I', packet, 4)
            return ack(session, seq, offset+(length if op==11 else 0), state=4 if op==12 else 3)
        updater.request = request
        with self.assertRaisesRegex(RuntimeError, '持久化'):
            await updater.upload(b'abc', 3)
        updater.close()

    async def test_persistent_http_and_no_redirect(self):
        ports, bodies = [], []
        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args):
                pass
            def do_POST(self):
                ports.append(self.client_address[1])
                bodies.append(self.rfile.read(int(self.headers['Content-Length'])))
                self.server.test.assertEqual(self.headers['Authorization'], 'Bearer private-token')
                data = ack(seq=2, consumed=3)
                self.send_response(200 if self.path == '/v1/channels/ota' else 302)
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Location', 'http://example.invalid/')
                self.end_headers()
                self.wfile.write(data)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.test = self
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        updater = ota.HttpUploader(f'http://127.0.0.1:{server.server_port}', 'private-token', progress=lambda _: None)
        try:
            packet = ota.frame(11, 42, 2, 0, b'abc')
            await updater.send(packet)
            await updater.send(packet)
            self.assertEqual(len(set(ports)), 1)
            self.assertEqual(bodies, [packet, packet])
            with self.assertRaisesRegex(RuntimeError, '302'):
                await updater.request('POST', '/redirect', b'x')
        finally:
            updater.close()
            await asyncio.to_thread(server.shutdown)
            server.server_close()

    async def test_http_partial_batch_ack_lost_resends_only_suffix(self):
        bodies = []
        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args):
                pass
            def do_GET(self):
                data = ack(seq=2, consumed=3)
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            def do_POST(self):
                packet = self.rfile.read(int(self.headers['Content-Length']))
                bodies.append(packet)
                if len(bodies) == 1:
                    # 模拟设备已消费并 ACK，只有 HTTP 回复在网络中丢失。
                    self.server.acked = packet[2052:]
                    self.close_connection = True
                    return
                self.server.test.assertEqual(packet, self.server.acked)
                data = ack(seq=3, consumed=6)
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.test = self
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        updater = ota.HttpUploader(f'http://127.0.0.1:{server.server_port}', 'private-token', timeout=2, progress=lambda _: None)
        try:
            packet = ota.frame(11, 42, 2, 0, b'abc')
            second = ota.frame(11, 42, 3, 3, b'def')
            self.assertEqual((await updater.send_batch([packet, second]))['consumed'], 6)
            self.assertEqual(bodies, [packet + second, second])
        finally:
            updater.close()
            await asyncio.to_thread(server.shutdown)
            server.server_close()

    async def test_http_retry_is_bounded_and_only_transient(self):
        updater = ota.HttpUploader('http://127.0.0.1', 'test', progress=lambda _: None)
        packet = ota.frame(11, 42, 2, 0, b'abc')
        for code, expected in [(401, 1), (409, 1), (504, 3)]:
            requests = []
            async def request(method, path, body=None):
                if method == 'GET':
                    return ack(seq=1)
                requests.append(body)
                raise ota.HttpStatusError(code)
            updater.request = request
            with patch.object(ota.asyncio, 'sleep', return_value=None):
                with self.assertRaises(TimeoutError if code == 504 else ota.HttpStatusError):
                    await updater.send(packet)
            self.assertEqual(requests, [packet] * expected)
        requests = []
        async def request(method, path, body=None):
            if method == 'GET':
                return ack(seq=1)
            requests.append(body)
            if len(requests) == 1:
                raise ota.HttpStatusError(504)
            return ack(seq=2, consumed=3)
        updater.request = request
        with patch.object(ota.asyncio, 'sleep', return_value=None):
            await updater.send(packet)
        self.assertEqual(requests, [packet, packet])
        updater.close()

    async def test_upload_batch_boundaries_and_prefix_validation(self):
        updater = ota.HttpUploader('http://127.0.0.1', 'test', progress=lambda _: None)
        batches = []
        async def request(method, path, body):
            packets = [body[i:i+2052] for i in range(0, len(body), 2052)]
            batches.append(packets)
            _, op, session, seq, offset, length, _, _ = struct.unpack_from('<8I', packets[-1])
            consumed = offset + (length if op == 11 else 0)
            return ack(session, seq, consumed, consumed, 4 if op == 12 else 3)
        updater.request = request
        await updater.upload(bytes(2016 * 17 + 5), 3)
        self.assertEqual([len(batch) for batch in batches], [1, 16, 2, 1])
        data_batch = batches[1]
        session = struct.unpack_from('<I', data_batch[0], 8)[0]
        for current in [ack(session+1, 4, 6048), ack(session, 4, 6047), ack(session, 18, 34272)]:
            with self.assertRaises(RuntimeError):
                updater.check_prefix(data_batch, ota.status(current))
        self.assertEqual(updater.check_prefix(data_batch, ota.status(ack(session, 4, 6048))), 3)
        updater.close()

    @unittest.skipUnless(shutil.which('node'), 'Node.js unavailable')
    def test_browser_wifi_and_http(self):
        result = subprocess.run(['node', str(Path(__file__).with_name('test_web_wifi.js'))], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
