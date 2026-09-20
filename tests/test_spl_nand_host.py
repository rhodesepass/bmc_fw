from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


TESTS = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "driver/gpio.h", "sdkconfig.h", "driver/spi_slave.h",
    "esp_private/spi_slave_internal.h", "esp_attr.h", "esp_heap_caps.h",
    "esp_intr_alloc.h", "esp_log.h", "esp_partition.h", "freertos/FreeRTOS.h",
    "freertos/task.h", "freertos/semphr.h", "esp_check.h", "esp_cpu.h", "esp_timer.h",
)


@unittest.skipUnless(shutil.which("cc"), "host C compiler unavailable")
class SplNandHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        root = Path(cls.temporary.name)
        for name in HEADERS:
            header = root / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "esp_idf_stub.h"\n')
        cls.binary = root / "spl_nand_host"
        result = subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(root), "-I", str(TESTS), str(TESTS / "spl_nand_host.c"),
            "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_fast_spi_callbacks(self):
        root = Path(self.temporary.name)
        binary = root / "spl_nand_fast_host"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-DCONFIG_BMC_FAST_SPI_SLAVE=1", "-I", str(root), "-I", str(TESTS),
                        str(TESTS / "spl_nand_host.c"), "-o", str(binary)], check=True)
        for mode in ([], ["52-pages"], ["bad-checksum"], ["bad-size"]):
            subprocess.run([str(binary), *mode], check=True)

    def test_real_spl_page_sequence(self):
        subprocess.run([str(self.binary), "52-pages"], check=True)

    def test_image_size_boundaries(self):
        subprocess.run([str(self.binary), "bad-size"], check=True)

    def test_actual_firmware_callbacks(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("bounded cache", result.stdout)

    def test_corrupt_partition_never_releases_d1s(self):
        result = subprocess.run([str(self.binary), "bad-checksum"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("reset asserted", result.stdout)


if __name__ == "__main__":
    unittest.main()
