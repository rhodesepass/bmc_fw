#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

typedef int esp_err_t;
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
typedef struct { uint32_t custom[4]; } rtc_retain_mem_t;
#define CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE 16
#define CONFIG_BMC_BLE_DEBUG 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FINISHED -2
#define ESP_RST_POWERON 1
#define ESP_RST_DEEPSLEEP 5
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_MODE_INPUT_OUTPUT_OD 7
#define GPIO_PULLUP_ENABLE 1
#define ESP_SLEEP_WAKEUP_ALL 0
#define ESP_GPIO_WAKEUP_GPIO_LOW 0
#define pdMS_TO_TICKS(n) (n)
#define ESP_ERROR_CHECK(e) assert((e) == ESP_OK)
#define ESP_LOGE(tag, ...) ((void)(tag))

static rtc_retain_mem_t rtc;
static int levels[22], modes[22];
static bool held[22], deep_hold, ready;
static unsigned cause, reason, retries, charge_calls, stop_calls;
static int charge_error, wifi_error, ble_error, config_error;
static uint64_t pins, wake_mask, timer_us;
static jmp_buf jump;
static rtc_retain_mem_t *bootloader_common_get_rtc_retain_mem(void) { return &rtc; }
static int gpio_set_level(int pin, int level) { levels[pin] = level; return ESP_OK; }
static int gpio_get_level(int pin) { return levels[pin]; }
static int gpio_config(const gpio_config_t *cfg)
{
    if (config_error) return config_error;
    for (unsigned i = 0; i < 22; ++i)
        if (cfg->pin_bit_mask & (1ULL << i)) modes[i] = cfg->mode;
    return ESP_OK;
}
static int gpio_hold_en(int pin) { held[pin] = true; return ESP_OK; }
static int gpio_hold_dis(int pin)
{
    assert(levels[18] == 1 && levels[19] == 1 && levels[3] == 0);
    assert(modes[18] == GPIO_MODE_INPUT_OUTPUT && modes[19] == GPIO_MODE_INPUT_OUTPUT);
    held[pin] = false;
    return ESP_OK;
}
static void gpio_deep_sleep_hold_en(void) { deep_hold = true; }
static void gpio_deep_sleep_hold_dis(void) { deep_hold = false; }
static unsigned esp_sleep_get_wakeup_causes(void) { return cause; }
static uint64_t esp_sleep_get_gpio_wakeup_status(void) { return pins; }
static unsigned esp_reset_reason(void) { return reason; }
static int esp_sleep_disable_wakeup_source(int source)
{
    (void)source; wake_mask = timer_us = 0; return ESP_OK;
}
static int esp_sleep_enable_timer_wakeup(uint64_t us) { timer_us = us; return ESP_OK; }
static int esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(uint64_t mask, int mode)
{
    assert(mode == ESP_GPIO_WAKEUP_GPIO_LOW); wake_mask = mask; return ESP_OK;
}
static void esp_deep_sleep_start(void) { longjmp(jump, 1); }
static void esp_restart(void) { longjmp(jump, 4); }
static int esp_task_wdt_reset(void) { return ESP_OK; }
static const char *esp_err_to_name(int err) { (void)err; return "error"; }
static void vTaskDelay(int ms) { assert(ms == 2000); ++retries; longjmp(jump, 2); }
