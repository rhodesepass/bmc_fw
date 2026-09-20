from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


@unittest.skipUnless(shutil.which("cc"), "host C compiler unavailable")
class BmcOtaHostTests(unittest.TestCase):
    def test_actual_self_ota_handler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("esp_err.h", "esp_http_server.h", "esp_ota_ops.h",
                         "esp_app_desc.h", "esp_app_format.h", "esp_system.h", "esp_timer.h",
                         "psa/crypto.h", "freertos/FreeRTOS.h", "freertos/semphr.h"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "bmc_ota_stub.h"\n')
            binary = root / "bmc_ota_host"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root), "-I", str(TESTS),
                            str(TESTS / "bmc_ota_host.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
