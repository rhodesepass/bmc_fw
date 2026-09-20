from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


@unittest.skipUnless(shutil.which("cc"), "host C compiler unavailable")
class RuntimeHostTests(unittest.TestCase):
    def test_actual_runtime_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("esp_err.h", "esp_attr.h", "esp_app_desc.h", "esp_log.h",
                         "esp_random.h", "esp_rom_crc.h", "esp_timer.h", "sdkconfig.h",
                         "driver/gpio.h", "driver/spi_common.h", "driver/uart.h",
                         "soc/spi_periph.h", "esp_rom_gpio.h",
                         "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(TESTS / "bmc_runtime_stub.h")
            binary = root / "runtime"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-I", str(root), "-I", str(TESTS),
                            str(TESTS / "bmc_runtime_host.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
