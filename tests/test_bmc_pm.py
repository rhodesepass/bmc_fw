from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


@unittest.skipUnless(shutil.which("cc"), "host C compiler unavailable")
class PowerManagementTests(unittest.TestCase):
    def test_runtime_clock_and_radio_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("esp_err.h", "esp_attr.h", "esp_bt.h", "esp_pm.h",
                         "esp_private/esp_clk.h", "freertos/FreeRTOS.h"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(TESTS / "bmc_pm_stub.h")
            binary = root / "pm"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-I", str(root), "-I", str(TESTS),
                            str(TESTS / "bmc_pm_host.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
