import asyncio
import io
import os
from pathlib import Path
import pty
import sys
import tempfile
import termios
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bmc_ble import UART_RX, UART_TX, LOG, CONTROL, parse_args
from bmc_terminal import EscapeParser, terminal


class Client:
    mtu_size = 23
    is_connected = True

    def __init__(self):
        self.callbacks = {}
        self.writes = []
        self.fail = False

    async def start_notify(self, uuid, callback):
        self.callbacks[uuid] = callback

    async def stop_notify(self, uuid):
        del self.callbacks[uuid]

    async def write_gatt_char(self, uuid, data, response):
        if self.fail:
            raise RuntimeError("write failed")
        self.writes.append((uuid, bytes(data)))

    async def read_gatt_char(self, uuid):
        return b"OK status"


def args(**kw):
    return SimpleNamespace(**dict(no_replay=True, duration=2, bmc_logs=False, log_dir=None) | kw)


class Tests(unittest.IsolatedAsyncioTestCase):
    def test_escape_boundaries(self):
        parser = EscapeParser()
        self.assertEqual(parser.feed(b"abc\x03\x1d"), [("uart", b"abc\x03")])
        self.assertEqual(parser.feed(b"s\x1d\x1d\x1dqdiscard"), [("command", "status"), ("uart", b"\x1d"), ("local", "q")])

    def test_legacy_arguments(self):
        self.assertEqual(parse_args(["console", "--output", "x"]).output, "x")
        self.assertTrue(parse_args(["terminal", "--bmc-logs"]).bmc_logs)

    async def test_pty_raw_and_log_files(self):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        client = Client()
        out = io.BytesIO()
        with os.fdopen(slave, "rb", buffering=0) as stdin, tempfile.TemporaryDirectory() as directory:
            task = asyncio.create_task(terminal(client, args(log_dir=directory), stdin=stdin, stdout=out, stderr=io.StringIO()))
            await asyncio.sleep(.03)
            self.assertFalse(termios.tcgetattr(slave)[3] & termios.ISIG)
            client.callbacks[UART_TX](None, b"APP\xff\r\n")
            client.callbacks[LOG](None, b"BMC\r\n")
            os.write(master, b"uname\r\x03\x04\x1ds\x1dq")
            await task
            self.assertEqual(termios.tcgetattr(slave), before)
            self.assertEqual(b"".join(data for uuid, data in client.writes if uuid == UART_RX), b"uname\r\x03\x04")
            self.assertIn((CONTROL, b"status"), client.writes)
            self.assertEqual(out.getvalue(), b"APP\xff\r\n")
            self.assertEqual(next(Path(directory).glob("*-app.log")).read_bytes(), out.getvalue())
            self.assertEqual(next(Path(directory).glob("*-bmc.log")).read_bytes(), b"BMC\r\n")
            self.assertFalse(client.callbacks)
        os.close(master)

    async def test_restore_after_write_error(self):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        client = Client()
        client.fail = True
        with os.fdopen(slave, "rb", buffering=0) as stdin:
            task = asyncio.create_task(terminal(client, args(), stdin=stdin, stdout=io.BytesIO(), stderr=io.StringIO()))
            await asyncio.sleep(.03)
            os.write(master, b"x")
            with self.assertRaisesRegex(RuntimeError, "write failed"):
                await task
            self.assertEqual(termios.tcgetattr(slave), before)
        os.close(master)

    async def test_disconnect_and_cancel_restore(self):
        for cancel in (False, True):
            master, slave = pty.openpty()
            before = termios.tcgetattr(slave)
            client = Client()
            with os.fdopen(slave, "rb", buffering=0) as stdin:
                task = asyncio.create_task(terminal(client, args(), stdin=stdin, stdout=io.BytesIO(), stderr=io.StringIO()))
                await asyncio.sleep(.03)
                if cancel:
                    task.cancel()
                    with self.assertRaises(asyncio.CancelledError):
                        await task
                else:
                    client.is_connected = False
                    await task
                self.assertEqual(termios.tcgetattr(slave), before)
            os.close(master)

    async def test_non_tty_binary_no_escapes(self):
        client = Client()
        with tempfile.TemporaryFile() as stdin:
            data = b"\x03\x1dq" + bytes(range(256)) * 3
            stdin.write(data)
            stdin.seek(0)
            await terminal(client, args(), stdin=stdin, stdout=io.BytesIO(), stderr=io.StringIO())
        self.assertEqual(b"".join(data for uuid, data in client.writes if uuid == UART_RX), data)
        self.assertTrue(all(len(data) <= 20 for _, data in client.writes))


if __name__ == "__main__":
    unittest.main()
