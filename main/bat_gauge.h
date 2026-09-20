#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bq25601.h"
#include "esp_err.h"

typedef enum {
    BAT_POWER_BATTERY = 0,
    BAT_POWER_CHARGING,
    BAT_POWER_FULL,
    BAT_POWER_FAULT,
} bat_power_t;

typedef struct {
    uint32_t vbat_mv;
    uint32_t vbat_filt_mv;
    int soc_pct;           /* 0..100 */
    bat_power_t power;
    bq25601_status_t chg;
    bool valid;
} bat_gauge_snapshot_t;

esp_err_t bat_gauge_init(void);
esp_err_t bat_gauge_update(bat_gauge_snapshot_t *out);
const char *bat_power_str(bat_power_t p);
