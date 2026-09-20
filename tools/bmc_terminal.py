"""Raw POSIX terminal for the EPASS BLE UART."""

import asyncio
from contextlib import ExitStack
from datetime import datetime
import os
from pathlib import Path
import stat
import sys
import termios
import tty

from bmc_ble import UART_TX, LOG, command, write_uart

HELP = "Ctrl-] 然后按 q 退出、? 帮助、s 状态、r 复位 APP、b 正常启动、f 进入 FEL、h 保持 APP 复位、l 切换 BMC 日志；连续 Ctrl-] 发送原字符。Ctrl-C/Ctrl-D 发送给 APP。"
KEYS = {ord("s"): "status", ord("r"): "reset", ord("b"): "boot", ord("f"): "fel", ord("h"): "hold"}


class EscapeParser:
    def __init__(self):
        self.pending = False

    def feed(self, data):
        events = []
        uart = bytearray()
        for byte in data:
            if self.pending:
                self.pending = False
                if uart:
                    events.append(("uart", bytes(uart)))
                    uart.clear()
                if byte == 0x1d:
                    uart.append(byte)
                elif byte in KEYS:
                    events.append(("command", KEYS[byte]))
                elif byte in (ord("q"), ord("?"), ord("l")):
                    events.append(("local", chr(byte)))
                    if byte == ord("q"):
                        break
                else:
                    events.append(("local", "?"))
            elif byte == 0x1d:
                self.pending = True
            else:
                uart.append(byte)
        if uart:
            events.append(("uart", bytes(uart)))
        return events


async def terminal(client, args, *, stdin=None, stdout=None, stderr=None):
    stdin = sys.stdin if stdin is None else stdin
    stdout = sys.stdout.buffer if stdout is None else stdout
    stderr = sys.stderr if stderr is None else stderr
    fd = stdin.fileno()
    interactive = os.isatty(fd)
    saved = termios.tcgetattr(fd) if interactive else None
    loop = asyncio.get_running_loop()
    queue = asyncio.Queue(maxsize=128)
    parser = EscapeParser()
    stop = asyncio.Event()
    show_bmc = args.bmc_logs
    installed = False
    tasks = []
    notified = []

    def status(message):
        stderr.write("\r\n[BMC terminal] " + message + "\r\n")
        stderr.flush()

    with ExitStack() as stack:
        app_log = bmc_log = None
        if args.log_dir:
            directory = Path(args.log_dir).expanduser()
            directory.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            app_log = stack.enter_context(open(directory / f"{stamp}-app.log", "ab", buffering=0))
            bmc_log = stack.enter_context(open(directory / f"{stamp}-bmc.log", "ab", buffering=0))
            status(f"日志目录：{directory.resolve()}，前缀：{stamp}")

        def receive_app(_sender, data):
            stdout.write(data)
            stdout.flush()
            if app_log:
                app_log.write(data)

        def receive_bmc(_sender, data):
            if bmc_log:
                bmc_log.write(data)
            if show_bmc:
                status("日志 " + bytes(data).decode("utf-8", errors="replace").rstrip())

        def on_stdin():
            data = os.read(fd, 244)
            if not data:
                if installed:
                    loop.remove_reader(fd)
                events = [("eof", None)]
            else:
                events = parser.feed(data) if interactive else [("uart", data)]
            for event in events:
                try:
                    queue.put_nowait(event)
                except asyncio.QueueFull:
                    status("输入超过 BLE 发送速度，终止连接；请缩小粘贴内容。")
                    stop.set()
                    break

        async def writer():
            nonlocal show_bmc
            while True:
                kind, value = await queue.get()
                if kind == "uart":
                    await write_uart(client, value)
                elif kind == "command":
                    status(await command(client, value))
                elif kind == "eof" or value == "q":
                    return
                elif value == "l":
                    show_bmc = not show_bmc
                    status("BMC 日志显示：" + ("开" if show_bmc else "关"))
                else:
                    status(HELP)

        async def disconnect():
            while client.is_connected:
                await asyncio.sleep(0.1)
            status("BLE 已断开")

        async def file_input():
            while not stop.is_set():
                data = os.read(fd, 244)
                await queue.put(("uart", data) if data else ("eof", None))
                if not data:
                    return

        try:
            for uuid, callback in ((UART_TX, receive_app), (LOG, receive_bmc)):
                await client.start_notify(uuid, callback)
                notified.append(uuid)
            if not args.no_replay:
                status(await command(client, "uart-replay"))
                if show_bmc or bmc_log:
                    status(await command(client, "log-replay"))
            if interactive:
                tty.setraw(fd)
                status(HELP)
            if stat.S_ISREG(os.fstat(fd).st_mode):
                tasks.append(asyncio.create_task(file_input()))
            else:
                loop.add_reader(fd, on_stdin)
                installed = True
            writer_task = asyncio.create_task(writer())
            tasks.extend((writer_task, asyncio.create_task(disconnect()), asyncio.create_task(stop.wait())))
            done, _ = await asyncio.wait(tasks[-3:], timeout=args.duration, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            if installed:
                loop.remove_reader(fd)
            if saved is not None:
                termios.tcsetattr(fd, termios.TCSANOW, saved)
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            for uuid in notified:
                if client.is_connected:
                    await client.stop_notify(uuid)
