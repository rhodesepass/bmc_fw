from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent

@unittest.skipUnless(shutil.which("cc"), "host C compiler unavailable")
class AppOtaHostTests(unittest.TestCase):
    def test_firmware_state_machine(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("esp_err.h", "esp_attr.h", "esp_http_server.h", "sdkconfig.h", "nvs.h", "driver/gpio.h",
                         "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h", "freertos/semphr.h"):
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('#include "app_ota_stub.h"\n')
            binary = root / "ota"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                            "-I", str(TESTS), str(TESTS / "app_ota_host.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
