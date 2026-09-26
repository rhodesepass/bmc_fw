#include "bmc_sleep.h"
#include "bmc_debug.h"
#include "bmc_power.h"
#include "bmc_recovery.h"
#include "ble_debug.h"
#include "board_pins.h"
#include "bq25601.h"
#include "bootloader_common.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../bootloader_components/bmc_rescue/recovery_state.h"
#include "../bootloader_components/bmc_rescue/sleep_state.h"
#include <stdio.h>
#include <stdatomic.h>

_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE >= 16,
               "sleep and rescue share the custom RTC area");

static const char *TAG = "bmc_sleep";
static _Atomic(const char *) phase = "awake";
static _Atomic esp_err_t last_error;
static bool bootloader_ready;
static unsigned wake_cause;
static uint64_t wake_pins;
static const gpio_num_t quiet_pins[] = {
    BMC_PIN_SPI_SCLK, BMC_PIN_SPI_CS, BMC_PIN_SPI_MOSI, BMC_PIN_SPI_MISO,
    BMC_PIN_UART_RX, BMC_PIN_UART_TX, BMC_PIN_TO_APP_IRQ,
};
static const gpio_num_t output_pins[] = {
    BMC_PIN_APP_RESET, BMC_PIN_APP_CORE_DIS, BMC_PIN_MAINSYS_DIS,
};

static volatile uint32_t *retained(void)
{
    return (volatile uint32_t *)bootloader_common_get_rtc_retain_mem()->custom;
}

static esp_err_t off_pins(void)
{
    for (unsigned i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i) {
        gpio_num_t pin = output_pins[i];
        esp_err_t err = gpio_set_level(pin, pin != BMC_PIN_APP_RESET);
        if (err != ESP_OK) return err;
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << pin,
            .mode = pin == BMC_PIN_APP_RESET ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT};
        err = gpio_config(&cfg);
        if (err != ESP_OK) return err;
        err = gpio_hold_en(pin);
        if (err != ESP_OK) return err;
    }
    for (unsigned i = 0; i < sizeof(quiet_pins) / sizeof(quiet_pins[0]); ++i) {
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << quiet_pins[i], .mode = GPIO_MODE_INPUT};
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) return err;
        err = gpio_hold_en(quiet_pins[i]);
        if (err != ESP_OK) return err;
    }
    gpio_deep_sleep_hold_en();
    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << BMC_PIN_ESP_WAKE) | (1ULL << BMC_PIN_CHRG_IRQ),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&inputs);
}

static void release_pins(void)
{
    /* Program the off state before unholding: reset defaults enable both rails. */
    ESP_ERROR_CHECK(off_pins());
    gpio_deep_sleep_hold_dis();
    for (unsigned i = 0; i < sizeof(quiet_pins) / sizeof(quiet_pins[0]); ++i)
        ESP_ERROR_CHECK(gpio_hold_dis(quiet_pins[i]));
    for (unsigned i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i)
        ESP_ERROR_CHECK(gpio_hold_dis(output_pins[i]));
}

static esp_err_t enter_sleep(void)
{
    phase = "charger";
    esp_err_t err = bq25601_poll();
    if (err != ESP_OK) return err;
    bq25601_status_t status;
    err = bq25601_read_status(&status);
    if (err != ESP_OK) return err;
    phase = "gpio";
    err = off_pins();
    if (err != ESP_OK) return err;
    phase = "wake_config";
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    uint64_t mask = 1ULL << BMC_PIN_ESP_WAKE;
    if (gpio_get_level(BMC_PIN_CHRG_IRQ)) mask |= 1ULL << BMC_PIN_CHRG_IRQ;
    else {
        err = esp_sleep_enable_timer_wakeup(60000000ULL);
        if (err != ESP_OK) return err;
    }
    err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) return err;
    bmc_sleep_retain(retained(), true);
    __sync_synchronize();
    phase = "sleep";
    /* A key pressed during preparation must cause immediate wake, not be lost. */
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static void boot_if_pressed(void)
{
    if (!gpio_get_level(BMC_PIN_ESP_WAKE)) {
        /* Services may already be stopped; a new user press requests a fresh boot. */
        bmc_sleep_retain(retained(), false);
        esp_restart();
    }
}

static void retry(esp_err_t err)
{
    last_error = err;
    ESP_LOGE(TAG, "%s: %s; APP remains off", phase, esp_err_to_name(err));
    boot_if_pressed();
    ESP_ERROR_CHECK(esp_task_wdt_reset());
    vTaskDelay(pdMS_TO_TICKS(2000));
}

void bmc_sleep_boot(void)
{
    bootloader_ready = retained()[3] == BMC_SLEEP_BOOTLOADER_MAGIC;
    retained()[3] = 0;
    wake_cause = esp_sleep_get_wakeup_causes();
    wake_pins = esp_sleep_get_gpio_wakeup_status();
    bool resume = bootloader_ready && esp_reset_reason() == ESP_RST_DEEPSLEEP &&
        bmc_sleep_retained(retained());
    if (!resume) {
        /* Clear stale holds after a maintenance crash or a corrupted RTC marker. */
        if (esp_reset_reason() != ESP_RST_POWERON) release_pins();
        bmc_sleep_retain(retained(), false);
        return;
    }
    ESP_ERROR_CHECK(off_pins());
    bool key = (wake_pins & (1ULL << BMC_PIN_ESP_WAKE)) != 0;
    while (!key && gpio_get_level(BMC_PIN_ESP_WAKE)) {
        phase = "charger_init";
        esp_err_t err = bq25601_init();
        if (err == ESP_OK) err = enter_sleep();
        retry(err);
        key = !gpio_get_level(BMC_PIN_ESP_WAKE);
    }
    bmc_sleep_retain(retained(), false);
    unsigned attempts = bmc_recovery_decode(retained()[0]);
    if (attempts >= BMC_RECOVERY_FAILED_BOOTS) bmc_recovery_request_rescue();
    retained()[0] = bmc_recovery_encode(attempts + 1);
    release_pins();
    phase = "awake";
}

void bmc_sleep_poll(void)
{
#if CONFIG_BMC_BLE_DEBUG
    if (!bootloader_ready) { phase = "bootloader_required"; return; }
    if (!bmc_debug_prepare_sleep()) return;
    phase = "wifi_stop";
    esp_err_t err = bmc_wifi_stop();
    if (err != ESP_OK) { retry(err); return; }
    phase = "ble_stop";
    err = ble_debug_stop();
    if (err == ESP_ERR_NOT_FINISHED) { boot_if_pressed(); return; }
    if (err != ESP_OK) { retry(err); return; }
    for (;;) retry(enter_sleep());
#endif
}

void bmc_sleep_status(char *out, size_t size)
{
    snprintf(out, size, " sleep=%s sleep_error=%d wake=%u wake_pins=0x%llx",
             phase, (int)last_error, wake_cause, (unsigned long long)wake_pins);
}
