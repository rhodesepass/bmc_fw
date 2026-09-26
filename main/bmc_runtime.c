#include "bmc_runtime.h"
#include "bmc_lifecycle.h"
#include "bmc_recovery.h"
#include "bmc_power.h"
#include "board_pins.h"
#include "spl_nand.h"
#include "app_ota.h"
#include "driver/spi_common.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/spi_periph.h"
#include <stdio.h>
#include <string.h>

#define FRAME_SIZE 2052
#define MAGIC 0x31434d42u

static const char *TAG = "bmc_runtime";
static struct bmc_lifecycle lifecycle;
static QueueHandle_t incoming;
static DMA_ATTR uint32_t replies[3][FRAME_SIZE / 4];
static DRAM_ATTR uint32_t request[FRAME_SIZE / 4];
static const uint32_t *published, *dma_active;
static bool boot_seen;
static portMUX_TYPE wire_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE battery_lock = portMUX_INITIALIZER_UNLOCKED;
static bat_gauge_snapshot_t battery;
static int64_t battery_at;
static bool key_raw, key_stable;
static unsigned key_edges;
static uint64_t key_changed;

void bmc_runtime_battery(const bat_gauge_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&battery_lock);
    battery = *snapshot;
    battery_at = esp_timer_get_time();
    portEXIT_CRITICAL(&battery_lock);
}

void bmc_runtime_reset(void)
{
    if (!incoming) return;
    uint32_t epoch = esp_random();
    if (!epoch || epoch == lifecycle.epoch) epoch = lifecycle.epoch + 1;
    bmc_lifecycle_reset(&lifecycle, epoch);
    xQueueReset(incoming);
    portENTER_CRITICAL(&wire_lock);
    published = NULL;
    boot_seen = false;
    portEXIT_CRITICAL(&wire_lock);
    ESP_LOGI(TAG, "boot epoch=%lu: power cuts locked until APP READY", (unsigned long)lifecycle.epoch);
}

void IRAM_ATTR bmc_runtime_boot_seen(void)
{
    portENTER_CRITICAL_ISR(&wire_lock);
    boot_seen = true;
    portEXIT_CRITICAL_ISR(&wire_lock);
}

const uint8_t *IRAM_ATTR bmc_runtime_spi_done(const uint8_t *rx, size_t bits)
{
    if (!incoming || bits != FRAME_SIZE * 8) return NULL;
    const uint32_t *words = (const uint32_t *)rx;
    if (words[0] != MAGIC && words[0] != 0) return NULL;
    if (!words[0])
        for (unsigned i = 1; i < FRAME_SIZE / 4; ++i) if (words[i]) return NULL;
    if (words[0] == MAGIC) {
        BaseType_t wake = pdFALSE;
        xQueueSendFromISR(incoming, rx, &wake);
        if (wake) portYIELD_FROM_ISR();
    }
    portENTER_CRITICAL_ISR(&wire_lock);
    dma_active = published;
    const uint8_t *result = (const uint8_t *)dma_active;
    portEXIT_CRITICAL_ISR(&wire_lock);
    return result;
}

static void receive(uint64_t now)
{
    if (xQueueReceive(incoming, request, 0) != pdTRUE) return;
    if (request[0] != MAGIC || request[1] != 1 || !request[2] ||
        request[3] < 1 || request[3] > 3 ||
        esp_rom_crc32_le(0, (const uint8_t *)request, FRAME_SIZE - 4) != request[512]) return;
    for (unsigned i = 5; i < 512; ++i) if (request[i]) return;

    uint32_t *reply = NULL;
    portENTER_CRITICAL(&wire_lock);
    for (unsigned i = 0; i < 3; ++i)
        if (replies[i] != published && replies[i] != dma_active) { reply = replies[i]; break; }
    portEXIT_CRITICAL(&wire_lock);
    if (!reply) return;
    bool was_armed = lifecycle.armed;
    unsigned result = bmc_lifecycle_request(&lifecycle, request[3], request[4], now);
    if (!result && request[3] == 2) bmc_recovery_mark_app_ready();
    if (!was_armed && lifecycle.armed && key_stable) lifecycle.released = false;
    bat_gauge_snapshot_t snapshot;
    int64_t sampled;
    portENTER_CRITICAL(&battery_lock);
    snapshot = battery;
    sampled = battery_at;
    portEXIT_CRITICAL(&battery_lock);
    /* An open ADC input must not become a believable 0% battery reading. */
    bool fresh = snapshot.valid && esp_timer_get_time() - sampled < 6000000;
    bool valid = fresh &&
        snapshot.vbat_mv >= 1800 && snapshot.vbat_mv <= 4700 &&
        snapshot.vbat_filt_mv >= 1800 && snapshot.vbat_filt_mv <= 4700;
    memset(reply, 0, FRAME_SIZE);
    reply[0] = MAGIC;
    reply[1] = 1;
    reply[2] = request[2];
    reply[3] = request[3] | 0x80000000u;
    reply[4] = lifecycle.epoch;
    reply[5] = result;
    reply[6] = lifecycle.state;
    reply[7] = lifecycle.armed | (valid << 1) |
        ((valid && snapshot.power == BAT_POWER_CHARGING) << 2) |
        ((valid && snapshot.power == BAT_POWER_FULL) << 3) |
        ((fresh && snapshot.chg.power_good) << 4);
    reply[8] = valid ? snapshot.vbat_filt_mv : 0;
    reply[9] = valid ? (uint32_t)snapshot.soc_pct : 0;
    reply[10] = now / 1000;
    snprintf((char *)&reply[16], 32, "%.31s", esp_app_get_description()->version);
    reply[512] = esp_rom_crc32_le(0, (const uint8_t *)reply, FRAME_SIZE - 4);
    portENTER_CRITICAL(&wire_lock);
    published = reply;
    portEXIT_CRITICAL(&wire_lock);
}

esp_err_t bmc_runtime_power_on(void)
{
    bmc_runtime_reset();
    esp_err_t err = bmc_mainsys_set(true);
    if (err == ESP_OK) err = gpio_set_level(BMC_PIN_APP_CORE_DIS, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        err = gpio_set_direction(BMC_PIN_SPI_MISO, GPIO_MODE_INPUT_OUTPUT);
    }
    if (err == ESP_OK) {
        /* gpio_output_enable() clears GPIO-matrix routing in ESP-IDF. */
        esp_rom_gpio_connect_out_signal(BMC_PIN_SPI_MISO,
            spi_periph_signal[SPI2_HOST].spiq_out, false, false);
        err = uart_set_pin(UART_NUM_0, BMC_PIN_UART_TX, BMC_PIN_UART_RX,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    return err;
}

static void cut_power(bool forced)
{
    /* Stop Wi-Fi before removing its rail; a stop failure leaves rescue powered. */
    esp_err_t err = bmc_wifi_stop();
    if (forced) err = ESP_OK;
    if (err != ESP_OK) {
        lifecycle.shutdown_at = 0;
        ESP_LOGE(TAG, "Wi-Fi stop failed: %s; soft poweroff cancelled", esp_err_to_name(err));
        return;
    }
    err = spl_nand_hold();
    /* Avoid back-powering the unpowered APP through idle-high signal pins. */
    if (err == ESP_OK) err = gpio_set_direction(BMC_PIN_UART_TX, GPIO_MODE_INPUT);
    if (err == ESP_OK) err = gpio_set_direction(BMC_PIN_SPI_MISO, GPIO_MODE_INPUT);
    if (err == ESP_OK) err = gpio_set_level(BMC_PIN_APP_CORE_DIS, 1);
    if (err == ESP_OK) err = forced ? bmc_mainsys_force_off() : bmc_mainsys_set(false);
    if (err == ESP_OK) {
        lifecycle.state = BMC_APP_OFF;
        lifecycle.armed = false;
        lifecycle.released = false;
        lifecycle.shutdown_at = 0;
        ESP_LOGI(TAG, "APP powered off; waiting for key release before deep sleep");
    } else {
        bmc_runtime_power_on();
        ESP_LOGE(TAG, "power cut failed: %s; power restored for recovery", esp_err_to_name(err));
    }
}

void bmc_runtime_poll(void)
{
    if (!incoming) return;
    portENTER_CRITICAL(&wire_lock);
    bool rebooted = boot_seen;
    portEXIT_CRITICAL(&wire_lock);
    if (rebooted) bmc_runtime_reset();
    uint64_t now = esp_timer_get_time() / 1000;
    receive(now);
    bool pressed = gpio_get_level(BMC_PIN_ESP_WAKE) == 0;
    if (pressed != key_raw) {
        key_raw = pressed;
        key_changed = now;
        ++key_edges;
        ESP_LOGI(TAG, "power key GPIO%d level=%d state=%u", BMC_PIN_ESP_WAKE,
                 !pressed, (unsigned)lifecycle.state);
    }
    if (now - key_changed >= 40) key_stable = key_raw;
    enum bmc_button_action action = bmc_lifecycle_button(&lifecycle, key_stable, now);
    if (action == BMC_BUTTON_RESCUE) {
        bmc_recovery_request_rescue();
    } else if (action == BMC_BUTTON_BOOT && !app_ota_pending()) {
        esp_err_t err = bmc_runtime_power_on();
        if (err == ESP_OK) {
            app_ota_normal_boot();
            err = spl_nand_boot_slot(CONFIG_BMC_SPL_SLOT);
        }
        ESP_LOGI(TAG, "power button boot: %s", esp_err_to_name(err));
    } else if (action == BMC_BUTTON_CUT ||
               (lifecycle.shutdown_at && now >= lifecycle.shutdown_at)) {
        cut_power(action == BMC_BUTTON_CUT);
    }
}

bool bmc_runtime_can_sleep(void)
{
    return lifecycle.state == BMC_APP_OFF && lifecycle.released &&
        !key_raw && !key_stable && !bmc_mainsys_enabled();
}

void bmc_runtime_status(char *out, size_t size)
{
    snprintf(out, size, "epoch=%lu state=%u armed=%u mainsys=%u boot_attempts=%u key_gpio=%d key_level=%d key_stable=%u key_released=%u key_edges=%u",
             (unsigned long)lifecycle.epoch, (unsigned)lifecycle.state,
             lifecycle.armed, bmc_mainsys_enabled(), bmc_recovery_boot_attempts(),
             BMC_PIN_ESP_WAKE, gpio_get_level(BMC_PIN_ESP_WAKE), key_stable,
             lifecycle.released, key_edges);
}

esp_err_t bmc_runtime_init(void)
{
    gpio_config_t key = {.pin_bit_mask = 1ULL << BMC_PIN_ESP_WAKE,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_err_t err = gpio_config(&key);
    if (err != ESP_OK) return err;
    err = gpio_set_level(BMC_PIN_APP_CORE_DIS, 0);
    if (err != ESP_OK) return err;
    gpio_config_t core = {.pin_bit_mask = 1ULL << BMC_PIN_APP_CORE_DIS,
        .mode = GPIO_MODE_INPUT_OUTPUT};
    err = gpio_config(&core);
    if (err != ESP_OK) return err;
    incoming = xQueueCreate(2, FRAME_SIZE);
    if (!incoming) return ESP_ERR_NO_MEM;
    key_raw = key_stable = gpio_get_level(BMC_PIN_ESP_WAKE) == 0;
    key_changed = esp_timer_get_time() / 1000;
    published = replies[0];
    bmc_runtime_reset();
    return ESP_OK;
}
