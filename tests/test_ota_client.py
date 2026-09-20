import importlib.util
import struct
import unittest
import tempfile
import shutil
import subprocess
import types
from unittest.mock import AsyncMock, Mock, patch
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location('bmc_ota', Path(__file__).resolve().parents[1] / 'tools/bmc_ota.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


class ProtocolTests(unittest.TestCase):
    def test_bmc_image_and_stage_boundaries(self):
        self.assertEqual(ota.TARGETS['bmc'], 7)
        for size in (288, 0x180000):
            ota.validate_image(b'\xe9' + bytes(size - 1), 7)
        for data in (b'', b'\xe9' + bytes(286), bytes(288), b'\xe9' + bytes(0x180000)):
            with self.assertRaises(ValueError):
                ota.validate_image(data, 7)
        self.assertEqual(ota.status(struct.pack('<8I', ota.MAGIC, 1, 1, 288, 288, 4, 0, 3))['stage'], 3)

    def test_touch_boundaries(self):
        self.assertEqual(ota.TARGETS['touch'], 6)
        for size in (4, 63488):
            packets = list(ota.image_frames(bytes(size), 6, 42))
            self.assertEqual(struct.unpack_from('<II', packets[0], 32), (size, 6))
        for size in (0, 1, 3, 5, 63487, 63489, 63492):
            with self.assertRaises(ValueError):
                list(ota.image_frames(bytes(size), 6, 42))

    def test_frame_stream_and_fragment_reassembly(self):
        data = bytes(range(256)) * 20
        packets = list(ota.image_frames(data, 5, 123))
        received = bytearray()
        for seq, packet in enumerate(packets, 1):
            self.assertEqual(len(packet), 2052)
            self.assertEqual(zlib.crc32(packet[:2048]), struct.unpack_from('<I', packet, 2048)[0])
            magic, op, session, actual_seq, offset, length, _, _ = struct.unpack_from('<8I', packet)
            self.assertEqual((magic, session, actual_seq), (ota.MAGIC, 123, seq))
            rebuilt = bytearray()
            for part in ota.fragments(packet):
                self.assertLessEqual(len(part), 20)
                self.assertEqual(struct.unpack_from('<I', part)[0], len(rebuilt))
                rebuilt.extend(part[4:])
            self.assertEqual(rebuilt, packet)
            if op == 11:
                self.assertEqual(offset, len(received))
                received.extend(packet[32:32 + length])
        self.assertEqual(received, data)
        self.assertEqual(struct.unpack_from('<I', packets[-1], 4)[0], 12)

    def test_reject_invalid_inputs(self):
        for target, data in [(0, b'x'), (4, b''), (3, bytes((1 << 20) + 1))]:
            with self.assertRaises(ValueError):
                list(ota.image_frames(data, target, 1))
        with self.assertRaises(ValueError):
            ota.status(bytes(32))
        with self.assertRaises(ValueError):
            ota.status(struct.pack('<8I', ota.MAGIC, 1, 1, 0, 1, 3, 0, 2))


class FakeClient:
    is_connected = True

    def __init__(self, complete=True):
        self.s = [ota.MAGIC, 0, 0, 0, 0, 0, 0, 2]
        self.buffer = bytearray()
        self.writes = 0
        self.complete = complete

    async def read_gatt_char(self, uuid):
        return struct.pack('<8I', *self.s)

    async def write_gatt_char(self, uuid, part, response):
        self.writes += 1
        offset = struct.unpack_from('<I', part)[0]
        if offset == 0:
            self.buffer.clear()
        assert offset == len(self.buffer)
        self.buffer.extend(part[4:])
        if len(self.buffer) == 2052:
            _, op, session, seq, offset, length, _, _ = struct.unpack_from('<8I', self.buffer)
            self.s = [ota.MAGIC, session, seq, offset + (length if op == 11 else 0), 0, 4 if op == 12 and self.complete else 3, 0, 2]


class UploadTests(unittest.IsolatedAsyncioTestCase):
    async def test_touch_end_requires_full_durability(self):
        for cls in (ota.Uploader, ota.HttpUploader):
            for durable in (0, 4):
                uploader = object.__new__(cls)
                uploader.progress = lambda _: None
                async def send(packet):
                    return {'durable': durable}
                uploader.send = send
                uploader.send_batch = lambda packets: send(packets[-1])
                if durable:
                    await uploader.upload(b'CH32', 6)
                else:
                    with self.assertRaisesRegex(RuntimeError, '持久化'):
                        await uploader.upload(b'CH32', 6)

    async def test_complete_and_duplicate_ack(self):
        client = FakeClient()
        uploader = ota.Uploader(client, timeout=1, progress=lambda _: None)
        packets = list(ota.image_frames(b'abc', 5, 42))
        for packet in packets:
            await uploader.send(packet)
        writes = client.writes
        await uploader.send(packets[-1])
        self.assertEqual(client.writes, writes)

    async def test_disconnect_after_device_ack_queries_before_resend(self):
        class DisconnectAfterAck(FakeClient):
            dropped = False

            async def connect(self):
                self.is_connected = True

            async def write_gatt_char(self, *args, **kwargs):
                await super().write_gatt_char(*args, **kwargs)
                if len(self.buffer) == 2052 and not self.dropped:
                    self.dropped = True
                    self.is_connected = False
                    raise RuntimeError('模拟 BLE 链路断开')

        client = DisconnectAfterAck()
        uploader = ota.Uploader(client, timeout=2, progress=lambda _: None)
        packet = next(ota.image_frames(b'abc', 5, 42))
        await uploader.send(packet)
        self.assertEqual(client.writes, len(list(ota.fragments(packet))))

    async def test_att_write_is_not_completion(self):
        client = FakeClient(complete=False)
        uploader = ota.Uploader(client, timeout=1, progress=lambda _: None)
        with self.assertRaisesRegex(RuntimeError, 'END'):
            for packet in ota.image_frames(b'abc', 5, 42):
                await uploader.send(packet)


class TransactionTests(unittest.IsolatedAsyncioTestCase):
    async def test_bmc_mixed_transaction_order_and_failure(self):
        for targets in ((7,), (1, 6, 7)):
            for fail_target in (None, *targets, 4 if len(targets) > 1 else 7, 'ota-finish'):
                events = []
                class Updater:
                    async def control(self, command):
                        events.append(command)
                        if command == fail_target:
                            raise RuntimeError('failed')
                    async def wait_stage(self, stage):
                        events.append(('stage', stage))
                    async def upload(self, data, target):
                        events.append(('image', target))
                        if target == fail_target:
                            raise RuntimeError('failed')
                operation = ota.update_transaction(Updater(), types.SimpleNamespace(boot=True), b'fit', [(b'image', target) for target in targets])
                if fail_target is not None:
                    with self.assertRaisesRegex(RuntimeError, 'failed'):
                        await operation
                    self.assertNotIn('ota-reboot', events)
                    if fail_target != 'ota-finish':
                        self.assertNotIn('ota-finish', events)
                else:
                    await operation
                    expected = ['ota-bmc', ('stage', 3), ('image', 7)]
                    if len(targets) > 1:
                        expected += ['ota', ('stage', 1), ('image', 4), ('stage', 2), ('image', 1), ('image', 6)]
                    self.assertEqual(events, expected + ['ota-finish', 'ota-reboot'])

    async def test_bmc_only_needs_no_fit_and_invalid_other_image_prevents_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            bmc = Path(directory) / 'bmc.bin'
            touch = Path(directory) / 'touch.bin'
            bmc.write_bytes(b'\xe9' + bytes(287))
            touch.write_bytes(b'bad')
            args = types.SimpleNamespace(target='bmc', image=bmc, extra_images=[], sha256=None,
                                         boot=False, uboot_fit=None, url='http://device', token='secret', timeout=1)
            with patch.object(ota, 'update_transaction') as transaction, patch.object(ota, 'HttpUploader'):
                await ota.run(args)
                self.assertIsNone(transaction.call_args.args[2])
                transaction.reset_mock()
                args.extra_images = [f'touch={touch}']
                with self.assertRaisesRegex(ValueError, '4 字节'):
                    await ota.run(args)
                transaction.assert_not_called()

    async def test_touch_only_transaction_success_and_failure(self):
        for fail in (False, True):
            events = []
            class Updater:
                async def control(self, command):
                    events.append(command)
                async def wait_stage(self, stage):
                    events.append(('stage', stage))
                async def upload(self, data, target):
                    events.append(('image', target))
                    if target == 6 and fail:
                        raise RuntimeError('CH32 readback failed')
            args = types.SimpleNamespace(boot=False)
            if fail:
                with self.assertRaisesRegex(RuntimeError, 'CH32'):
                    await ota.update_transaction(Updater(), args, b'fit', [(b'CH32', 6)])
                self.assertNotIn('ota-finish', events)
            else:
                await ota.update_transaction(Updater(), args, b'fit', [(b'CH32', 6)])
                self.assertEqual(events, ['ota', ('stage', 1), ('image', 4), ('stage', 2), ('image', 6), 'ota-finish'])

    async def test_mtu_checked_before_ota_control(self):
        for negotiated, succeeds in ((23, False), (247, True)):
            events = []

            class Client:
                mtu_size = 23

                def __init__(self, *args, **kwargs):
                    self._backend = types.SimpleNamespace(_acquire_mtu=self.acquire)

                async def acquire(self):
                    events.append('mtu')
                    self.mtu_size = negotiated

                async def __aenter__(self):
                    return self

                async def __aexit__(self, *args):
                    pass

                async def write_gatt_char(self, uuid, data, response):
                    events.append(data.decode())
                    raise RuntimeError('reached control')

            with tempfile.TemporaryDirectory() as directory:
                fit = Path(directory) / 'fit'
                fit.write_bytes(b'fit')
                args = types.SimpleNamespace(uboot_fit=fit, address='fake', timeout=1, att_payload=244)
                with patch.object(ota, 'load_images', return_value=[(b'test', 5)]), patch.dict('sys.modules', {'bleak': types.SimpleNamespace(BleakClient=Client)}):
                    with self.assertRaisesRegex(RuntimeError if succeeds else ValueError, 'reached control' if succeeds else '尚未开始 OTA'):
                        await ota.run(args)
                self.assertEqual(events, ['mtu', 'ota'] if succeeds else ['mtu'])

    async def test_multi_image_finishes_only_once(self):
        events = []

        class Client:
            def __init__(self, *args, **kwargs):
                pass

            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                pass

        class Updater:
            def __init__(self, *args):
                pass

            async def control(self, command):
                events.append(command)

            async def wait_stage(self, stage):
                events.append(('stage', stage))

            async def upload(self, data, target):
                events.append(('image', target))

        with tempfile.TemporaryDirectory() as directory:
            boot = Path(directory) / 'boot'
            rootfs = Path(directory) / 'rootfs'
            boot.write_bytes(b'boot')
            rootfs.write_bytes(bytes(128 << 10))
            fit = Path(directory) / 'u-boot.img'
            fit.write_bytes(b'fit')
            args = types.SimpleNamespace(target=None, image=None, extra_images=[f'boot={boot}', f'rootfs={rootfs}'], sha256=None, boot=True, uboot_fit=fit, address='fake', timeout=1, att_payload=20)
            with patch.dict('sys.modules', {'bleak': types.SimpleNamespace(BleakClient=Client)}), patch.object(ota, 'Uploader', Updater):
                await ota.run(args)
            self.assertEqual(events, ['ota', ('stage', 1), ('image', 4), ('stage', 2), ('image', 1), ('image', 2), 'ota-finish', 'boot'])
            rootfs.write_bytes(b'not aligned')
            with self.assertRaisesRegex(ValueError, '128 KiB'):
                ota.load_images(args)

    async def test_missing_fit_rejected_before_connection(self):
        with patch.object(ota, 'load_images', return_value=[(b'boot', 1)]):
            with self.assertRaisesRegex(ValueError, '--uboot-fit'):
                await ota.run(types.SimpleNamespace(uboot_fit=None))

    async def test_ble_and_wifi_provisioning_require_explicit_address(self):
        for route in ({}, {'wifi_sta': 'test-network'}, {'wifi_ap': True}):
            with self.subTest(route=route):
                client = Mock(side_effect=AssertionError('must not connect'))
                args = types.SimpleNamespace(address=None, **route)
                with patch.object(ota, 'load_images', return_value=[(b'app', 7)]), \
                        patch.dict('sys.modules', {'bleak': types.SimpleNamespace(BleakClient=client)}):
                    with self.assertRaisesRegex(ValueError, '--address'):
                        await ota.run(args)
                client.assert_not_called()

    async def test_http_session_does_not_require_ble_address(self):
        args = types.SimpleNamespace(url='http://192.0.2.1', token='test-token', timeout=1)
        images = [(b'app', 7)]
        client = Mock(side_effect=AssertionError('HTTP must not connect BLE'))
        updater = Mock()
        transaction = AsyncMock()
        with patch.object(ota, 'load_images', return_value=images), \
                patch.object(ota, 'HttpUploader', return_value=updater) as http, \
                patch.object(ota, 'update_transaction', transaction), \
                patch.dict('sys.modules', {'bleak': types.SimpleNamespace(BleakClient=client)}):
            await ota.run(args)
        http.assert_called_once_with(args.url, args.token, args.timeout)
        transaction.assert_awaited_once_with(updater, args, None, images)
        updater.close.assert_called_once_with()
        client.assert_not_called()


class CliTests(unittest.TestCase):
    def test_bmc_payload_limit_checked_before_run(self):
        for payload in (4, 245, 512):
            with patch('sys.argv', ['bmc_ota.py', 'verify', 'test.bin', '--uboot-fit', 'fit', '--att-payload', str(payload)]), patch.object(ota, 'run') as run:
                with self.assertRaises(SystemExit) as caught:
                    ota.main()
                self.assertEqual(caught.exception.code, 2)
                run.assert_not_called()
        with self.assertRaisesRegex(ValueError, '5..244'):
            ota.Uploader(FakeClient(), att_payload=245)

    def test_fit_required_and_legacy_option_alias(self):
        for option in ('--uboot-fit', '--rescue-fit'):
            async def check(args):
                self.assertEqual(args.uboot_fit, Path('u-boot.img'))
                self.assertIsNone(args.address)
            with patch('sys.argv', ['bmc_ota.py', 'boot', 'boot.bin', option, 'u-boot.img']), patch.object(ota, 'run', check):
                ota.main()
        with patch('sys.argv', ['bmc_ota.py', 'boot', 'boot.bin']):
            with self.assertRaises(SystemExit) as caught:
                ota.main()
            self.assertEqual(caught.exception.code, 2)


class WebClientTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'), 'Node.js unavailable')
    def test_web_multi_image_mock_gatt(self):
        script = r"""
const fs=require('fs'), vm=require('vm'), assert=require('assert'), crypto=require('crypto');
const elements=new Map(), el=id=>{if(!elements.has(id))elements.set(id,{value:'',files:[],disabled:false});return elements.get(id);};
const bytesFile=data=>({size:data.length,arrayBuffer:async()=>Uint8Array.from(data).buffer});
el('target').value='1';el('ubootFit').files=[bytesFile(Buffer.from('fit'))];el('file').files=[bytesFile(Buffer.from('boot'))];
el('extraRootfs').files=[bytesFile(Buffer.alloc(128*1024))];
let s=[0x31555042,0,0,0,0,0,0,2], assembly=Buffer.alloc(0), active=null, events=[], response='';
const characteristics={
4:{writeValueWithResponse:async data=>{response='OK '+Buffer.from(data).toString();events.push(Buffer.from(data).toString());if(Buffer.from(data).toString()==='ota')s=[0x31555042,0,0,0,0,0,0,1];},readValue:async()=>new TextEncoder().encode(response)},
7:{readValue:async()=>{const b=new ArrayBuffer(32),v=new DataView(b);s.forEach((n,i)=>v.setUint32(i*4,n,true));return v;}},
6:{writeValueWithResponse:async fragment=>{
  assert(fragment.length<=20);const part=Buffer.from(fragment),at=part.readUInt32LE();if(at===0)assembly=Buffer.alloc(0);assert.equal(at,assembly.length);assembly=Buffer.concat([assembly,part.subarray(4)]);
  if(assembly.length!==2052)return;
  const f=assembly,op=f.readUInt32LE(4),session=f.readUInt32LE(8),seq=f.readUInt32LE(12),offset=f.readUInt32LE(16),len=f.readUInt32LE(20);
  assert.equal(f.readUInt32LE(),0x31555042);
  if(op===10){assert.equal(seq,1);active={session,seq:1,total:f.readUInt32LE(32),target:f.readUInt32LE(36),hash:f.subarray(40,72),data:Buffer.alloc(0)};}
  else{assert.equal(session,active.session);assert.equal(seq,++active.seq);assert.equal(offset,active.data.length);if(op===11)active.data=Buffer.concat([active.data,f.subarray(32,32+len)]);else{assert.equal(op,12);assert.equal(active.data.length,active.total);assert(crypto.createHash('sha256').update(active.data).digest().equals(active.hash));events.push('END '+active.target);}}
  s=[0x31555042,session,seq,active.data.length,active.target===4 || (active.target===6 && op!==12)?0:active.data.length,op===12?4:3,0,active.target===4&&op!==12?1:2];
}}
};
const device={addEventListener:()=>{},gatt:{connected:true,connect:async()=>({getPrimaryService:async()=>({getCharacteristic:async uuid=>characteristics[Number(uuid[7])]})})}};
const context={document:{getElementById:el},navigator:{bluetooth:{requestDevice:async()=>device}},crypto:crypto.webcrypto,TextEncoder,TextDecoder,DataView,Uint8Array,Uint32Array,Set,Date,Error,Promise,Number,Array,setTimeout:fn=>{fn();},window:{addEventListener:()=>{}}};
vm.createContext(context);vm.runInContext(fs.readFileSync(process.argv[1],'utf8'),context);
(async()=>{await el('connect').onclick();const fit=el('ubootFit').files;el('ubootFit').files=[];await el('start').onclick();assert.deepEqual(events,[]);assert(el('notice').textContent.includes('更新未完成'));el('ubootFit').files=fit;await el('start').onclick();assert.deepEqual(events,['ota','END 4','END 1','END 2','ota-finish']);assert.equal(el('boot').disabled,false);assert(el('notice').textContent.includes('已提交'));events=[];el('extraRootfs').files=[];el('target').value='6';el('file').files=[bytesFile(Buffer.from('CH32'))];await el('start').onclick();assert.deepEqual(events,['ota','END 4','END 6','ota-finish']);})().catch(e=>{console.error(e);process.exitCode=1;});
"""
        result = subprocess.run(['node', '-e', script, str(Path(__file__).resolve().parents[1] / 'web/ota.js')], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
