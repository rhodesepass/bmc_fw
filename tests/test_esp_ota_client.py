import contextlib
import hashlib
import importlib.util
import io
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import threading
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location('esp_ota', Path(__file__).resolve().parents[1] / 'tools/esp_ota.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


class ClientTests(unittest.TestCase):
    def test_endpoint_token_precedence_and_fragment_removal(self):
        self.assertEqual(ota.endpoint_and_token('http://127.0.0.1/#token=url-secret', {'BMC_TOKEN': 'env-secret'}),
                         ('http://127.0.0.1/v1/bmc/firmware', 'env-secret'))
        self.assertEqual(ota.endpoint_and_token('http://127.0.0.1/#token=url-secret', {})[1], 'url-secret')

    def test_invalid_endpoints_and_token(self):
        for url in ('https://host', 'http://user:pass@host', 'http://host/?token=secret', 'http://host/other'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                ota.endpoint_and_token(url, {'BMC_TOKEN': 'secret'})
        with self.assertRaises(ValueError):
            ota.endpoint_and_token('http://host', {})
        with self.assertRaises(ValueError):
            ota.endpoint_and_token('http://host', {'BMC_TOKEN': 'bad\nvalue'})

    def test_image_validation(self):
        ota.validate_image(b'\xe9' + bytes(287))
        for data in (b'', b'\xe9', bytes(288), b'\xe9' + bytes(0x180000)):
            with self.assertRaises(ValueError):
                ota.validate_image(data)

    def test_http_headers_payload_and_no_retry_or_redirect(self):
        seen = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                seen.append(('GET', self.path, self.headers, b''))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"running_partition":"ota_0"}')

            def do_POST(self):
                data = self.rfile.read(int(self.headers['Content-Length']))
                seen.append(('POST', self.path, self.headers, data))
                if self.path == '/v1/control':
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b'OK ' + data)
                    return
                if data.endswith(b'R'):
                    self.send_response(302)
                    self.send_header('Location', '/redirected')
                else:
                    self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"status":"staged","partition":"ota_1","reboot":false}')

        server = HTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            endpoint = f'http://127.0.0.1:{server.server_port}/v1/bmc/firmware'
            self.assertEqual(ota.request_json(endpoint, 'secret')['running_partition'], 'ota_0')
            data = b'\xe9' + bytes(287)
            self.assertEqual(ota.request_json(endpoint, 'secret', data)['status'], 'staged')
            self.assertEqual(seen[-1][2]['Authorization'], 'Bearer secret')
            self.assertEqual(seen[-1][2]['X-SHA256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(seen[-1][3], data)
            with self.assertRaisesRegex(RuntimeError, 'HTTP 302'):
                ota.request_json(endpoint, 'secret', data + b'R')
            self.assertEqual(len(seen), 3)
            ota.control(endpoint, 'secret', 'ota-finish')
            self.assertEqual(seen[-1][1], '/v1/control')
            self.assertEqual(seen[-1][2]['Authorization'], 'Bearer secret')
            self.assertEqual(seen[-1][3], b'ota-finish')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_get_only_without_firmware(self):
        with patch.dict(ota.os.environ, {'BMC_TOKEN': 'secret'}), patch.object(ota, 'request_json', return_value={'version': 'v1'}) as request:
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(ota.main(['--url', 'http://device']), 0)
            request.assert_called_once_with('http://device/v1/bmc/firmware', 'secret', timeout=120)
            self.assertEqual(json.loads(output.getvalue()), {'version': 'v1'})

    def test_stage_ack_requires_all_fields(self):
        for reply, expected in (({'status': 'staged'}, 1),
                                ({'status': 'committed', 'partition': 'ota_1', 'reboot': True}, 1),
                                ({'status': 'staged', 'partition': 'ota_1', 'reboot': False}, 0)):
            with self.subTest(reply=reply), patch.dict(ota.os.environ, {'BMC_TOKEN': 'secret'}), \
                    patch.object(ota.Path, 'read_bytes', return_value=b'\xe9' + bytes(287)), \
                    patch.object(ota, 'request_json', return_value=reply) as request, \
                    patch.object(ota, 'control') as command, \
                    contextlib.redirect_stdout(io.StringIO()) as output, \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ota.main(['--url', 'http://device', '--firmware', 'app.bin']), expected)
                self.assertEqual(request.call_count, 1)
                command.assert_not_called()
                self.assertEqual('已暂存至' in output.getvalue(), expected == 0)
                if expected == 0:
                    self.assertIn('未切换启动槽', output.getvalue())

    def test_explicit_commit_and_reboot_order_and_failure(self):
        for fail in (False, True):
            with patch.dict(ota.os.environ, {'BMC_TOKEN': 'secret'}), \
                    patch.object(ota, 'request_json') as request, \
                    patch.object(ota, 'control', side_effect=RuntimeError('failed') if fail else None) as command, \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ota.main(['--url', 'http://device', '--commit', '--reboot']), int(fail))
                self.assertEqual([call.args[2] for call in command.call_args_list],
                                 ['ota-finish'] if fail else ['ota-finish', 'ota-reboot'])
                request.assert_not_called()


if __name__ == '__main__':
    unittest.main()
