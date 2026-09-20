from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "host compiler unavailable")
class PowerTests(unittest.TestCase):
    def test_actual_power_interlock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "stub.h").write_text(r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;
typedef int gpio_num_t;
typedef int *SemaphoreHandle_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_WIFI_NOT_INIT 3
#define ESP_ERR_WIFI_NOT_STARTED 4
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define portMAX_DELAY 1000
#define pdMS_TO_TICKS(n) (n)
static int level, started, start_calls, stop_calls, held, mutex;
static int stop_error, gpio_error, invalidations;
static int force_expected;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
static void xSemaphoreTake(SemaphoreHandle_t m, int timeout) { (void)m; (void)timeout; assert(!held); held=1; }
static void xSemaphoreGive(SemaphoreHandle_t m) { (void)m; assert(held); held=0; }
static int gpio_set_level(int pin, int value) { (void)pin; assert(held); if(gpio_error) return gpio_error; if(value) assert(!started || force_expected); level=value; return 0; }
static int gpio_get_level(int pin) { (void)pin; assert(held); return level; }
static int gpio_config(const gpio_config_t *cfg) { assert(held); assert(cfg->mode==GPIO_MODE_INPUT_OUTPUT); return 0; }
static void vTaskDelay(int ticks) { assert(held); assert(ticks>=20); }
static int esp_wifi_start(void) { assert(held); assert(!level); start_calls++; started=1; return 0; }
static int esp_wifi_stop(void) { assert(held); stop_calls++; if(stop_error) return stop_error; started=0; return 0; }
''')
            for name in ("esp_err.h", "sdkconfig.h", "esp_wifi.h", "driver/gpio.h",
                         "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "stub.h"\n')
            source = root / "test.c"
            source.write_text(f'#include "{ROOT / "main/bmc_power.c"}"\n' + r'''
void bmc_link_invalidate(void) { assert(held); invalidations++; }
static int apply_under_power(void *arg) { assert(held && !level); return *(int *)arg; }
int main(void) {
    assert(!bmc_mainsys_enabled());
    assert(bmc_wifi_start()==ESP_ERR_INVALID_STATE);
    assert(!start_calls);
    int callback_result = 77;
    assert(bmc_wifi_apply(apply_under_power, &callback_result)==ESP_ERR_INVALID_STATE);
    assert(bmc_power_init()==ESP_OK);
    assert(bmc_mainsys_enabled());
    assert(bmc_wifi_apply(apply_under_power, &callback_result)==77);
    assert(!start_calls);
    assert(bmc_wifi_start()==ESP_OK && started);
    stop_error=99;
    assert(bmc_mainsys_set(false)==99);
    assert(level==0 && started);
    assert(invalidations==1);
    stop_error=0;
    assert(bmc_mainsys_set(false)==ESP_OK && level==1 && !started);
    assert(!bmc_mainsys_enabled());
    assert(bmc_wifi_apply(apply_under_power, &callback_result)==ESP_ERR_INVALID_STATE);
    assert(bmc_wifi_start()==ESP_ERR_INVALID_STATE && start_calls==1);
    assert(bmc_power_init()==ESP_OK && level==1);
    assert(bmc_mainsys_set(true)==ESP_OK && level==0);
    assert(bmc_wifi_start()==ESP_OK && start_calls==2);
    assert(bmc_wifi_stop()==ESP_OK && !started);
    stop_error=ESP_ERR_WIFI_NOT_INIT;
    assert(bmc_mainsys_set(false)==ESP_OK && level==1);
    assert(bmc_mainsys_set(true)==ESP_OK);
    stop_error=ESP_ERR_WIFI_NOT_STARTED;
    assert(bmc_mainsys_set(false)==ESP_OK);
    assert(!started && level==1);
    stop_error=0;
    assert(bmc_mainsys_set(true)==ESP_OK);
    assert(bmc_wifi_start()==ESP_OK && started);
    stop_error=99;
    force_expected=1;
    assert(bmc_mainsys_force_off()==ESP_OK && level==1);
    assert(bmc_wifi_start()==ESP_ERR_INVALID_STATE);
    return 0;
}
''')
            binary = root / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_no_wifi_power_bypass(self):
        for path in (ROOT / "main").glob("*.c"):
            if path.name in ("bmc_power.c", "bmc_link.c"):
                continue
            self.assertNotRegex(path.read_text(), r"\besp_wifi_(?:start|stop)\s*\(", str(path))
        link = (ROOT / "main/bmc_link.c").read_text()
        self.assertIn("bmc_wifi_apply(start_link, &args)", link)
        self.assertEqual(link.count("esp_wifi_start("), 1)
        self.assertNotIn("esp_wifi_stop(", link)


if __name__ == "__main__":
    unittest.main()
