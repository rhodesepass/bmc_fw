#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t bmc_power_init(void);
bool bmc_mainsys_enabled(void);
esp_err_t bmc_mainsys_set(bool enabled);
esp_err_t bmc_mainsys_force_off(void);

/* All Wi-Fi start/stop callers must use these serialized power interlocks. */
esp_err_t bmc_wifi_start(void);
esp_err_t bmc_wifi_stop(void);

esp_err_t bmc_wifi_apply(esp_err_t (*apply)(void *), void *arg);
