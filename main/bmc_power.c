#include "bmc_power.h"
#include "bmc_link.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t power_lock;
static bool initialized;

static esp_err_t stop_wifi(void)
{
    bmc_link_invalidate();
    esp_err_t err = esp_wifi_stop();
    return err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED ? ESP_OK : err;
}

esp_err_t bmc_power_init(void)
{
    if (initialized) return ESP_OK;
    if (!power_lock) power_lock = xSemaphoreCreateMutex();
    if (!power_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    /* The external pulldown already enables MAINSYS; avoid a shutoff glitch. */
    esp_err_t err = gpio_set_level(BMC_PIN_MAINSYS_DIS, 0);
    if (err == ESP_OK) {
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << BMC_PIN_MAINSYS_DIS,
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&config);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        initialized = true;
    }
    xSemaphoreGive(power_lock);
    return err;
}

bool bmc_mainsys_enabled(void)
{
    if (!power_lock) return false;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    bool enabled = initialized && gpio_get_level(BMC_PIN_MAINSYS_DIS) == 0;
    xSemaphoreGive(power_lock);
    return enabled;
}

esp_err_t bmc_mainsys_set(bool enabled)
{
    if (!power_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    esp_err_t err = initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && !enabled) err = stop_wifi();
    if (err == ESP_OK) {
        err = gpio_set_level(BMC_PIN_MAINSYS_DIS, enabled ? 0 : 1);
        if (err == ESP_OK && enabled) vTaskDelay(pdMS_TO_TICKS(20) + 1);
    }
    xSemaphoreGive(power_lock);
    return err;
}

esp_err_t bmc_wifi_start(void)
{
    if (!power_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (initialized && gpio_get_level(BMC_PIN_MAINSYS_DIS) == 0)
        err = esp_wifi_start();
    xSemaphoreGive(power_lock);
    return err;
}

esp_err_t bmc_mainsys_force_off(void)
{
    if (!power_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    /* A physical long press overrides a failed Wi-Fi stop, not a soft shutdown. */
    stop_wifi();
    esp_err_t err = initialized ? gpio_set_level(BMC_PIN_MAINSYS_DIS, 1) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(power_lock);
    return err;
}

esp_err_t bmc_wifi_stop(void)
{
    if (!power_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    esp_err_t err = stop_wifi();
    xSemaphoreGive(power_lock);
    return err;
}

esp_err_t bmc_wifi_apply(esp_err_t (*apply)(void *), void *arg)
{
    if (!power_lock || !apply) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(power_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (initialized && gpio_get_level(BMC_PIN_MAINSYS_DIS) == 0)
        err = apply(arg);
    xSemaphoreGive(power_lock);
    return err;
}
