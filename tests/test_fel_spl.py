from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from make_fel_spl import make_fel_spl, make_header


class FelSplTest(unittest.TestCase):
    def test_header_and_checksum(self):
        image = make_fel_spl()
        self.assertEqual(len(image), 1024)
        self.assertEqual(image[4:12], b'eGON.BT0')
        checksum, length = struct.unpack_from('<II', image, 12)
        self.assertEqual(length, len(image))
        words = list(struct.unpack('<256I', image))
        words[3] = 0x5F0A6C39
        self.assertEqual(sum(words) & 0xFFFFFFFF, checksum)

    def test_execution_reaches_rom_with_instruction_cache_disabled(self):
        image = make_fel_spl()
        registers = [0] * 32
        pc, mhcr, fenced = 0x20000, 1, False
        for _ in range(5):
            instruction, = struct.unpack_from('<I', image, pc - 0x20000)
            opcode = instruction & 0x7F
            rd = instruction >> 7 & 31
            rs1 = instruction >> 15 & 31
            if opcode == 0x6F:
                immediate = ((instruction >> 31) << 20 |
                             (instruction >> 12 & 255) << 12 |
                             (instruction >> 20 & 1) << 11 |
                             (instruction >> 21 & 1023) << 1)
                self.assertEqual(rd, 0)
                pc += immediate
            elif opcode == 0x73:
                self.assertEqual(instruction >> 20, 0x7C1)
                self.assertEqual(instruction >> 12 & 7, 7)
                mhcr &= ~rs1
                pc += 4
            elif opcode == 0x0F:
                self.assertEqual(instruction >> 12 & 7, 1)
                fenced = True
                pc += 4
            elif opcode == 0x13:
                self.assertEqual(instruction >> 12 & 7, 0)
                registers[rd] = registers[rs1] + (instruction >> 20)
                pc += 4
            elif opcode == 0x67:
                self.assertEqual(rd, 0)
                pc = (registers[rs1] + (instruction >> 20)) & ~1
            else:
                self.fail(f'Unexpected instruction {instruction:08x}')
        self.assertEqual(pc, 0x20)
        self.assertEqual(mhcr & 1, 0)
        self.assertTrue(fenced)

    def test_checked_in_artifacts_match(self):
        image = make_fel_spl()
        self.assertEqual((ROOT / 'main/fel_spl.bin').read_bytes(), image)
        self.assertEqual((ROOT / 'main/fel_spl.h').read_text(), make_header(image))


if __name__ == '__main__':
    unittest.main()
