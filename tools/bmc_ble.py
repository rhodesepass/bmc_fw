#!/usr/bin/env python3
"""EPASS-BMC BLE debug CLI. Dependencies: uv sync（见同目录 pyproject.toml）。"""

import argparse
import asyncio
import glob
import os
from pathlib import Path
import shlex
import sys
import time

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
UART_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
UART_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
CONTROL = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
LOG = "6e400005-b5a3-f393-e0a9-e50e24dcca9e"
COMMANDS = ("trace-on", "trace-off", "status", "power-status", "hold", "reset", "fel", "c3-download", "rescue-download", "boot", "uart-replay", "log-replay")


async def command(client, request, timeout=5.0):
    if request not in COMMANDS and not (request.startswith("trace ") and request[6:].isdigit()):
        raise ValueError(f"unsupported command: {request}")
    # A successful ATT write confirms queuing; the worker reports completion by read.
    await client.write_gatt_char(CONTROL, request.encode("ascii"), response=True)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = bytes(await client.read_gatt_char(CONTROL)).decode("utf-8", errors="replace")
        except Exception:
            if request in ("c3-download", "rescue-download") and not client.is_connected:
                return "DISCONNECTED download requested; verify ROM using esptool"
            raise
        if result.startswith("OK "):
            return result
        if result.startswith("ERROR "):
            raise RuntimeError(result)
        if not result.startswith("BUSY "):
            raise RuntimeError(f"unexpected BMC response: {result!r}")
        await asyncio.sleep(0.05)
    raise TimeoutError(f"BMC command timed out: {request}")


async def write_uart(client, data):
    services = getattr(client, "services", None)
    characteristic = services.get_characteristic(UART_RX) if services else None
    # BlueZ may not know client.mtu_size yet; the characteristic exposes a safe ATT payload size.
    payload_size = getattr(characteristic, "max_write_without_response_size", 20)
    chunk_size = min(244, payload_size) if isinstance(payload_size, int) and payload_size > 0 else 20
    for offset in range(0, len(data), chunk_size):
        await client.write_gatt_char(UART_RX, data[offset:offset + chunk_size], response=True)


async def run_process(*argv, timeout=20):
    proc = await asyncio.create_subprocess_exec(
        *map(str, argv), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
    )
    try:
        output, _ = await asyncio.wait_for(proc.communicate(), timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.communicate()
        raise TimeoutError(f"command timed out: {shlex.join(map(str, argv))}") from None
    return proc.returncode, output.decode(errors="replace")


async def wait_fel(xfel, timeout=15):
    deadline = time.monotonic() + timeout
    last_output = ""
    while time.monotonic() < deadline:
        rc, last_output = await run_process(xfel, "version", timeout=3)
        if rc == 0 and any(marker in last_output.upper() for marker in ("D1", "F133", "0000000018590000")):
            return last_output.strip()
        await asyncio.sleep(0.25)
    raise TimeoutError(f"D1s FEL not found: {last_output.strip()}")


def is_uopbridge_port(port):
    tty = Path(port).resolve().name
    device = Path("/sys/class/tty") / tty / "device"
    for parent in device.resolve().parents:
        try:
            vid = (parent / "idVendor").read_text().strip().lower()
            pid = (parent / "idProduct").read_text().strip().lower()
        except OSError:
            continue
        return (vid, pid) == ("1209", "d1c3")
    return False


async def wait_bridge(port=None, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = [port] if port else glob.glob("/dev/serial/by-id/*D1S-C3-0001*")
        for candidate in candidates:
            if candidate and os.path.exists(candidate) and is_uopbridge_port(candidate):
                return candidate
        await asyncio.sleep(0.25)
    raise TimeoutError("uopbridge CDC not found; supply --port if no /dev/serial/by-id link exists")


def bridge_script(args):
    bridge = Path(args.uopbridge).expanduser().resolve()
    if not bridge.is_file():
        raise FileNotFoundError(f"uopbridge script not found: {bridge}")
    return bridge


async def load_uopbridge(args, bridge):
    print(await wait_fel(args.xfel))
    rc, output = await run_process("bash", bridge, timeout=60)
    print(output, end="" if output.endswith("\n") else "\n")
    if rc:
        raise RuntimeError(f"uopbridge loader failed with exit status {rc}")
    port = await wait_bridge(args.port)
    print(f"uopbridge CDC: {port}")
    return port


def print_download_command(port):
    print("Run from epass_bmc (no flash has been written):")
    print(f"python -m esptool --chip esp32c3 --port {shlex.quote(port)} --before no-reset --after no-reset chip-id")
    print("For flashing, use build/flash_args with the same --before no-reset option.")


async def prepare_download(client, args):
    bridge = bridge_script(args)
    print(await command(client, "fel"))
    port = await load_uopbridge(args, bridge)
    print(await command(client, "c3-download"))
    print_download_command(port)


async def rescue_download(client, args):
    bridge = bridge_script(args)
    print(await command(client, "rescue-download"))
    # Let the delayed C3 reset finish before accepting a pre-existing FEL instance.
    await asyncio.sleep(1.5)
    port = await load_uopbridge(args, bridge)
    print_download_command(port)


async def stream(client, kind, replay=True, duration=None, output=None):
    target = UART_TX if kind == "console" else LOG
    sink = open(output, "ab", buffering=0) if output else sys.stdout.buffer
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    stdin_installed = False
    queue = asyncio.Queue(maxsize=64)

    def receive(_sender, data):
        sink.write(data)
        sink.flush()

    def on_stdin():
        data = os.read(sys.stdin.fileno(), 244)
        if not data:
            stop.set()
        elif not queue.full():
            queue.put_nowait(data)
        else:
            print("UART input queue full", file=sys.stderr)
            stop.set()

    async def input_writer():
        while True:
            data = await queue.get()
            await write_uart(client, data)

    async def wait_disconnect():
        while client.is_connected:
            await asyncio.sleep(0.1)
        stop.set()

    tasks = []
    try:
        await client.start_notify(target, receive)
        if replay:
            print(await command(client, "uart-replay" if kind == "console" else "log-replay"), file=sys.stderr)
        if kind == "console" and sys.stdin.isatty():
            loop.add_reader(sys.stdin.fileno(), on_stdin)
            stdin_installed = True
            tasks.append(asyncio.create_task(input_writer()))
            print("D1s console: line input enabled, Ctrl-C exits", file=sys.stderr)
        tasks.extend([asyncio.create_task(wait_disconnect()), asyncio.create_task(stop.wait())])
        done, _ = await asyncio.wait(tasks, timeout=duration, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        if stdin_installed:
            loop.remove_reader(sys.stdin.fileno())
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if client.is_connected:
            await client.stop_notify(target)
        if output:
            sink.close()


async def main_async(args):
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        raise RuntimeError("缺少 bleak：在 epass_bmc/tools 下执行 uv sync") from None
    if args.action == "scan":
        devices = await BleakScanner.discover(timeout=args.timeout, return_adv=True)
        for device, adv in devices.values():
            if SERVICE in [uuid.lower() for uuid in adv.service_uuids] or (device.name or "").startswith("EPASS-BMC"):
                print(f"{device.address}\t{device.name}\tRSSI={adv.rssi}")
        return
    address = args.address
    if not address:
        address = await BleakScanner.find_device_by_filter(
            lambda _dev, adv: SERVICE in [uuid.lower() for uuid in adv.service_uuids], timeout=args.timeout
        )
        if address is None:
            raise RuntimeError("EPASS-BMC not found; use scan and --address")
    async with BleakClient(address, timeout=args.timeout) as client:
        if args.action == "terminal":
            from bmc_terminal import terminal
            await terminal(client, args)
        elif args.action == "trace":
            for index in range(args.start, min(args.start + args.count, 256), 8):
                result = await command(client, f"trace {index}")
                print(result)
                if result.strip() == "OK end": break
        elif args.action == "rescue-download":
            await rescue_download(client, args)
        elif args.action == "prepare-download":
            await prepare_download(client, args)
        elif args.action in COMMANDS:
            print(await command(client, args.action))
        else:
            await stream(client, args.action, replay=not args.no_replay, duration=args.duration, output=args.output)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="BLE address; otherwise discover the NUS service")
    parser.add_argument("--timeout", type=float, default=10, help="BLE discovery/connect timeout")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("scan")
    terminal = sub.add_parser("terminal", help="Raw APP terminal; Ctrl-] ? for local controls")
    terminal.add_argument("--no-replay", action="store_true")
    terminal.add_argument("--duration", type=float)
    terminal.add_argument("--bmc-logs", action="store_true", help="Also display BMC logs on stderr")
    terminal.add_argument("--log-dir", help="Save APP and BMC bytes to separate timestamped files")
    trace = sub.add_parser("trace")
    trace.add_argument("--start", type=int, default=0)
    trace.add_argument("--count", type=int, default=256)
    for name in COMMANDS:
        if name != "rescue-download":
            sub.add_parser(name)
    for name in ("console", "logs"):
        item = sub.add_parser(name)
        item.add_argument("--no-replay", action="store_true")
        item.add_argument("--duration", type=float, help="Stop after this many seconds")
        item.add_argument("--output", help="Append raw bytes to a file instead of stdout")
    for name in ("prepare-download", "rescue-download"):
        item = sub.add_parser(name)
        item.add_argument("--uopbridge", default=str(Path(__file__).resolve().parent / "../../uopbridge/boot.sh"))
        item.add_argument("--xfel", default="xfel")
        item.add_argument("--port", help="Expected uopbridge CDC path")
    return parser.parse_args(argv)


if __name__ == "__main__":
    try:
        asyncio.run(main_async(parse_args()))
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
