#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t bmc_pm_init(void);
esp_err_t bmc_pm_radio_ready(void);
esp_err_t bmc_pm_boot(bool active);
esp_err_t bmc_pm_command(const char *request, char *response, size_t capacity);
