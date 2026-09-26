#include "bat_gauge.h"
#include "spl_nand.h"
#include "bmc_debug.h"
#include "bmc_power.h"
#include "bmc_runtime.h"
#include "bmc_ota.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epass_bmc";

#define BAT_POLL_MS 2000

static void chrg_irq_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BMC_PIN_CHRG_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    esp_err_t charger_err = bq25601_init();
    if (charger_err != ESP_OK)
        ESP_LOGE(TAG, "charger configuration: %s", esp_err_to_name(charger_err));
    ESP_LOGI(TAG, "BMC bring-up: battery gauge");
    esp_err_t power_err = bmc_power_init();
    if (power_err != ESP_OK)
        ESP_LOGE(TAG, "MAINSYS control: %s; Wi-Fi remains unavailable", esp_err_to_name(power_err));

#if CONFIG_BMC_BLE_DEBUG
    esp_err_t debug_err = bmc_debug_init();
    ESP_ERROR_CHECK(esp_task_wdt_reset());
    if (debug_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BMC_DEBUG_BOOT_DELAY_MS));
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        debug_err = bmc_debug_boot_default();
    }
    if (debug_err != ESP_OK) {
        ESP_LOGE(TAG, "Debug boot: %s; releasing D1s for recovery", esp_err_to_name(debug_err));
        if (spl_nand_enter_fel() != ESP_OK)
            gpio_set_level(BMC_PIN_APP_RESET, 1);
    }
#elif CONFIG_BMC_SPL_NAND
    esp_err_t boot_err = spl_nand_start();
    if (boot_err != ESP_OK)
        ESP_LOGE(TAG, "SPL NAND: %s; D1s held in reset", esp_err_to_name(boot_err));
#endif

    vTaskDelay(pdMS_TO_TICKS(100));
    chrg_irq_gpio_init();

    esp_err_t err = bat_gauge_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bat_gauge_init: %s", esp_err_to_name(err));
    }

#if CONFIG_BMC_SPL_NAND
    spl_nand_stats_t previous = {0};
    unsigned boot_log_ticks = 0;
#endif
    ESP_ERROR_CHECK(esp_task_wdt_reset());
    while (1) {
        charger_err = bq25601_poll();
        if (charger_err != ESP_OK)
            ESP_LOGE(TAG, "charger verification: %s", esp_err_to_name(charger_err));
        bmc_ota_poll();
        ESP_ERROR_CHECK(esp_task_wdt_reset());
#if CONFIG_BMC_SPL_NAND
        spl_nand_stats_t boot;
        spl_nand_get_stats(&boot);
        if (++boot_log_ticks % 5 == 0 ||
            boot.page_reads != previous.page_reads || boot.cache_reads != previous.cache_reads ||
            boot.unexpected != previous.unexpected || boot.cache_misses != previous.cache_misses ||
            boot.queue_error != previous.queue_error) {
            ESP_LOGI(TAG, "SPL NAND: page=%lu cache=%lu unexpected=%lu cache_miss=%lu queue=%s",
                     (unsigned long)boot.page_reads, (unsigned long)boot.cache_reads,
                     (unsigned long)boot.unexpected, (unsigned long)boot.cache_misses, esp_err_to_name(boot.queue_error));
            previous = boot;
        }
#endif
        bat_gauge_snapshot_t s;
        err = bat_gauge_update(&s);
        if (err != ESP_OK) s = (bat_gauge_snapshot_t){0};
        bmc_runtime_battery(&s);
        if (err == ESP_OK && s.valid) {
            int irq = gpio_get_level(BMC_PIN_CHRG_IRQ);
            ESP_LOGI(TAG,
                     "bat %lumV (filt %lu) soc=%d%% %s | vbus=%s chg=%s pg=%d fault=0x%02x latched=0x%02x irq=%d",
                     (unsigned long)s.vbat_mv,
                     (unsigned long)s.vbat_filt_mv,
                     s.soc_pct,
                     bat_power_str(s.power),
                     bq25601_vbus_str(s.chg.vbus),
                     bq25601_chg_str(s.chg.chg),
                     (int)s.chg.power_good,
                     s.chg.fault,
                     s.chg.fault_latched,
                     irq);
        } else {
            ESP_LOGW(TAG, "gauge update failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_POLL_MS));
    }
}
