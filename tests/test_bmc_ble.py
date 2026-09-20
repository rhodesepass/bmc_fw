import importlib.util
from pathlib import Path
import types
import unittest
from unittest.mock import AsyncMock, Mock, patch

SPEC = importlib.util.spec_from_file_location("bmc_ble", Path(__file__).parents[1] / "tools/bmc_ble.py")
ble = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ble)


class CommandsTest(unittest.IsolatedAsyncioTestCase):
    def client(self, replies):
        return types.SimpleNamespace(
            write_gatt_char=AsyncMock(), read_gatt_char=AsyncMock(side_effect=replies),
            is_connected=True, mtu_size=23,
        )

    async def test_waits_for_worker_not_att_write(self):
        client = self.client([b"BUSY fel", b"OK FEL active"])
        result = await ble.command(client, "fel")
        self.assertEqual(result, "OK FEL active")
        client.write_gatt_char.assert_awaited_once_with(ble.CONTROL, b"fel", response=True)
        self.assertEqual(client.read_gatt_char.await_count, 2)

    async def test_error_is_reported(self):
        with self.assertRaisesRegex(RuntimeError, "unsafe"):
            await ble.command(self.client([b"ERROR unsafe"]), "reset")

    async def test_download_disconnect_is_request_not_proof(self):
        client = self.client([ConnectionError()])
        client.is_connected = False
        self.assertIn("verify ROM", await ble.command(client, "c3-download"))
        with self.assertRaises(ConnectionError):
            await ble.command(self.client([ConnectionError()]), "fel")

    async def test_uart_respects_mtu_and_preserves_bytes(self):
        client = self.client([])
        payload = bytes(range(256))
        await ble.write_uart(client, payload)
        pieces = [call.args[1] for call in client.write_gatt_char.await_args_list]
        self.assertEqual(b"".join(pieces), payload)
        self.assertTrue(all(len(piece) <= 20 for piece in pieces))
        self.assertTrue(all(call.kwargs == {"response": True} for call in client.write_gatt_char.await_args_list))

    async def test_uart_uses_characteristic_size_without_reading_client_mtu(self):
        class Client:
            @property
            def mtu_size(self):
                raise AssertionError("must not read BlueZ client.mtu_size")

        for advertised, expected in ((20, 20), (182, 182), (512, 244), (0, 20), (None, 20)):
            with self.subTest(advertised=advertised):
                client = Client()
                client.services = Mock()
                client.services.get_characteristic.return_value = types.SimpleNamespace(
                    max_write_without_response_size=advertised)
                client.write_gatt_char = AsyncMock()
                payload = bytes(range(256)) * 3
                await ble.write_uart(client, payload)
                calls = client.write_gatt_char.await_args_list
                self.assertEqual(len(calls[0].args[1]), expected)
                self.assertEqual(b"".join(call.args[1] for call in calls), payload)
                self.assertTrue(all(call.kwargs == {"response": True} for call in calls))
                client.services.get_characteristic.assert_called_once_with(ble.UART_RX)

    async def test_prepare_stops_before_download_on_loader_failure(self):
        args = types.SimpleNamespace(uopbridge=__file__, xfel="xfel", port=None)
        with patch.object(ble, "command", AsyncMock(return_value="OK")) as command, \
             patch.object(ble, "wait_fel", AsyncMock(return_value="D1")), \
             patch.object(ble, "run_process", AsyncMock(return_value=(1, "failed"))), \
             patch.object(ble, "wait_bridge", AsyncMock()) as bridge:
            with self.assertRaisesRegex(RuntimeError, "loader failed"):
                await ble.prepare_download(None, args)
            self.assertEqual([call.args[1] for call in command.await_args_list], ["fel"])
            bridge.assert_not_awaited()

    async def test_prepare_never_downloads_without_fel_or_cdc(self):
        args = types.SimpleNamespace(uopbridge=__file__, xfel="xfel", port=None)
        for stage in ("fel", "cdc"):
            with self.subTest(stage=stage), \
                 patch.object(ble, "command", AsyncMock(return_value="OK")) as command, \
                 patch.object(ble, "wait_fel", AsyncMock(return_value="D1", side_effect=TimeoutError("FEL") if stage == "fel" else None)), \
                 patch.object(ble, "run_process", AsyncMock(return_value=(0, "loaded"))) as loader, \
                 patch.object(ble, "wait_bridge", AsyncMock(side_effect=TimeoutError("CDC"))):
                with self.assertRaises(TimeoutError):
                    await ble.prepare_download(None, args)
                self.assertEqual([call.args[1] for call in command.await_args_list], ["fel"])
                if stage == "fel":
                    loader.assert_not_awaited()

    async def test_fel_accepts_d1s_silicon_id(self):
        with patch.object(ble, "run_process", AsyncMock(return_value=(0, "AWUSBFEX soc=0000000018590000"))):
            self.assertIn("1859", await ble.wait_fel("xfel"))

    async def test_rescue_main_never_reconnects_after_rom_request(self):
        args = ble.parse_args(["--address", "AA:BB", "rescue-download", "--uopbridge", __file__])
        client = self.client([b"OK rescue scheduled"])
        client.__aenter__ = AsyncMock(return_value=client)
        client.__aexit__ = AsyncMock(return_value=False)
        events = []

        class Connection:
            async def __aenter__(self):
                return client

            async def __aexit__(self, *_args):
                events.append("close")

        def make_client(*_args, **_kwargs):
            events.append("connect")
            return Connection()

        async def fel(_xfel):
            client.is_connected = False
            client.read_gatt_char.side_effect = AssertionError("BLE read after ROM")
            client.write_gatt_char.side_effect = AssertionError("BLE write after ROM")
            events.append("fel")
            return "D1"

        async def loader(*_args, **_kwargs):
            events.append("load")
            return 0, "loaded"

        async def cdc(_port):
            events.append("cdc")
            return "/dev/ttyACM0"

        async def settle(delay):
            self.assertEqual(delay, 1.5)
            client.write_gatt_char.assert_awaited_once_with(ble.CONTROL, b"rescue-download", response=True)
            events.append("reset-settle")

        module = types.SimpleNamespace(BleakClient=Mock(side_effect=make_client), BleakScanner=Mock())
        with patch.dict("sys.modules", bleak=module), patch.object(ble, "wait_fel", fel), \
             patch.object(ble.asyncio, "sleep", settle), \
             patch.object(ble, "run_process", loader), patch.object(ble, "wait_bridge", cdc), \
             patch.object(ble, "print_download_command") as output:
            await ble.main_async(args)
        client.write_gatt_char.assert_awaited_once_with(ble.CONTROL, b"rescue-download", response=True)
        module.BleakClient.assert_called_once()
        output.assert_called_once_with("/dev/ttyACM0")
        self.assertEqual(events, ["connect", "reset-settle", "fel", "load", "cdc", "close"])

    async def test_rescue_stops_when_loader_fails_without_more_ble(self):
        args = types.SimpleNamespace(uopbridge=__file__, xfel="xfel", port=None)
        client = self.client([b"OK rescue scheduled"])
        with patch.object(ble, "wait_fel", AsyncMock(return_value="D1")), \
             patch.object(ble.asyncio, "sleep", AsyncMock()), \
             patch.object(ble, "run_process", AsyncMock(return_value=(1, "failed"))), \
             patch.object(ble, "wait_bridge", AsyncMock()) as bridge, \
             patch.object(ble, "print_download_command") as output:
            with self.assertRaisesRegex(RuntimeError, "loader failed"):
                await ble.rescue_download(client, args)
        client.write_gatt_char.assert_awaited_once_with(ble.CONTROL, b"rescue-download", response=True)
        bridge.assert_not_awaited()
        output.assert_not_called()

    async def test_rescue_disconnect_is_expected(self):
        client = self.client([ConnectionError()])
        client.is_connected = False
        self.assertIn("verify ROM", await ble.command(client, "rescue-download"))

    async def test_prepare_download_requires_fel_loader_and_cdc(self):
        args = types.SimpleNamespace(uopbridge=__file__, xfel="xfel", port=None)
        events = []

        async def command(_client, request):
            events.append(request)
            return "OK"

        async def fel(_xfel):
            events.append("fel-confirmed")
            return "D1"

        async def loader(*_args, **_kwargs):
            events.append("load")
            return 0, "loaded"

        async def cdc(_port):
            events.append("cdc")
            return "/dev/ttyACM0"

        with patch.object(ble, "command", command), patch.object(ble, "wait_fel", fel), \
             patch.object(ble, "run_process", loader), patch.object(ble, "wait_bridge", cdc):
            await ble.prepare_download(None, args)
        self.assertEqual(events, ["fel", "fel-confirmed", "load", "cdc", "c3-download"])


if __name__ == "__main__":
    unittest.main()
