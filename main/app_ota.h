#pragma once
#include "app_ota_wire.h"
#include "esp_err.h"
esp_err_t app_ota_init(void);
esp_err_t app_ota_upload(const uint8_t *data, size_t len);
/* Retries must retain the entire original frame, including its session and sequence. */
esp_err_t app_ota_submit_frame(const uint8_t *data, size_t len);
void app_ota_get_status(app_ota_status_t *out);
void app_ota_wait_status(uint32_t timeout_ms);
bool app_ota_pending(void);
esp_err_t app_ota_command(const char *cmd, char *response, size_t capacity);
void app_ota_spi_reset(void);
/* ISR returns NULL for normal NAND traffic. */
const uint8_t *app_ota_spi_done(const uint8_t *rx, size_t bits);

void app_ota_normal_boot(void);
