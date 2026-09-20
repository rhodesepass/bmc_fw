import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HEADERS = ["sdkconfig.h", "bootloader_common.h", "esp_rom_gpio.h", "esp_rom_sys.h",
           "hal/gpio_ll.h", "soc/efuse_reg.h", "soc/gpio_sig_map.h",
           "soc/reset_reasons.h", "soc/rtc_cntl_reg.h", "soc/soc.h"]


def test_rescue():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for name in HEADERS:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "rescue_stub.h"\n')
        binary = root / "rescue-host"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root), "-I", str(HERE),
                        str(HERE / "rescue_host.c"), str(HERE.parent / "hooks.c"),
                        str(HERE.parents[2] / "main/bmc_recovery.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_rescue()
