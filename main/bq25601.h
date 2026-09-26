#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BQ25601_VBUS_NONE = 0,
    BQ25601_VBUS_USB_SDP,
    BQ25601_VBUS_USB_CDP,
    BQ25601_VBUS_USB_DCP,
    BQ25601_VBUS_UNKNOWN,
    BQ25601_VBUS_USB_OTG = 7,
} bq25601_vbus_t;

typedef enum {
    BQ25601_CHG_NOT_CHARGING = 0,
    BQ25601_CHG_PRECHARGE,
    BQ25601_CHG_FAST,
    BQ25601_CHG_DONE,
} bq25601_chg_t;

typedef struct {
    bq25601_vbus_t vbus;
    bq25601_chg_t chg;
    bool power_good;
    bool therm_regulation;
    bool vsys_min_hit;
    uint8_t fault; /* REG09 raw */
    uint8_t fault_latched;
    uint8_t part_info;
} bq25601_status_t;

typedef struct {
    bool enabled;
    bool verified;
    uint16_t input_ma;
    uint16_t charge_ma;
    uint16_t voltage_mv;
    uint16_t precharge_ma;
    uint16_t termination_ma;
} bq25601_config_t;

esp_err_t bq25601_init(void);
esp_err_t bq25601_poll(void);
esp_err_t bq25601_read_config(bq25601_config_t *out);
esp_err_t bq25601_read_status(bq25601_status_t *out);
const char *bq25601_chg_str(bq25601_chg_t chg);
const char *bq25601_vbus_str(bq25601_vbus_t vbus);
