from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


@unittest.skipUnless(shutil.which("cc"), "host C compiler unavailable")
class ChargerHostTests(unittest.TestCase):
    def test_safe_profile_and_fault_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("esp_err.h", "esp_check.h", "esp_log.h",
                         "driver/i2c_master.h", "freertos/FreeRTOS.h",
                         "freertos/semphr.h"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(TESTS / "bq25601_stub.h")
            binary = root / "charger"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-I", str(root), "-I", str(TESTS),
                str(TESTS / "bq25601_host.c"),
                str(TESTS.parent / "main/bq25601.c"), "-o", str(binary),
            ], check=True)
            result = subprocess.run([str(binary), "normal"], check=True,
                                    capture_output=True, text=True)
            operations = int(result.stdout.strip().split("=")[-1])
            for scenario in ("mismatch", "bus-down", "wrong-part"):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)
            for operation in range(1, operations + 1):
                with self.subTest(failed_operation=operation):
                    subprocess.run([str(binary), "init-failure", str(operation)],
                                   check=True)


if __name__ == "__main__":
    unittest.main()
