#include "spl_nand.h"
#include "app_ota.h"
#include "bmc_runtime.h"
#include "fel_spl.h"

#include <stdint.h>
#include <string.h>
#include "board_pins.h"
#include "driver/spi_slave.h"
#include "esp_private/spi_slave_internal.h"
#include "esp_attr.h"
#include "esp_timer.h"
#if CONFIG_BMC_FAST_SPI_SLAVE
#include "fast_spi_slave.h"
#endif
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SLOT_SIZE 0x20000u
#define PAGE_SIZE 0x800u
#define PAGE_COUNT (SLOT_SIZE / PAGE_SIZE)
#define HEADER_SIZE 4u
#define FRAME_SIZE (HEADER_SIZE + PAGE_SIZE)
#define CHECKSUM_STAMP 0x5f0a6c39u

static const char *TAG = "spl_nand";
#define CACHE_COUNT 5u
#define INVALID_PAGE UINT32_MAX
static DMA_ATTR uint8_t cache_frames[CACHE_COUNT][FRAME_SIZE];
static uint32_t cache_tags[CACHE_COUNT];
static portMUX_TYPE cache_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t cache_writer;
static TaskHandle_t cache_task;
static const uint8_t *mapped_slots, *slot_data;
static esp_partition_mmap_handle_t mapping;
static uint32_t image_length, cache_epoch, requested_page;
static int selected_cache = -1;
static bool cache_enabled;
static DMA_ATTR uint8_t empty_frame[FRAME_SIZE];
static DMA_ATTR uint8_t rx_frame[FRAME_SIZE];
static spi_slave_transaction_t transaction;
static volatile bool armed;
static volatile spl_nand_stats_t stats;
static DMA_ATTR uint8_t fel_frame[FRAME_SIZE];
static volatile bool serve_fel;
static bool initialized;
static esp_err_t image_error;
static unsigned loaded_slot = CONFIG_BMC_SPL_SLOT;
static volatile unsigned trace_count;
static volatile bool trace_enabled;
static volatile spl_nand_trace_t trace[SPL_NAND_TRACE_COUNT];

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void copy_page(unsigned index, unsigned page)
{
    uint8_t *frame = cache_frames[index];
    size_t offset = page * PAGE_SIZE;
    size_t count = image_length - offset;
    if (count > PAGE_SIZE) count = PAGE_SIZE;
    memset(frame, 0xff, FRAME_SIZE);
    frame[2] = 0;
    memcpy(frame + HEADER_SIZE, slot_data + offset, count);
}

static void prefetch_pages(void)
{
    xSemaphoreTake(cache_writer, portMAX_DELAY);
    portENTER_CRITICAL(&cache_lock);
    uint32_t epoch = cache_epoch, page = requested_page;
    bool enabled = cache_enabled;
    portEXIT_CRITICAL(&cache_lock);
    for (uint32_t wanted = page; enabled && wanted <= page + 2 && wanted * PAGE_SIZE < image_length; ++wanted) {
        portENTER_CRITICAL(&cache_lock);
        if (!cache_enabled || epoch != cache_epoch || requested_page != page) {
            portEXIT_CRITICAL(&cache_lock);
            break;
        }
        int available = -1;
        bool found = false;
        for (unsigned i = 0; i < CACHE_COUNT; ++i) {
            if (cache_tags[i] == wanted) found = true;
            if (i && (int)i != selected_cache && (cache_tags[i] == INVALID_PAGE ||
                cache_tags[i] + 1 < page || cache_tags[i] > page + 2)) available = i;
        }
        if (found || available < 0) { portEXIT_CRITICAL(&cache_lock); continue; }
        cache_tags[available] = INVALID_PAGE;
        portEXIT_CRITICAL(&cache_lock);
        copy_page((unsigned)available, wanted);
        portENTER_CRITICAL(&cache_lock);
        if (cache_enabled && epoch == cache_epoch) cache_tags[available] = wanted;
        portEXIT_CRITICAL(&cache_lock);
    }
    xSemaphoreGive(cache_writer);
}

static void cache_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        prefetch_pages();
    }
}

static void cache_quiesce(void)
{
    portENTER_CRITICAL(&cache_lock);
    cache_enabled = false;
    ++cache_epoch;
    portEXIT_CRITICAL(&cache_lock);
    if (cache_writer) {
        xSemaphoreTake(cache_writer, portMAX_DELAY);
        xSemaphoreGive(cache_writer);
    }
}

static esp_err_t load_frames(unsigned slot)
{
    if (slot >= 2) return ESP_ERR_INVALID_ARG;
    cache_quiesce();
    if (!cache_writer) cache_writer = xSemaphoreCreateMutex();
    if (!cache_writer) return ESP_ERR_NO_MEM;
    if (!cache_task && xTaskCreate(cache_worker, "spl_cache", 2048, NULL, 24, &cache_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (!mapped_slots) {
        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "d1s_spl");
        if (!part || part->size < 2 * SLOT_SIZE) return ESP_ERR_NOT_FOUND;
        esp_err_t err = esp_partition_mmap(part, 0, 2 * SLOT_SIZE, ESP_PARTITION_MMAP_DATA,
                                         (const void **)&mapped_slots, &mapping);
        if (err != ESP_OK) return err;
    }
    xSemaphoreTake(cache_writer, portMAX_DELAY);
    for (unsigned i = 0; i < CACHE_COUNT; ++i) cache_tags[i] = INVALID_PAGE;
    selected_cache = -1;
    slot_data = mapped_slots + slot * SLOT_SIZE;
    image_length = get_le32(slot_data + 16);
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (memcmp(slot_data + 4, "eGON.BT0", 8) || image_length < 32 ||
        image_length > SLOT_SIZE || (image_length & 0x3ff)) goto done;
    uint32_t checksum = 0;
    for (unsigned i = 0; i < image_length; i += 4)
        checksum += i == 12 ? CHECKSUM_STAMP : get_le32(slot_data + i);
    err = ESP_ERR_INVALID_CRC;
    if (checksum != get_le32(slot_data + 12)) goto done;
    for (unsigned i = 0; i < 3 && i * PAGE_SIZE < image_length; ++i) {
        copy_page(i, i);
        cache_tags[i] = i;
    }
    requested_page = 0;
    loaded_slot = slot;
    err = ESP_OK;
    ESP_LOGI(TAG, "SPL slot %u: %lu bytes, checksum OK; %u cached pages", slot,
             (unsigned long)image_length, CACHE_COUNT);
done:
    if (err != ESP_OK) image_length = 0;
    xSemaphoreGive(cache_writer);
    return err;
}

static const uint8_t *IRAM_ATTR select_page(unsigned page)
{
    const uint8_t *frame = empty_frame;
    bool notify = false;
    portENTER_CRITICAL_ISR(&cache_lock);
    selected_cache = -1;
    if (cache_enabled && page * PAGE_SIZE < image_length) {
        for (unsigned i = 0; i < CACHE_COUNT; ++i) {
            if (cache_tags[i] == page) { selected_cache = i; frame = cache_frames[i]; break; }
        }
        if (selected_cache < 0) stats.cache_misses++;
        requested_page = page;
        notify = true;
    }
    portEXIT_CRITICAL_ISR(&cache_lock);
    if (notify) {
        BaseType_t wake = pdFALSE;
        vTaskNotifyGiveFromISR(cache_task, &wake);
        if (wake) portYIELD_FROM_ISR();
    }
    return frame;
}

static void IRAM_ATTR unselect_page(void)
{
    portENTER_CRITICAL_ISR(&cache_lock);
    selected_cache = -1;
    portEXIT_CRITICAL_ISR(&cache_lock);
}

static void IRAM_ATTR transaction_ready(spi_slave_transaction_t *trans)
{
    (void)trans;
    armed = true;
}

static void IRAM_ATTR transaction_done(spi_slave_transaction_t *trans)
{
    if (trace_enabled && trace_count < SPL_NAND_TRACE_COUNT) {
        unsigned n = trace_count;
        trace[n].cycles = (uint32_t)esp_timer_get_time();
        trace[n].bits = trans->trans_len;
        trace[n].rx = *(uint32_t *)rx_frame;
        trace[n].tx = *(const uint32_t *)((const uint8_t *)trans->tx_buffer + HEADER_SIZE);
        trace_count = n + 1;
    }
    /* The APP can reset itself without going through our GPIO reset path. */
    if (trans->trans_len == 8) bmc_runtime_boot_seen();
    const uint8_t *ota = bmc_runtime_spi_done(rx_frame, trans->trans_len);
    if (!ota) ota = app_ota_spi_done(rx_frame, trans->trans_len);
    if (ota) {
        unselect_page();
        trans->tx_buffer = ota;
#if !CONFIG_BMC_FAST_SPI_SLAVE
        stats.queue_error = spi_slave_queue_trans_isr(SPI2_HOST, trans);
#endif
        return;
    }
    size_t bytes = trans->trans_len / 8;
    /* GDMA may discard a short RX tail; ROM reset/status are identified by length. */
    if (bytes == 1) {
        unselect_page();
        trans->tx_buffer = empty_frame;
    } else if (bytes == 4 && rx_frame[0] == 0x13) {
        stats.page_reads++;
        uint32_t row = (uint32_t)rx_frame[1] << 16 |
                       (uint32_t)rx_frame[2] << 8 | rx_frame[3];
        /* ROM scans eight 128 KiB copies; all expose the selected validated slot. */
        unselect_page();
        trans->tx_buffer = empty_frame;
        if (row < 8 * PAGE_COUNT) {
            if (serve_fel)
                trans->tx_buffer = row % PAGE_COUNT == 0 ? fel_frame : empty_frame;
            else
                trans->tx_buffer = select_page(row % PAGE_COUNT);
        }
    } else if (bytes == FRAME_SIZE && rx_frame[0] == 0x0b &&
               rx_frame[1] == 0 && rx_frame[2] == 0) {
        stats.cache_reads++;
    } else if (bytes != 3) {
        stats.unexpected++;
        unselect_page();
        trans->tx_buffer = empty_frame;
    }
    /* The ROM can clock the next command before a task could re-arm the slave. */
#if !CONFIG_BMC_FAST_SPI_SLAVE
    stats.queue_error = spi_slave_queue_trans_isr(SPI2_HOST, trans);
#endif
}

#if CONFIG_BMC_FAST_SPI_SLAVE
static const uint8_t *IRAM_ATTR fast_transaction_done(size_t bits, void *ctx)
{
    (void)ctx;
    transaction.trans_len = bits;
    transaction_done(&transaction);
    return transaction.tx_buffer;
}
#endif

esp_err_t spl_nand_prepare(void)
{
    if (initialized)
        return ESP_ERR_INVALID_STATE;
    gpio_set_level(BMC_PIN_APP_RESET, 0);
    gpio_config_t reset = {
        .pin_bit_mask = 1ULL << BMC_PIN_APP_RESET,
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    esp_err_t err = gpio_config(&reset);
    if (err != ESP_OK)
        return err;
    image_error = load_frames(CONFIG_BMC_SPL_SLOT);
    if (image_error != ESP_OK)
        ESP_LOGE(TAG, "SPL unavailable: %s; FEL recovery remains available", esp_err_to_name(image_error));

    memset(empty_frame, 0xff, sizeof(empty_frame));
    empty_frame[2] = 0;
    memcpy(fel_frame, empty_frame, FRAME_SIZE);
    memcpy(fel_frame + HEADER_SIZE, fel_spl_image, sizeof(fel_spl_image));
#if CONFIG_BMC_FAST_SPI_SLAVE
    err = fast_spi_slave_init(rx_frame, fast_transaction_done, NULL);
#else
    spi_bus_config_t bus = {
        .mosi_io_num = BMC_PIN_SPI_MOSI,
        .miso_io_num = BMC_PIN_SPI_MISO,
        .sclk_io_num = BMC_PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_SIZE,
        .intr_flags = ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
    };
    spi_slave_interface_config_t slave = {
        .spics_io_num = BMC_PIN_SPI_CS,
        .mode = 0,
        .queue_size = 1,
        .flags = SPI_SLAVE_NO_RETURN_RESULT,
        .post_setup_cb = transaction_ready,
        .post_trans_cb = transaction_done,
    };
    err = spi_slave_initialize(SPI2_HOST, &bus, &slave, SPI_DMA_CH_AUTO);
#endif
    if (err != ESP_OK) return err;
    initialized = true;
    return ESP_OK;
}

esp_err_t spl_nand_hold(void)
{
    esp_err_t err = gpio_set_level(BMC_PIN_APP_RESET, 0);
    if (err == ESP_OK) {
        bmc_runtime_reset();
        cache_quiesce();
    }
    return err;
}

esp_err_t spl_nand_boot(bool fel)
{
    if (!initialized)
        return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(spl_nand_hold(), TAG, "hold reset");
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
#if CONFIG_BMC_FAST_SPI_SLAVE
    ESP_RETURN_ON_ERROR(fast_spi_slave_pause(), TAG, "pause SPI");
#else
    ESP_RETURN_ON_ERROR(spi_slave_queue_reset(SPI2_HOST), TAG, "reset SPI queue");
#endif
    if (!fel) {
        image_error = load_frames(loaded_slot);
        if (image_error != ESP_OK) return image_error;
    }
    app_ota_spi_reset();
    serve_fel = fel;
    portENTER_CRITICAL(&cache_lock);
    selected_cache = -1;
    cache_enabled = !fel;
    portEXIT_CRITICAL(&cache_lock);
    stats = (spl_nand_stats_t){0};
    trace_count = 0;
    armed = false;
    transaction = (spi_slave_transaction_t) {
        .length = FRAME_SIZE * 8,
        .tx_buffer = empty_frame,
        .rx_buffer = rx_frame,
    };
#if CONFIG_BMC_FAST_SPI_SLAVE
    ESP_RETURN_ON_ERROR(fast_spi_slave_arm(empty_frame), TAG, "arm fast SPI");
    armed = true;
#else
    ESP_RETURN_ON_ERROR(spi_slave_queue_trans(SPI2_HOST, &transaction, portMAX_DELAY), TAG, "arm SPI");
#endif
    for (unsigned wait = 0; !armed && wait < 100; wait++)
        vTaskDelay(1);
    if (!armed)
        return ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "SPI ready; releasing D1s reset, image=%s", fel ? "FEL" : "SPL");
    ESP_RETURN_ON_ERROR(gpio_set_level(BMC_PIN_APP_RESET, 1), TAG, "release reset");
    return ESP_OK;
}

bool spl_nand_is_fel(void)
{
    return serve_fel;
}

esp_err_t spl_nand_start(void)
{
    esp_err_t err = spl_nand_prepare();
    return err == ESP_OK ? spl_nand_boot(false) : err;
}

void spl_nand_get_stats(spl_nand_stats_t *snapshot)
{
    *snapshot = stats;
}

bool spl_nand_get_trace(unsigned index, spl_nand_trace_t *entry)
{
    if (index >= trace_count) return false;
    *entry = trace[index];
    return true;
}

esp_err_t spl_nand_enter_fel(void)
{
#if CONFIG_BMC_FAST_SPI_SLAVE
    if (!initialized) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(spl_nand_hold(), TAG, "hold for FEL");
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    ESP_RETURN_ON_ERROR(fast_spi_slave_pause(), TAG, "disable boot NAND");
    serve_fel = true;
    stats = (spl_nand_stats_t){0};
    trace_count = 0;
    return gpio_set_level(BMC_PIN_APP_RESET, 1);
#else
    return spl_nand_boot(true);
#endif
}

void spl_nand_trace_enable(bool enabled)
{
    trace_enabled = enabled;
}


esp_err_t spl_nand_boot_slot(unsigned slot)
{
    if (slot >= 2 || !initialized) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(spl_nand_hold(), TAG, "hold for slot switch");
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
#if CONFIG_BMC_FAST_SPI_SLAVE
    ESP_RETURN_ON_ERROR(fast_spi_slave_pause(), TAG, "pause for slot switch");
#else
    ESP_RETURN_ON_ERROR(spi_slave_queue_reset(SPI2_HOST), TAG, "pause for slot switch");
#endif
    loaded_slot = slot;
    return spl_nand_boot(false);
}
