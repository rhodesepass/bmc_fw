#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t bat_adc_init(void);
/* Battery pack voltage in mV (after divider restore). */
esp_err_t bat_adc_read_mv(uint32_t *vbat_mv);
