#include "bmc_debug.h"
#include "ble_debug.h"
#include "board_pins.h"
#include "spl_nand.h"
#include "app_ota.h"
#include "bmc_ota.h"
#include "bmc_link.h"
#include "bq25601.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "bmc_runtime.h"
#include "bmc_power.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "soc/efuse_reg.h"
#include "soc/rtc_cntl_reg.h"

static const char *TAG = "bmc_debug";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t uart_history[16384], log_history[8192];
struct history {
    uint8_t *data;
    size_t capacity;
    uint64_t head, cursor, lost;
};
static struct history uart_log = {uart_history, sizeof(uart_history), 0, 0, 0};
static struct history bmc_log = {log_history, sizeof(log_history), 0, 0, 0};
static int64_t download_at;
static bool reset_d1_on_download;
static bool fel_default, held = true, boot_decided;
static SemaphoreHandle_t control_lock;
static bool self_ota_active;
static esp_err_t spi_error = ESP_ERR_INVALID_STATE;

static void append(struct history *h, const uint8_t *data, size_t len)
{
    portENTER_CRITICAL(&lock);
    if (len > h->capacity) {
        size_t skip = len - h->capacity;
        data += skip;
        h->head += skip;
        len = h->capacity;
    }
    size_t pos = (size_t)h->head & (h->capacity - 1);
    size_t first = len < h->capacity - pos ? len : h->capacity - pos;
    memcpy(h->data + pos, data, first);
    memcpy(h->data, data + first, len - first);
    h->head += len;

    portEXIT_CRITICAL(&lock);
}

static int log_output(const char *format, va_list args)
{
    char text[384];
    int n = vsnprintf(text, sizeof(text), format, args);
    if (n > 0)
        append(&bmc_log, (const uint8_t *)text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return n;
}

static void replay(struct history *h)
{
    portENTER_CRITICAL(&lock);
    h->cursor = h->head > h->capacity ? h->head - h->capacity : 0;
    portEXIT_CRITICAL(&lock);
}

static void send_history(struct history *h, esp_err_t (*publish)(const uint8_t *, size_t))
{
    uint8_t bytes[244];
    size_t maximum = ble_debug_mtu() > 3 ? ble_debug_mtu() - 3 : 20;
    if (maximum > sizeof(bytes)) maximum = sizeof(bytes);
    portENTER_CRITICAL(&lock);
    if (h->head - h->cursor > h->capacity) {
        uint64_t next = h->head - h->capacity;
        h->lost += next - h->cursor;
        h->cursor = next;
    }
    uint64_t cursor = h->cursor;
    size_t n = h->head - cursor;
    if (n > maximum) n = maximum;
    size_t pos = (size_t)cursor & (h->capacity - 1);
    size_t first = n < h->capacity - pos ? n : h->capacity - pos;
    memcpy(bytes, h->data + pos, first);
    memcpy(bytes + first, h->data, n - first);
    portEXIT_CRITICAL(&lock);
    if (n && publish(bytes, n) == ESP_OK) {
        portENTER_CRITICAL(&lock);
        if (h->cursor == cursor) h->cursor += n;
        portEXIT_CRITICAL(&lock);
    }
}

static esp_err_t persist_mode(bool fel)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("bmc_debug", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "fel", fel);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) fel_default = fel;
    return err;
}

static esp_err_t uart_input(const uint8_t *bytes, size_t len)
{
    return uart_write_bytes(UART_NUM_0, bytes, len) == (int)len ? ESP_OK : ESP_FAIL;
}

static esp_err_t run_command(const char *request, char *response, size_t capacity)
{
    esp_err_t err = ESP_OK;
    if (self_ota_active && (!strcmp(request, "ota-finish") || !strcmp(request, "ota-reboot") ||
                            !strcmp(request, "ota-abort"))) {
        if (!bmc_ota_staged() && !bmc_ota_committed()) {
            snprintf(response, capacity, "BMC upload still in progress");
            return ESP_ERR_INVALID_STATE;
        }
        if (!strcmp(request, "ota-finish")) err = bmc_ota_commit();
        else if (!strcmp(request, "ota-reboot")) err = bmc_ota_reboot();
        else {
            err = bmc_ota_stage_abort();
            if (err == ESP_OK) self_ota_active = false;
        }
        snprintf(response, capacity, "%s: %s", request, esp_err_to_name(err));
        return err;
    }
    if (self_ota_active && strcmp(request, "status") && strcmp(request, "power-status") &&
        strcmp(request, "wifi-status") && strcmp(request, "ota-status") && strcmp(request, "charger-status")) {
        snprintf(response, capacity, "BMC firmware update in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (bmc_ota_committed() && strcmp(request, "status") && strcmp(request, "power-status") &&
        strcmp(request, "wifi-status") && strcmp(request, "ota-status") && strcmp(request, "charger-status") &&
        strcmp(request, "ota-finish") && strcmp(request, "ota-reboot")) {
        snprintf(response, capacity, "BMC image committed; use ota-reboot");
        return ESP_ERR_INVALID_STATE;
    }
    if (!strcmp(request, "charger-status")) {
        bq25601_config_t c;
        bq25601_status_t s;
        err = bq25601_read_config(&c);
        if (err == ESP_OK) err = bq25601_read_status(&s);
        if (err != ESP_OK) {
            snprintf(response, capacity, "charger state unknown: %s", esp_err_to_name(err));
            return err;
        }
        snprintf(response, capacity,
                 "verified=%d enabled=%d input_ma=%u charge_ma=%u voltage_mv=%u pre_ma=%u term_ma=%u vbus=%s chg=%s pg=%d thermal=%d fault=0x%02x latched=0x%02x",
                 c.verified, c.enabled, c.input_ma, c.charge_ma, c.voltage_mv,
                 c.precharge_ma, c.termination_ma, bq25601_vbus_str(s.vbus),
                 bq25601_chg_str(s.chg), s.power_good, s.therm_regulation, s.fault, s.fault_latched);
        return ESP_OK;
    }
    if (!strcmp(request, "power-status")) {
        bmc_runtime_status(response, capacity);
        return ESP_OK;
    }
    if (!strcmp(request, "boot") || !strcmp(request, "fel") ||
        !strcmp(request, "reset") || !strcmp(request, "hold") ||
        !strcmp(request, "c3-download") || !strcmp(request, "rescue-download"))
        boot_decided = true;
    if (!strncmp(request, "wifi-", 5))
        return bmc_link_command(request, response, capacity);
    if (!strcmp(request, "ota") || !strncmp(request, "ota-", 4)) {
        if (strcmp(request, "ota-status") && strcmp(request, "charger-status")) boot_decided = true;
        return app_ota_command(request, response, capacity);
    }
    if (app_ota_pending() && (!strcmp(request, "boot") || !strcmp(request, "reset"))) {
        snprintf(response, capacity, "OTA pending: use ota-rescue; complete upload and ota-finish before boot");
        return ESP_ERR_INVALID_STATE;
    }
    if (!strcmp(request, "status")) {
        spl_nand_stats_t s;
        spl_nand_get_stats(&s);
        portENTER_CRITICAL(&lock);
        uint64_t rx = uart_log.head, lost = uart_log.lost, logs = bmc_log.head;
        portEXIT_CRITICAL(&lock);
        snprintf(response, capacity,
                 "mode=%s held=%d spi=%s page=%lu cache=%lu unexpected=%lu cache_miss=%lu queue=%s uart=%llu lost=%llu log=%llu ble_drop=%lu heap_free=%lu heap_largest=%lu reset_reason=%d",
                 fel_default ? "fel" : "spl", held, esp_err_to_name(spi_error),
                 (unsigned long)s.page_reads, (unsigned long)s.cache_reads,
                 (unsigned long)s.unexpected, (unsigned long)s.cache_misses, esp_err_to_name(s.queue_error),
                 (unsigned long long)rx, (unsigned long long)lost,
                 (unsigned long long)logs, (unsigned long)ble_debug_dropped(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (int)esp_reset_reason());
        return ESP_OK;
    }
    if (!strcmp(request, "trace-on") || !strcmp(request, "trace-off")) {
        spl_nand_trace_enable(!strcmp(request, "trace-on"));
        snprintf(response, capacity, "%s", request);
        return ESP_OK;
    }
    if (!strncmp(request, "trace ", 6)) {
        unsigned index;
        char extra;
        if (sscanf(request + 6, "%u%c", &index, &extra) != 1 || index >= SPL_NAND_TRACE_COUNT)
            return ESP_ERR_INVALID_ARG;
        size_t used = 0;
        for (unsigned i = index; i < index + 8; i++) {
            spl_nand_trace_t t;
            if (!spl_nand_get_trace(i, &t)) break;
            int n = snprintf(response + used, capacity - used, "%u %08lx %lu %08lx %08lx\n", i,
                             (unsigned long)t.cycles, (unsigned long)t.bits,
                             (unsigned long)t.rx, (unsigned long)t.tx);
            if (n < 0 || (size_t)n >= capacity - used) break;
            used += n;
        }
        if (!used) snprintf(response, capacity, "end");
        return ESP_OK;
    }
    if (!strcmp(request, "uart-replay")) {
        replay(&uart_log);
    } else if (!strcmp(request, "log-replay")) {
        replay(&bmc_log);
    } else if (!strcmp(request, "hold")) {
        err = spl_nand_hold();
        if (err == ESP_OK) held = true;
    } else if (!strcmp(request, "boot") || !strcmp(request, "fel") || !strcmp(request, "reset")) {
        bool fel = !strcmp(request, "fel") || (!strcmp(request, "reset") && fel_default);
        if (strcmp(request, "reset")) err = persist_mode(fel);
        if (err == ESP_OK) {
            held = true;
            app_ota_normal_boot();
            err = bmc_runtime_power_on();
            if (err == ESP_OK)
                err = fel ? spl_nand_enter_fel() : spl_nand_boot_slot(CONFIG_BMC_SPL_SLOT);
            spi_error = err;
            if (err == ESP_OK) held = false;
        }
    } else if (!strcmp(request, "c3-download") || !strcmp(request, "rescue-download")) {
        bool rescue = !strcmp(request, "rescue-download");
        if ((REG_READ(EFUSE_RD_REPEAT_DATA0_REG) & EFUSE_DIS_FORCE_DOWNLOAD) ||
            (REG_READ(EFUSE_RD_REPEAT_DATA3_REG) & EFUSE_DIS_DOWNLOAD_MODE)) {
            snprintf(response, capacity, "ROM download disabled by eFuse");
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (held && !rescue) {
            snprintf(response, capacity, "D1s held; use fel and load uopbridge first");
            return ESP_ERR_INVALID_STATE;
        }
        /* Leave the running D1s bridge untouched while C3 switches to its ROM. */
        bmc_runtime_reset();
        portENTER_CRITICAL(&lock);
        reset_d1_on_download = rescue;
        download_at = esp_timer_get_time() + 1000000;
        portEXIT_CRITICAL(&lock);
        snprintf(response, capacity, "C3 ROM download in 1s; esptool --before no-reset");
        return ESP_OK;
    } else {
        snprintf(response, capacity, "unknown command");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(response, capacity, "%s: %s", request, esp_err_to_name(err));
    return err;
}

esp_err_t bmc_debug_command(const char *request, char *response, size_t capacity)
{
    if (!control_lock || !request || !response || !capacity) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(control_lock, portMAX_DELAY);
    esp_err_t err = run_command(request, response, capacity);
    xSemaphoreGive(control_lock);
    return err;
}

esp_err_t bmc_debug_self_ota_begin(void)
{
    if (!control_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(control_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (boot_decided && !self_ota_active && !app_ota_pending() && !download_at) {
        self_ota_active = true;
        err = ESP_OK;
    }
    xSemaphoreGive(control_lock);
    return err;
}

void bmc_debug_self_ota_end(void)
{
    xSemaphoreTake(control_lock, portMAX_DELAY);
    self_ota_active = false;
    xSemaphoreGive(control_lock);
}

esp_err_t bmc_debug_link_command(const char *request, char *response, size_t capacity,
                                 uint32_t link_epoch)
{
    if (!control_lock || !request || !response || !capacity) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(control_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (bmc_link_session_valid(link_epoch)) err = run_command(request, response, capacity);
    else snprintf(response, capacity, "Wi-Fi session expired");
    xSemaphoreGive(control_lock);
    return err;
}

static void debug_task(void *arg)
{
    (void)arg;
    esp_task_wdt_user_handle_t control_watchdog;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_ERROR_CHECK(esp_task_wdt_add_user("bmc_control", &control_watchdog));
    uint8_t bytes[256];
    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        int n = uart_read_bytes(UART_NUM_0, bytes, sizeof(bytes), pdMS_TO_TICKS(10));
        if (n > 0) append(&uart_log, bytes, n);
        if (xSemaphoreTake(control_lock, 0) == pdTRUE) {
            bmc_runtime_poll();
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(control_watchdog));
            xSemaphoreGive(control_lock);
        }
        if (ble_debug_uart_subscribed()) send_history(&uart_log, ble_debug_publish_uart);
        if (ble_debug_log_subscribed()) send_history(&bmc_log, ble_debug_publish_log);
        portENTER_CRITICAL(&lock);
        int64_t deadline = download_at;
        bool reset_d1 = reset_d1_on_download;
        portEXIT_CRITICAL(&lock);
        if (deadline && esp_timer_get_time() >= deadline) {
            if (reset_d1) {
                spl_nand_hold();
                vTaskDelay(pdMS_TO_TICKS(20) + 1);
            }
            uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
            REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_rom_software_reset_system();
        }
    }
}

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "heap %s: free=%lu largest=%lu reset_reason=%d", stage,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (int)esp_reset_reason());
}

esp_err_t bmc_debug_init(void)
{
    control_lock = xSemaphoreCreateMutex();
    if (!control_lock) return ESP_ERR_NO_MEM;
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    esp_log_set_vprintf(log_output);
    ESP_LOGI(TAG, "reset reason=%d; hardware UART reserved for APP", esp_reset_reason());
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS");
    nvs_handle_t nvs;
    if (nvs_open("bmc_debug", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t mode = 0;
        if (nvs_get_u8(nvs, "fel", &mode) == ESP_OK) fel_default = mode != 0;
        nvs_close(nvs);
    }
    const uart_config_t uart = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0), TAG, "UART driver");
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_0, &uart), TAG, "UART config");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_0, BMC_PIN_UART_TX, BMC_PIN_UART_RX,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "UART pins");
    ESP_RETURN_ON_ERROR(app_ota_init(), TAG, "OTA init");
    ESP_RETURN_ON_ERROR(bmc_runtime_init(), TAG, "runtime init");
    log_heap("before SPL");
    spi_error = spl_nand_prepare();
    log_heap("after SPL");
    ESP_LOGI(TAG, "D1s SPI prepare: %s", esp_err_to_name(spi_error));
    const ble_debug_callbacks_t callbacks = {.uart_write = uart_input, .command = bmc_debug_command};
    log_heap("before BLE");
    esp_err_t ble_error = ble_debug_init(&callbacks);
    log_heap("after BLE");
    ESP_RETURN_ON_ERROR(ble_error, TAG, "BLE");
    if (xTaskCreate(debug_task, "bmc_uart", 4096, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "BLE debug ready; D1s UART capture armed");
    return ESP_OK;
}

esp_err_t bmc_debug_boot_default(void)
{
    xSemaphoreTake(control_lock, portMAX_DELAY);
    if (boot_decided || spi_error != ESP_OK) {
        esp_err_t result = spi_error;
        xSemaphoreGive(control_lock);
        return result;
    }
    boot_decided = true;
    if (app_ota_pending()) {
        held = true;
        spl_nand_hold();
        xSemaphoreGive(control_lock);
        ESP_LOGW(TAG, "Incomplete OTA: D1s held; use ota-rescue");
        return ESP_OK;
    }
    esp_err_t err = fel_default ? spl_nand_enter_fel() : spl_nand_boot_slot(CONFIG_BMC_SPL_SLOT);
    spi_error = err;
    if (err == ESP_OK) held = false;
    xSemaphoreGive(control_lock);
    return err;
}
