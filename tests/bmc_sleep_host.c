#include "bmc_sleep_stub.h"
#include "../main/bmc_sleep.c"
#include <string.h>

bool bmc_debug_prepare_sleep(void) { return ready; }
esp_err_t bmc_wifi_stop(void) { ++stop_calls; return wifi_error; }
esp_err_t ble_debug_stop(void) { ++stop_calls; return ble_error; }
void bmc_recovery_request_rescue(void) { longjmp(jump, 3); }
esp_err_t bq25601_init(void) { ++charge_calls; return charge_error; }
esp_err_t bq25601_poll(void) { ++charge_calls; return charge_error; }
esp_err_t bq25601_read_status(bq25601_status_t *s) { memset(s, 0, sizeof(*s)); return charge_error; }

static void fresh(void)
{
    memset(&rtc, 0, sizeof(rtc));
    memset(held, 0, sizeof(held));
    memset(modes, 0, sizeof(modes));
    memset(levels, 0, sizeof(levels));
    levels[0] = levels[2] = 1;
    reason = ESP_RST_DEEPSLEEP;
    cause = 1;
    pins = 1ULL << 2;
    ready = deep_hold = false;
    charge_error = wifi_error = ble_error = config_error = 0;
    retries = charge_calls = stop_calls = 0;
    timer_us = wake_mask = 0;
    rtc.custom[0] = bmc_recovery_encode(0);
    rtc.custom[3] = BMC_SLEEP_BOOTLOADER_MAGIC;
    bootloader_ready = true;
    bmc_sleep_retain(rtc.custom, true);
}

static void expect_sleep(void (*fn)(void))
{
    int event = setjmp(jump);
    if (!event) { fn(); assert(!"expected sleep"); }
    assert(event == 1);
    assert(bmc_sleep_retained(rtc.custom));
    assert(levels[18] && levels[19] && !levels[3]);
    assert(held[18] && held[19] && held[3] && deep_hold);
    assert(held[21] && modes[21] == GPIO_MODE_INPUT);
    assert(held[7] && modes[7] == GPIO_MODE_INPUT);
    assert(bmc_recovery_decode(rtc.custom[0]) == 0);
    assert(wake_mask & 1);
}

int main(void)
{
    fresh();
    expect_sleep(bmc_sleep_boot);
    assert(charge_calls == 2 && !stop_calls && wake_mask == 5 && !timer_us);
    fresh();
    levels[2] = 0;
    expect_sleep(bmc_sleep_boot);
    assert(wake_mask == 1 && timer_us == 60000000);
    fresh();
    pins = 5;
    bmc_sleep_boot();
    assert(!charge_calls && !bmc_sleep_retained(rtc.custom));
    assert(bmc_recovery_decode(rtc.custom[0]) == 1);
    assert(!held[18] && !held[19] && !held[21]);
    fresh();
    rtc.custom[2] ^= 1;
    bmc_sleep_boot();
    assert(!charge_calls && !bmc_sleep_retained(rtc.custom) && !deep_hold);
    fresh();
    bmc_sleep_poll();
    assert(!stop_calls && !charge_calls);
    ready = true;
    expect_sleep(bmc_sleep_poll);
    assert(stop_calls == 2);
    fresh();
    ready = true;
    levels[0] = 0;
    expect_sleep(bmc_sleep_poll);
    fresh();
    ready = true;
    wifi_error = ESP_FAIL;
    int event = setjmp(jump);
    if (!event) { bmc_sleep_poll(); assert(!"expected retry"); }
    assert(event == 2 && retries == 1 && !charge_calls);
    levels[0] = 0;
    event = setjmp(jump);
    if (!event) { bmc_sleep_poll(); assert(!"expected user-requested boot"); }
    assert(event == 4 && !bmc_sleep_retained(rtc.custom));
    fresh();
    ready = true;
    ble_error = ESP_ERR_NOT_FINISHED;
    bmc_sleep_poll();
    assert(!charge_calls && !retries);
    fresh();
    charge_error = ESP_FAIL;
    event = setjmp(jump);
    if (!event) { bmc_sleep_boot(); assert(!"expected retry"); }
    assert(event == 2 && !wake_mask && levels[18] && levels[19]);
    fresh();
    pins = 1;
    rtc.custom[0] = bmc_recovery_encode(3);
    event = setjmp(jump);
    if (!event) { bmc_sleep_boot(); assert(!"expected rescue"); }
    assert(event == 3);
    fresh();
    rtc.custom[3] = 0;
    bmc_sleep_boot();
    ready = true;
    bmc_sleep_poll();
    assert(!stop_calls && !charge_calls && !strcmp(phase, "bootloader_required"));
    puts("sleep host: charging/key/combined wake, holds, IRQ fallback, retry, RTC and rescue PASS");
}
