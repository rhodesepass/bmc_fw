from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import ctypes
import os
import zlib

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "host compiler unavailable")
class LinkTests(unittest.TestCase):
    def test_actual_http_handlers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("esp_err.h", "esp_event.h", "esp_http_server.h", "esp_netif.h",
                         "esp_random.h", "esp_rom_crc.h", "esp_wifi.h", "esp_heap_caps.h", "lwip/sockets.h", "freertos/FreeRTOS.h",
                         "freertos/semphr.h", "freertos/task.h"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "bmc_link_stub.h"\n')
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root), "-I", str(ROOT / "tests"),
                            str(ROOT / "tests/bmc_link_host.c"), "-o", str(root / "test")], check=True)
            subprocess.run([str(root / "test")], check=True)

    def test_idf_rom_crc_seed_matches_zlib(self):
        idf = Path(os.environ.get("IDF_PATH", str(Path.home() / ".espressif/v6.0.2/esp-idf")))
        source = idf / "components/esp_rom/linux/esp_rom_crc.c"
        if not source.exists():
            self.skipTest("ESP-IDF reference CRC implementation unavailable")
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "crc.so"
            subprocess.run(["cc", "-shared", "-fPIC", "-I", str(idf / "components/esp_rom/include"),
                            str(source), "-o", str(library)], check=True)
            crc = ctypes.CDLL(str(library)).esp_rom_crc32_le
            crc.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32]
            crc.restype = ctypes.c_uint32
            data = bytes(range(256)) * 1024
            for seed in (0, 0x12345678):
                self.assertEqual(crc(seed, data, len(data)), zlib.crc32(data, seed))
                split = seed
                for offset in range(0, len(data), 1024):
                    block = data[offset:offset+1024]
                    split = crc(split, block, len(block))
                self.assertEqual(split, zlib.crc32(data, seed))
            self.assertEqual(zlib.crc32(data), 3348152310)

    def test_actual_credentials_decoder(self):
        source = (ROOT / "main/bmc_link.c").read_text()
        functions = source[source.index("static int hex_digit("):source.index("static void random_hex(")]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = '#include <stddef.h>\n#include <stdint.h>\n#include <stdbool.h>\n#include <string.h>\n#include <assert.h>\n' + functions + r'''
int main(void) {
    uint8_t output[64];
    assert(decode("6550617373", output, 32) == 5);
    assert(!memcmp(output, "ePass", 5));
    assert(decode("e4b8ade69687", output, 32) == 6);
    assert(decode("", output, 32) == -1);
    assert(decode("123", output, 32) == -1);
    assert(decode("xy", output, 32) == -1);
    assert(decode("00", output, 32) == -1);
    assert(decode("c080", output, 32) == -1);
    assert(decode("eda080", output, 32) == -1);
    assert(decode("f4908080", output, 32) == -1);
    assert(decode("e4b8", output, 32) == -1);
    assert(decode("ff", output, 32) == -1);
    assert(decode("616263", output, 2) == -1);
    return 0;
}
'''
            (root / "test.c").write_text(code)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(root / "test.c"), "-o", str(root / "test")], check=True)
            subprocess.run([str(root / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
