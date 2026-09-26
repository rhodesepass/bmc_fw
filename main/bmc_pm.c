#include "bmc_pm.h"
#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

enum pm_mode { PM_PERFORMANCE, PM_MODEM, PM_BALANCED };
static enum pm_mode mode = PM_MODEM;
static const char *const names[] = {"performance", "modem", "balanced"};
static esp_pm_lock_handle_t apb_lock, boot_lock;
static portMUX_TYPE boot_mux = portMUX_INITIALIZER_UNLOCKED;
static bool boot_active, radio_ready;
static esp_err_t boot_error;

esp_err_t IRAM_ATTR bmc_pm_boot(bool active)
{
    if (!boot_lock) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL_SAFE(&boot_mux);
    esp_err_t err = ESP_OK;
    if (active != boot_active) {
        err = active ? esp_pm_lock_acquire(boot_lock) : esp_pm_lock_release(boot_lock);
        if (err == ESP_OK) boot_active = active;
    }
    boot_error = err;
    portEXIT_CRITICAL_SAFE(&boot_mux);
    return err;
}

static esp_err_t set_mode(enum pm_mode next)
{
    esp_pm_config_t previous;
    esp_err_t err = esp_pm_get_configuration(&previous);
    if (err != ESP_OK) return err;
    const esp_pm_config_t config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = next == PM_BALANCED ? 80 : 160,
        /* SPI0 can arrive without warning; UART has no wake preamble either. */
        .light_sleep_enable = false,
    };
    err = esp_pm_configure(&config);
    if (err != ESP_OK) return err;
    if (radio_ready) {
        err = next == PM_PERFORMANCE ? esp_bt_sleep_disable() : esp_bt_sleep_enable();
        if (err != ESP_OK) {
            esp_err_t restored = esp_pm_configure(&previous);
            return restored == ESP_OK ? err : restored;
        }
    }
    mode = next;
    return ESP_OK;
}

esp_err_t bmc_pm_init(void)
{
    esp_err_t err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "bmc_spi", &apb_lock);
    if (err != ESP_OK) return err;
    /* The direct SPI/GDMA backend bypasses the IDF slave driver's clock lock. */
    err = esp_pm_lock_acquire(apb_lock);
    if (err != ESP_OK) return err;
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "bmc_boot", &boot_lock);
    if (err != ESP_OK) return err;
    err = bmc_pm_boot(true);
    if (err != ESP_OK) return err;
    return set_mode(mode);
}

esp_err_t bmc_pm_radio_ready(void)
{
    radio_ready = true;
    return set_mode(mode);
}

esp_err_t bmc_pm_command(const char *request, char *response, size_t capacity)
{
    esp_err_t err = ESP_OK;
    if (strcmp(request, "pm-status")) {
        enum pm_mode next;
        for (next = PM_PERFORMANCE; next <= PM_BALANCED; ++next)
            if (!strncmp(request, "pm ", 3) && !strcmp(request + 3, names[next])) break;
        if (next > PM_BALANCED) {
            snprintf(response, capacity, "use pm performance|modem|balanced or pm-status");
            return ESP_ERR_INVALID_ARG;
        }
        err = set_mode(next);
    }
    esp_pm_config_t config;
    esp_err_t query = esp_pm_get_configuration(&config);
    if (query != ESP_OK) return query;
    portENTER_CRITICAL_SAFE(&boot_mux);
    bool boot = boot_active;
    esp_err_t lock_error = boot_error;
    portEXIT_CRITICAL_SAFE(&boot_mux);
    snprintf(response, capacity,
             "mode=%s min_mhz=%d max_mhz=%d cpu_mhz=%d apb_mhz=%d boot_lock=%d boot_error=%s bt_sleep=%d light_sleep=%d result=%s",
             names[mode], config.min_freq_mhz, config.max_freq_mhz,
             esp_clk_cpu_freq() / 1000000, esp_clk_apb_freq() / 1000000,
             boot, esp_err_to_name(lock_error), mode != PM_PERFORMANCE, config.light_sleep_enable,
             esp_err_to_name(err));
    return err;
}
