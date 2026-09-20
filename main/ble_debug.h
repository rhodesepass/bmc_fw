#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    esp_err_t (*uart_write)(const uint8_t *data, size_t len);
    esp_err_t (*command)(const char *request, char *response, size_t capacity);
} ble_debug_callbacks_t;

/* Initialize NVS before this function. Callbacks run on a dedicated worker task. */
esp_err_t ble_debug_init(const ble_debug_callbacks_t *callbacks);
esp_err_t ble_debug_publish_uart(const uint8_t *data, size_t len);
esp_err_t ble_debug_publish_log(const uint8_t *data, size_t len);
bool ble_debug_connected(void);
bool ble_debug_uart_subscribed(void);
bool ble_debug_log_subscribed(void);
uint16_t ble_debug_mtu(void);
uint32_t ble_debug_dropped(void);
