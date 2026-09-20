#!/usr/bin/env python3
"""BLE provisioning and persistent HTTP/BLE OTA uploader."""
import argparse
import asyncio
import hashlib
import http.client
import json
import getpass
from urllib.parse import urlsplit
import secrets
import struct
import time
import zlib
from pathlib import Path

MAGIC = 0x31555042
UUID = '6e40000{}-b5a3-f393-e0a9-e50e24dcca9e'
CONTROL, UPLOAD, STATUS = (UUID.format(n) for n in (4, 6, 7))
TARGETS = {'boot': 1, 'rootfs': 2, 'uboot': 3, 'rescue': 4, 'verify': 5, 'touch': 6, 'bmc': 7}
LIMITS = {1: 10 << 20, 2: 28 << 20, 3: 1 << 20, 4: 4 << 20, 5: 32 << 20, 6: 63488, 7: 0x180000}


def frame(op, session, seq, offset=0, payload=b''):
    if not session or not seq or len(payload) > 2016:
        raise ValueError('无效 session、seq 或载荷长度')
    body = struct.pack('<8I', MAGIC, op, session, seq, offset, len(payload), 0, 0)
    body += payload + bytes(2016 - len(payload))
    return body + struct.pack('<I', zlib.crc32(body))


def fragments(data, att_payload=20):
    if not 5 <= att_payload <= 512:
        raise ValueError('ATT 载荷必须在 5..512 字节之间，且不超过协商 MTU - 3')
    for offset in range(0, len(data), att_payload - 4):
        yield struct.pack('<I', offset) + data[offset:offset + att_payload - 4]


def status(data):
    if len(data) != 32:
        raise ValueError('OTA 状态长度错误')
    values = struct.unpack('<8I', data)
    if values[0] != MAGIC:
        raise ValueError('OTA 状态 magic 错误')
    result = dict(zip(('magic', 'session', 'seq', 'consumed', 'durable', 'state', 'error', 'stage'), values))
    if result['state'] > 5 or result['stage'] > 3 or result['durable'] > result['consumed']:
        raise ValueError('OTA 状态字段错误')
    return result


def validate_image(data, target):
    if target not in LIMITS or not 0 < len(data) <= LIMITS[target]:
        raise ValueError('镜像为空或超出目标大小限制')
    if target == 2 and len(data) % (128 << 10):
        raise ValueError('rootfs UBI 镜像必须按 128 KiB 对齐')
    if target == 6 and len(data) % 4:
        raise ValueError('touch raw 固件必须按 4 字节对齐')
    if target == 7 and (len(data) < 288 or data[0] != 0xe9):
        raise ValueError('bmc 必须是至少 288 字节、首字节为 0xe9 的 ESP app 镜像')


def image_frames(data, target, session):
    validate_image(data, target)
    yield frame(10, session, 1, payload=struct.pack('<II', len(data), target) + hashlib.sha256(data).digest())
    seq = 2
    for offset in range(0, len(data), 2016):
        yield frame(11, session, seq, offset, data[offset:offset + 2016])
        seq += 1
    yield frame(12, session, seq, len(data))


class Uploader:
    def __init__(self, client, timeout=60, att_payload=20, progress=print):
        if not 5 <= att_payload <= 244:
            raise ValueError('BMC ATT 载荷必须在 5..244 字节之间')
        self.client, self.timeout, self.att_payload, self.progress = client, timeout, att_payload, progress

    async def read(self):
        value = status(await self.client.read_gatt_char(STATUS))
        if value['error'] or value['state'] == 5:
            raise RuntimeError(f"设备错误 {value['error']}，stage={value['stage']} seq={value['seq']}")
        return value

    async def wait_stage(self, stage):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            if (await self.read())['stage'] == stage:
                return
            await asyncio.sleep(.1)
        raise TimeoutError(f'等待 stage {stage} 超时')

    async def control(self, command):
        await self.client.write_gatt_char(CONTROL, command.encode(), response=True)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            reply = bytes(await self.client.read_gatt_char(CONTROL)).decode(errors='replace')
            if reply.startswith('OK '):
                return
            if reply.startswith(('ERR', 'ERROR')):
                raise RuntimeError(reply)
            await asyncio.sleep(.1)
        raise TimeoutError('控制命令回复超时')

    async def send(self, packet):
        _, op, session, seq, offset, length, _, _ = struct.unpack_from('<8I', packet)
        deadline = time.monotonic() + self.timeout
        sent = False
        while time.monotonic() < deadline:
            try:
                current = await self.read()
                if current['session'] == session and current['seq'] == seq:
                    expected = offset + (length if op == 11 else 0)
                    if current['consumed'] != expected or (op == 12 and current['state'] != 4):
                        raise RuntimeError('ACK 进度或 END 完成状态不符')
                    self.progress(f"已接收 {current['consumed']} B / 已持久化 {current['durable']} B")
                    return current
                if current['session'] == session and current['seq'] > seq:
                    raise RuntimeError('设备序号超过当前帧，停止以避免混用会话')
                if not sent and current['state'] != 2:
                    for part in fragments(packet, self.att_payload):
                        await self.client.write_gatt_char(UPLOAD, part, response=True)
                    sent = True
                await asyncio.sleep(.05)
            except Exception as exc:
                if self.client.is_connected:
                    raise
                self.progress(f'连接中断，重连查询同一会话：{exc}')
                await asyncio.sleep(.5)
                await self.client.connect()
                current = await self.read()
                if current['session'] != session:
                    raise RuntimeError('设备会话丢失；重新运行并从 BEGIN/偏移 0 开始') from exc
                sent = current['state'] == 2
        raise TimeoutError(f'等待 session={session:08x} seq={seq} ACK 超时；未确认成功')

    async def upload(self, data, target):
        session = secrets.randbelow(0xffffffff) + 1
        for packet in image_frames(data, target, session):
            final = await self.send(packet)
        if target not in (4, 5) and final['durable'] != len(data):
            raise RuntimeError('END 持久化字节数不符')
        self.progress('END 已确认，SHA256 校验通过' + ('；RAM/verify 目标未持久化' if target in (4, 5) else ''))



def wifi_command(ssid, password):
    ssid_bytes, password_bytes = ssid.encode('utf-8'), password.encode('utf-8')
    if b'\0' in ssid_bytes or b'\0' in password_bytes:
        raise ValueError('SSID 和密码不能包含 NUL')
    if not 1 <= len(ssid_bytes) <= 32 or (password_bytes and not 8 <= len(password_bytes) <= 63):
        raise ValueError('SSID 必须为 1..32 UTF-8 字节，密码为空或 8..63 字节')
    return f"wifi-sta {ssid_bytes.hex()} {password_bytes.hex() or '-'}"


async def ble_command(client, command, timeout):
    await client.write_gatt_char(CONTROL, command.encode(), response=True)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        reply = bytes(await client.read_gatt_char(CONTROL)).decode('utf-8')
        if reply.startswith('OK '):
            return reply[3:].strip()
        if reply.startswith(('ERR', 'ERROR')):
            raise RuntimeError(reply)
        await asyncio.sleep(.1)
    raise TimeoutError('BLE 控制回复超时')


async def provision_wifi(client, args):
    command = 'wifi-ap' if args.wifi_ap else wifi_command(args.wifi_sta, args.wifi_password or '')
    # 配网命令可能达到 200 字节，先确认链路能容纳整条命令。
    acquire_mtu = getattr(getattr(client, '_backend', None), '_acquire_mtu', None)
    if acquire_mtu is not None:
        await acquire_mtu()
    if len(command.encode()) > client.mtu_size - 3:
        raise ValueError('协商 BLE MTU 不足以容纳配网命令')
    await ble_command(client, command, args.timeout)
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        info = json.loads(await ble_command(client, 'wifi-status', args.timeout))
        if info.get('state') == 'ready':
            if not info.get('url') or not info.get('token'):
                raise RuntimeError('Wi-Fi 就绪回复缺少 URL 或令牌')
            print(f"Wi-Fi {info['mode']} 已就绪，SSID={info.get('ssid', '')}，地址={info['url']}")
            if args.wifi_ap:
                print(f"请将此电脑连接到上述热点，WPA2 密码：{info.get('ap_password', '')}")
                await asyncio.to_thread(input, '连接完成后按 Enter 开始传输：')
            return info['url'], info['token']
        if info.get('state') == 'error' or info.get('error'):
            raise RuntimeError(f"Wi-Fi 连接失败：{info.get('error', 'unknown')}")
        await asyncio.sleep(.5)
    raise TimeoutError('Wi-Fi 等待连接超时')


class HttpStatusError(RuntimeError):
    def __init__(self, code):
        self.code = code
        super().__init__(f'设备 HTTP 错误 {code}；更新未确认')


class HttpUploader(Uploader):
    def __init__(self, url, token, timeout=60, progress=print):
        endpoint = urlsplit(url)
        if endpoint.scheme != 'http' or not endpoint.hostname or endpoint.username or endpoint.password or endpoint.path not in ('', '/') or endpoint.query or endpoint.fragment:
            raise ValueError('设备 URL 必须为 http://主机[:端口]')
        if not token or any(c in token for c in '\r\n'):
            raise ValueError('需要有效会话令牌')
        self.connection = http.client.HTTPConnection(endpoint.hostname, endpoint.port or 80, timeout=timeout)
        self.token, self.timeout, self.progress = token, timeout, progress

    def close(self):
        self.connection.close()

    def _request(self, method, path, body=None):
        try:
            self.connection.request(method, path, body=body, headers={
                'Authorization': 'Bearer ' + self.token,
                'Content-Type': 'application/octet-stream' if path.endswith('/ota') else 'text/plain',
            })
            reply = self.connection.getresponse()
            data = reply.read(8193)
            if len(data) > 8192:
                raise RuntimeError('HTTP 回复过长')
            if not 200 <= reply.status < 300:
                raise HttpStatusError(reply.status)
            return data
        except Exception:
            self.connection.close()
            raise

    async def request(self, method, path, body=None):
        return await asyncio.to_thread(self._request, method, path, body)

    def decode(self, data):
        current = status(data)
        if current['error'] or current['state'] == 5:
            raise RuntimeError(f"设备错误 {current['error']}，stage={current['stage']} seq={current['seq']}")
        return current

    async def read(self):
        return self.decode(await self.request('GET', '/v1/status'))

    async def control(self, command):
        reply = (await self.request('POST', '/v1/control', command.encode())).decode()
        if not reply.startswith('OK '):
            raise RuntimeError('控制命令未确认：' + reply)

    def check_prefix(self, packets, current, complete=False):
        headers = [struct.unpack_from('<8I', packet) for packet in packets]
        first, last = headers[0], headers[-1]
        if current['session'] != first[2] or not first[3] - 1 <= current['seq'] <= last[3]:
            raise RuntimeError('HTTP ACK 会话或序号不符；停止传输')
        count = current['seq'] - first[3] + 1
        if complete and count != len(packets):
            raise RuntimeError('HTTP 回复没有确认整个批次')
        header = headers[count - 1] if count else first
        expected = header[4] + (header[5] if count and header[1] == 11 else 0)
        if current['consumed'] != expected or (count and header[1] == 12 and current['state'] != 4):
            raise RuntimeError('ACK 进度或 END 完成状态不符')
        return count

    async def send(self, packet):
        return await self.send_batch([packet])

    async def send_batch(self, packets):
        if not 1 <= len(packets) <= 16:
            raise ValueError('HTTP 批次必须为 1..16 帧')
        first = struct.unpack_from('<8I', packets[0])
        for index, packet in enumerate(packets):
            header = struct.unpack_from('<8I', packet)
            if len(packet) != 2052 or header[2:4] != (first[2], first[3] + index) or (len(packets) > 1 and header[1] != 11):
                raise ValueError('HTTP 批次必须为同会话连续 DATA 帧')
        deadline, pending, recovering = time.monotonic() + self.timeout * 3, packets, False
        confirmed = 0
        for attempt in range(3):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.connection.timeout = min(self.timeout, remaining)
            if self.connection.sock is not None:
                self.connection.sock.settimeout(self.connection.timeout)
            try:
                if recovering:
                    current = await self.read()
                    count = self.check_prefix(packets, current)
                    if count < confirmed:
                        raise RuntimeError('HTTP ACK 进度倒退；停止传输')
                    confirmed = count
                    if count == len(packets):
                        self.progress(f"已接收 {current['consumed']} B / 已持久化 {current['durable']} B")
                        return current
                    pending = packets[count:]
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.connection.timeout = min(self.timeout, remaining)
                if self.connection.sock is not None:
                    self.connection.sock.settimeout(self.connection.timeout)
                data = await self.request('POST', '/v1/channels/ota', b''.join(pending))
            except (OSError, http.client.HTTPException, HttpStatusError) as exc:
                if isinstance(exc, HttpStatusError) and exc.code != 504:
                    raise
                recovering = True
                if attempt == 2:
                    raise TimeoutError('HTTP 批次重试耗尽；尚未确认，设备可能仍在处理') from exc
                self.progress('HTTP 响应丢失，查询已确认进度后重传未确认帧')
                await asyncio.sleep(min(.25, max(0, deadline - time.monotonic())))
                continue
            current = self.decode(data)
            self.check_prefix(packets, current, complete=True)
            self.progress(f"已接收 {current['consumed']} B / 已持久化 {current['durable']} B")
            return current
        raise TimeoutError('HTTP 批次超时；尚未确认，设备可能仍在处理')

    async def upload(self, data, target):
        session = secrets.randbelow(0xffffffff) + 1
        batch = []
        final = None
        for packet in image_frames(data, target, session):
            op = struct.unpack_from('<I', packet, 4)[0]
            if op == 11:
                batch.append(packet)
                if len(batch) == 16:
                    await self.send_batch(batch)
                    batch = []
            else:
                if batch:
                    await self.send_batch(batch)
                    batch = []
                final = await self.send(packet)
        if target not in (4, 5) and final['durable'] != len(data):
            raise RuntimeError('END 持久化字节数不符')
        self.progress('END 已确认，SHA256 校验通过' + ('；RAM/verify 目标未持久化' if target in (4, 5) else ''))


async def update_transaction(uploader, args, uboot, images):
    bmc = [(data, target) for data, target in images if target == 7]
    other = [(data, target) for data, target in images if target != 7]
    for data, target in bmc:
        await uploader.control('ota-bmc')
        await uploader.wait_stage(3)
        await uploader.upload(data, target)
    if other:
        await uploader.control('ota')
        await uploader.wait_stage(1)
        await uploader.upload(uboot, 4)
        await uploader.wait_stage(2)
        for data, target in other:
            await uploader.upload(data, target)
    if images[0][1] != 5:
        await uploader.control('ota-finish')
        if args.boot:
            await uploader.control('ota-reboot' if bmc else 'boot')
        else:
            print('全部更新已提交；通过 ' + ('ota-reboot 命令重启 ESP' if bmc else 'boot 命令启动系统'))

def load_images(args):
    selected = []
    if args.target or args.image:
        if not args.target or not args.image:
            raise ValueError('target 与 image 必须一起提供')
        selected.append((args.target, args.image))
    for item in args.extra_images:
        name, sep, path = item.partition('=')
        if not sep or name not in ('boot', 'rootfs', 'uboot', 'verify', 'touch', 'bmc') or not path:
            raise ValueError('--image 格式为 target=path')
        selected.append((name, Path(path)))
    if not selected or len({name for name, _ in selected}) != len(selected):
        raise ValueError('必须选择镜像，且目标不能重复')
    if any(name == 'verify' for name, _ in selected) and (len(selected) != 1 or args.boot):
        raise ValueError('verify 必须单独运行，不能提交或启动')
    if args.sha256 and len(selected) != 1:
        raise ValueError('--sha256 仅适用于单镜像')
    images = []
    for name, path in selected:
        target = TARGETS[name]
        if not 0 < path.stat().st_size <= LIMITS[target]:
            raise ValueError(f'{name} 镜像大小超出目标限制')
        data = path.read_bytes()
        validate_image(data, target)
        digest = hashlib.sha256(data).hexdigest()
        if args.sha256 and digest.lower() != args.sha256.lower():
            raise ValueError('文件 SHA256 与预期不符')
        print(f'{name}: {len(data)} B SHA256={digest}')
        images.append((data, target))
    return images


async def run(args):
    images = load_images(args)
    uboot = None
    if any(target != 7 for _, target in images):
        if not args.uboot_fit:
            raise ValueError('必须提供 --uboot-fit 完整 U-Boot FIT')
        if not 0 < args.uboot_fit.stat().st_size <= LIMITS[4]:
            raise ValueError('U-Boot FIT 大小必须为 1 字节到 4 MiB')
        uboot = args.uboot_fit.read_bytes()
        validate_image(uboot, 4)
    url, token = getattr(args, 'url', None), getattr(args, 'token', None)
    wants_wifi = getattr(args, 'wifi_sta', None) is not None or getattr(args, 'wifi_ap', False)
    if (wants_wifi or not url) and not getattr(args, 'address', None):
        raise ValueError('BLE 更新或 Wi-Fi 配网必须用 --address 明确指定目标设备；复用 --url HTTP 会话时可省略')
    if wants_wifi:
        from bleak import BleakClient
        async with BleakClient(args.address, timeout=20) as client:
            url, token = await provision_wifi(client, args)
    if url:
        token = token or getpass.getpass('Wi-Fi 会话令牌：')
        uploader = HttpUploader(url, token, args.timeout)
        try:
            await update_transaction(uploader, args, uboot, images)
        finally:
            uploader.close()
        return
    from bleak import BleakClient
    async with BleakClient(args.address, timeout=20) as client:
        uploader = Uploader(client, args.timeout, args.att_payload)
        if args.att_payload > 20:
            # BlueZ reports 23 until Bleak queries the negotiated MTU explicitly.
            acquire_mtu = getattr(getattr(client, '_backend', None), '_acquire_mtu', None)
            if acquire_mtu is not None:
                await acquire_mtu()
            maximum = client.mtu_size - 3
            if args.att_payload > maximum:
                raise ValueError(f'ATT 载荷 {args.att_payload} 超过协商 MTU 允许的 {maximum}；尚未开始 OTA')
        await update_transaction(uploader, args, uboot, images)


def main():
    parser = argparse.ArgumentParser(description='EPASS Wi-Fi / BLE OTA：仅 END ACK 表示成功；断电后从头重刷')
    parser.add_argument('target', nargs='?', choices=('boot', 'rootfs', 'uboot', 'verify', 'touch', 'bmc'))
    parser.add_argument('image', nargs='?', type=Path)
    parser.add_argument('--image', dest='extra_images', action='append', default=[], help='可重复：boot=boot.bin / rootfs=rootfs.ubi / uboot=u-boot.img / touch=touch.bin / bmc=epass_bmc.bin')
    parser.add_argument('--uboot-fit', '--rescue-fit', dest='uboot_fit', type=Path, help='完整 U-Boot FIT；包含 D1s/CH32 目标时必填，仅 bmc 不需要')
    parser.add_argument('--address', help='BLE 更新或 Wi-Fi 配网的目标地址；纯 HTTP --url 可省略')
    route = parser.add_mutually_exclusive_group()
    route.add_argument('--wifi-sta', metavar='SSID', help='通过 BLE 接入现有 Wi-Fi，然后用 HTTP 上传')
    route.add_argument('--wifi-ap', action='store_true', help='通过 BLE 启动设备热点，然后用 HTTP 上传')
    route.add_argument('--url', help='复用已建立的 Wi-Fi 会话，例如 http://192.168.4.1')
    parser.add_argument('--wifi-password', help='现有 Wi-Fi 密码；未提供时交互输入')
    parser.add_argument('--token', help='复用会话的令牌；建议省略以交互输入，避免保留在 shell 历史')
    parser.add_argument('--boot', action='store_true', help='更新提交后启动系统（verify 不允许）')
    parser.add_argument('--sha256', help='预期镜像 SHA256')
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--att-payload', type=int, default=20, help='5..244，且不超过协商 MTU - 3；默认兼容 20')
    args = parser.parse_args()
    if not args.uboot_fit and (args.target not in (None, 'bmc') or any(not item.startswith('bmc=') for item in args.extra_images)):
        parser.error('非 bmc 目标必须提供 --uboot-fit')
    if args.boot and args.target == 'verify':
        parser.error('verify 不能提交或启动')
    if args.timeout <= 0 or not 5 <= args.att_payload <= 244:
        parser.error('timeout 必须为正数，att-payload 必须在 5..244')
    if args.wifi_password is not None and args.wifi_sta is None:
        parser.error('--wifi-password 需要 --wifi-sta')
    if args.token and not args.url:
        parser.error('--token 需要 --url')
    try:
        if args.wifi_sta is not None and args.wifi_password is None:
            args.wifi_password = getpass.getpass('现有 Wi-Fi 密码（开放网络留空）：')
        asyncio.run(run(args))
    except (Exception, KeyboardInterrupt) as exc:
        parser.exit(1, f'更新未完成：{exc}\n')


if __name__ == '__main__':
    main()
