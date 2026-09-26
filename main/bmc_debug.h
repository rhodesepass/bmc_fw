#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
esp_err_t bmc_debug_init(void);
bool bmc_debug_prepare_sleep(void);
esp_err_t bmc_debug_self_ota_begin(void);
void bmc_debug_self_ota_end(void);
esp_err_t bmc_debug_boot_default(void);
esp_err_t bmc_debug_command(const char *request, char *response, size_t capacity);
esp_err_t bmc_debug_link_command(const char *request, char *response, size_t capacity, uint32_t link_epoch);
