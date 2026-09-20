#pragma once
#include "esp_http_server.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

esp_err_t bmc_ota_init(void);
esp_err_t bmc_ota_stage_begin(uint32_t total, const uint8_t hash[32]);
esp_err_t bmc_ota_stage_write(const uint8_t *data, size_t size);
esp_err_t bmc_ota_stage_end(void);
esp_err_t bmc_ota_stage_abort(void);
esp_err_t bmc_ota_commit(void);
esp_err_t bmc_ota_reboot(void);
bool bmc_ota_staged(void);
bool bmc_ota_committed(void);

/* The caller authenticates the request before dispatch. */
esp_err_t bmc_ota_http(httpd_req_t *req, uint32_t epoch);
void bmc_ota_poll(void);
