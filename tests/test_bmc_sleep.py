from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class SleepTests(unittest.TestCase):
    def test_actual_sleep_manager(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("esp_err.h", "sdkconfig.h", "driver/gpio.h", "bootloader_common.h",
                         "esp_sleep.h", "esp_system.h", "esp_task_wdt.h", "esp_log.h",
                         "freertos/FreeRTOS.h", "freertos/task.h"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.symlink_to(HERE / "bmc_sleep_stub.h")
            binary = root / "sleep"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-fsanitize=undefined",
                            "-I", str(root), "-I", str(HERE),
                            str(HERE / "bmc_sleep_host.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
