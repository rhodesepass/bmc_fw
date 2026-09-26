#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef int esp_err_t;
typedef int gpio_num_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define IRAM_ATTR
#define DRAM_ATTR
#define DMA_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
#define portYIELD_FROM_ISR() ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define pdMS_TO_TICKS(ms) (ms)
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_PULLUP_ENABLE 1
#define CONFIG_BMC_SPL_SLOT 0

typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en;
} gpio_config_t;
typedef struct { char version[32]; } esp_app_desc_t;
typedef struct { unsigned count; unsigned head; uint8_t data[2][2052]; } fake_queue_t;
typedef fake_queue_t *QueueHandle_t;
static fake_queue_t fake_queue;
static int64_t fake_us;
static int fake_key = 1, fake_core;
static int uart_direction = GPIO_MODE_OUTPUT, miso_direction = GPIO_MODE_OUTPUT;
static bool uart_routed = true, spi_routed = true;
#define SPI2_HOST 1
#define UART_NUM_0 0
#define UART_PIN_NO_CHANGE -1
static const struct { int spiq_out; } spi_periph_signal[2] = {{0}, {19}};
static int cuts, holds, boots;
static bool mainsys = true, ota_active;
static esp_err_t stop_error, hold_error;
static uint32_t next_random = 100;

static void fake_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
static const char *esp_err_to_name(esp_err_t err) { return err ? "ERROR" : "OK"; }
static int64_t esp_timer_get_time(void) { return fake_us; }
static uint32_t esp_random(void) { return ++next_random; }
static const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t descriptor = {.version = "runtime-host-v1"};
    return &descriptor;
}
static esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
static void vTaskDelay(unsigned ticks) { fake_us += ticks * 1000; }
static esp_err_t gpio_set_direction(gpio_num_t pin, int direction)
{
    if (pin == 21) { uart_direction = direction; uart_routed = false; }
    else if (pin == 7) { miso_direction = direction; spi_routed = false; }
    else assert(false);
    return ESP_OK;
}
static void esp_rom_gpio_connect_out_signal(int pin, int signal, bool invert, bool invert_enable)
{
    assert(pin == 7 && signal == 19 && !invert && !invert_enable);
    spi_routed = true;
}
static esp_err_t uart_set_pin(int uart, int tx, int rx, int rts, int cts)
{
    assert(uart == 0 && tx == 21 && rx == 20 && rts == -1 && cts == -1);
    uart_direction = GPIO_MODE_OUTPUT;
    uart_routed = true;
    return ESP_OK;
}
static int gpio_get_level(gpio_num_t pin) { (void)pin; return fake_key; }
static esp_err_t gpio_set_level(gpio_num_t pin, int level)
{
    if (pin == 18) fake_core = level;
    return ESP_OK;
}
static QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{
    assert(count == 2 && size == 2052);
    memset(&fake_queue, 0, sizeof(fake_queue));
    return &fake_queue;
}
static void xQueueReset(QueueHandle_t queue) { queue->count = queue->head = 0; }
static BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *data, BaseType_t *wake)
{
    *wake = pdFALSE;
    if (queue->count == 2) return pdFALSE;
    memcpy(queue->data[(queue->head + queue->count++) % 2], data, 2052);
    return pdTRUE;
}
static void (*receive_hook)(void);
static BaseType_t xQueueReceive(QueueHandle_t queue, void *data, unsigned timeout)
{
    (void)timeout;
    if (!queue->count) return pdFALSE;
    memcpy(data, queue->data[queue->head], 2052);
    queue->head = (queue->head + 1) % 2;
    queue->count--;
    if (receive_hook) receive_hook();
    return pdTRUE;
}
static uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, uint32_t length)
{
    crc = ~crc;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}
