#!/usr/bin/env python3
"""通过已建立的 Wi-Fi 会话更新 ESP32-C3 本体 app。"""
import argparse
import hashlib
import json
import math
import os
import sys
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def endpoint_and_token(url, environ=None):
    environ = os.environ if environ is None else environ
    parts = urlsplit(url)
    if (parts.scheme != 'http' or not parts.hostname or parts.username
            or parts.password or parts.query
            or parts.path not in ('', '/', '/v1/bmc/firmware')):
        raise ValueError('URL 必须是设备 HTTP 根地址或 /v1/bmc/firmware 地址')
    parts.port
    token = environ.get('BMC_TOKEN') or parse_qs(parts.fragment).get('token', [''])[0]
    if not token or any(ord(char) < 33 or ord(char) > 126 for char in token):
        raise ValueError('请设置 BMC_TOKEN，或在 URL fragment 中提供 #token=...')
    return urlunsplit(('http', parts.netloc, '/v1/bmc/firmware', '', '')), token


def validate_image(data):
    if len(data) < 288 or data[0] != 0xe9:
        raise ValueError('不是有效的 ESP app 镜像：至少 288 字节且首字节必须是 0xe9')
    if len(data) > 0x180000:
        raise ValueError('ESP app 镜像超过当前 1.5 MiB OTA 槽大小')


def request_json(endpoint, token, data=None, timeout=120):
    headers = {'Authorization': 'Bearer ' + token, 'Accept': 'application/json'}
    if data is not None:
        headers.update({'Content-Type': 'application/octet-stream',
                        'Content-Length': str(len(data)),
                        'X-SHA256': hashlib.sha256(data).hexdigest()})
    request = Request(endpoint, data=data, headers=headers,
                      method='GET' if data is None else 'POST')
    # 设备会话令牌不能随重定向或系统代理转发到其它主机。
    opener = build_opener(ProxyHandler({}), NoRedirect())
    try:
        with opener.open(request, timeout=timeout) as response:
            if response.status != 200:
                raise RuntimeError('设备未返回 HTTP 200')
            result = json.loads(response.read(16385))
    except HTTPError as exc:
        exc.close()
        raise RuntimeError(f'设备返回 HTTP {exc.code}；未确认提交成功') from None
    except (URLError, OSError, ValueError) as exc:
        action = '结果未知，请重新连接 BLE 后查询；不会自动重发' if data is not None else '查询失败'
        raise RuntimeError(action) from exc
    if not isinstance(result, dict):
        raise RuntimeError('设备返回的 JSON 不是对象；未确认成功')
    return result


def control(endpoint, token, command, timeout=120):
    parts = urlsplit(endpoint)
    url = urlunsplit((parts.scheme, parts.netloc, '/v1/control', '', ''))
    request = Request(url, data=command.encode(), headers={
        'Authorization': 'Bearer ' + token, 'Content-Type': 'text/plain'}, method='POST')
    opener = build_opener(ProxyHandler({}), NoRedirect())
    try:
        with opener.open(request, timeout=timeout) as response:
            reply = response.read(8193).decode()
            if response.status != 200 or not reply.startswith('OK ') or len(reply) > 8192:
                raise RuntimeError('控制命令未确认：' + command)
    except HTTPError as exc:
        exc.close()
        raise RuntimeError(f'设备返回 HTTP {exc.code}；控制命令未确认：{command}') from None
    except (URLError, OSError, ValueError) as exc:
        raise RuntimeError(f'{command} 结果未知；请查询状态，不会自动重试') from exc


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True, help='BLE 配网返回的设备 URL')
    parser.add_argument('--firmware', type=Path, help='build/epass_bmc.bin；省略时只查询')
    parser.add_argument('--commit', action='store_true', help='所有目标更新完成后显式提交 ota-finish')
    parser.add_argument('--reboot', action='store_true', help='显式发送 ota-reboot；设备要求已提交')
    parser.add_argument('--timeout', type=float, default=120, help='HTTP 超时秒数，默认 120')
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError('timeout 必须大于 0')
        endpoint, token = endpoint_and_token(args.url)
        if args.firmware is None and not (args.commit or args.reboot):
            result = request_json(endpoint, token, timeout=args.timeout)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0
        if args.firmware is not None:
            data = args.firmware.read_bytes()
            validate_image(data)
            print(f'ESP app: {len(data)} 字节，SHA256={hashlib.sha256(data).hexdigest()}', flush=True)
            result = request_json(endpoint, token, data, args.timeout)
            if (result.get('status') != 'staged' or result.get('reboot') is not False
                    or result.get('partition') not in ('ota_0', 'ota_1')):
                raise RuntimeError('响应未确认暂存完成，结果未知；请查询状态')
            print(f"已暂存至 {result['partition']}，未切换启动槽；使用 --commit 显式提交。")
        if args.commit:
            control(endpoint, token, 'ota-finish', args.timeout)
            print('全部更新已提交，未自动重启；可使用 --reboot。')
        if args.reboot:
            control(endpoint, token, 'ota-reboot', args.timeout)
            print('重启命令已确认；尚未验证新固件运行，请重新配网后查询运行槽和版本。')
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f'错误：{exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
