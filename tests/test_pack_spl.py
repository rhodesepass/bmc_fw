import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "pack_spl.py"
SPEC = importlib.util.spec_from_file_location("pack_spl", SCRIPT)
pack_spl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack_spl)


def make_spl(length=1024, fill=0x31):
    data = bytearray([fill] * length)
    data[4:12] = b"eGON.BT0"
    struct.pack_into("<II", data, 12, 0x5F0A6C39, length)
    checksum = sum(int.from_bytes(data[i:i + 4], "little") for i in range(0, length, 4)) & 0xFFFFFFFF
    struct.pack_into("<I", data, 12, checksum)
    return bytes(data)


class PackSplTests(unittest.TestCase):
    def test_checksum_and_payload_corruption(self):
        spl = make_spl(fill=0xFF)
        self.assertEqual(pack_spl.validate_spl(spl), spl)
        for offset in (12, 300, 1023):
            damaged = bytearray(spl)
            damaged[offset] ^= 1
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                pack_spl.validate_spl(damaged)

    def test_header_and_size_rejections(self):
        with self.assertRaisesRegex(ValueError, "truncated"):
            pack_spl.validate_spl(b"x" * 19)
        with self.assertRaisesRegex(ValueError, "magic"):
            pack_spl.validate_spl(b"x" * 1024)
        for length in (0, 20, 1023, 1025, 129 * 1024):
            spl = bytearray(make_spl())
            struct.pack_into("<I", spl, 16, length)
            with self.assertRaisesRegex(ValueError, "1024-aligned"):
                pack_spl.validate_spl(spl)
        with self.assertRaisesRegex(ValueError, "exceeds file size"):
            pack_spl.validate_spl(make_spl()[:-1])
        self.assertEqual(len(pack_spl.validate_spl(make_spl(128 * 1024))), 128 * 1024)

    def test_trailing_padding_and_combined_image_rejection(self):
        spl = make_spl()
        self.assertEqual(pack_spl.validate_spl(spl + b"\0" * 100 + b"\xff" * 100), spl)
        with self.assertRaisesRegex(ValueError, "non-padding"):
            pack_spl.validate_spl(spl + b"\0" * 100 + b"U-Boot")

    def test_slot_layout_and_unused_space(self):
        images = [make_spl((index + 1) * 1024, index + 1) for index in range(2)]
        for count in range(1, 3):
            partition = pack_spl.pack_spls(images[:count])
            self.assertEqual(len(partition), 512 * 1024)
            expected = images[:count] if count == 2 else images[:1] * 2
            for index, spl in enumerate(expected):
                start = index * 128 * 1024
                self.assertEqual(partition[start:start + len(spl)], spl)
                self.assertEqual(partition[start + len(spl):start + 128 * 1024], b"\xff" * (128 * 1024 - len(spl)))
            self.assertEqual(partition[256 * 1024:], b"\xff" * (256 * 1024))
        for images in ([], [make_spl()] * 3, [make_spl()] * 4):
            with self.assertRaises(ValueError):
                pack_spl.pack_spls(images)

    def test_cli_preserves_output_on_failure_and_prints_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sunxi-spl.bin"
            output = Path(directory) / "partition.bin"
            source.write_bytes(b"invalid")
            output.write_bytes(b"existing")
            command = [sys.executable, str(SCRIPT), str(source), "-o", str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"existing")
            source.write_bytes(make_spl())
            result = subprocess.run(command + ["--partition-offset", "0x280000"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("0x280000", result.stdout)
            self.assertIn("2 occupied slot(s)", result.stdout)
            self.assertIn("slot 1:", result.stdout)
            self.assertEqual(output.read_bytes(), pack_spl.pack_spls([source.read_bytes()]))


if __name__ == "__main__":
    unittest.main()
