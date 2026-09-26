#include "bat_gauge.h"

#include "bat_adc.h"
#include "esp_check.h"
#include "esp_log.h"

/*
 * Voltage→SOC for 1S LiPo (4.4V full) under light load (BMC-only / idle APP).
 * No coulomb counter on BQ25601 — OCV-ish estimate only.
 */
typedef struct {
    uint16_t mv;
    uint8_t pct;
} soc_point_t;

static const soc_point_t s_ocv_table[] = {
    {4400, 100},
    {4300,  95},
    {4200,  85},
    {4100,  75},
    {4000,  62},
    {3900,  50},
    {3800,  38},
    {3700,  28},
    {3600,  18},
    {3500,  10},
    {3400,   5},
    {3300,   2},
    {3000,   0},
};

#define OCV_N (sizeof(s_ocv_table) / sizeof(s_ocv_table[0]))
#define EMA_SHIFT 3          /* alpha = 1/8 */
#define CHARGE_IR_BIAS_MV 40 /* rough charge IR compensation */

static const char *TAG = "bat_gauge";

static uint32_t s_filt_mv;
static int s_soc = -1;
static bool s_inited;

static int mv_to_soc(uint32_t mv)
{
    if (mv >= s_ocv_table[0].mv) {
        return 100;
    }
    if (mv <= s_ocv_table[OCV_N - 1].mv) {
        return 0;
    }

    for (size_t i = 0; i + 1 < OCV_N; i++) {
        uint32_t hi = s_ocv_table[i].mv;
        uint32_t lo = s_ocv_table[i + 1].mv;
        if (mv <= hi && mv >= lo) {
            int pct_hi = s_ocv_table[i].pct;
            int pct_lo = s_ocv_table[i + 1].pct;
            uint32_t span = hi - lo;
            if (span == 0) {
                return pct_hi;
            }
            return pct_lo + (int)((mv - lo) * (pct_hi - pct_lo) / span);
        }
    }
    return 0;
}

static int clamp_soc(int soc)
{
    if (soc < 0) {
        return 0;
    }
    if (soc > 100) {
        return 100;
    }
    return soc;
}

/* One-step hysteresis to kill ADC flicker. */
static int settle_soc(int prev, int now)
{
    if (prev < 0) {
        return now;
    }
    if (now > prev + 1) {
        return prev + 1;
    }
    if (now < prev - 1) {
        return prev - 1;
    }
    return prev;
}

esp_err_t bat_gauge_init(void)
{
    ESP_RETURN_ON_ERROR(bat_adc_init(), TAG, "adc");

    uint32_t mv = 0;
    if (bat_adc_read_mv(&mv) == ESP_OK) {
        s_filt_mv = mv;
        s_soc = mv_to_soc(mv);
    }
    s_inited = true;
    ESP_LOGI(TAG, "ready, seed %lumV ~%d%%", (unsigned long)s_filt_mv, s_soc);
    return ESP_OK;
}

esp_err_t bat_gauge_update(bat_gauge_snapshot_t *out)
{
    if (!out || !s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    bat_gauge_snapshot_t snap = {0};
    esp_err_t adc_err = bat_adc_read_mv(&snap.vbat_mv);
    esp_err_t bq_err = bq25601_read_status(&snap.chg);

    if (adc_err != ESP_OK) {
        snap.valid = false;
        *out = snap;
        return adc_err;
    }

    if (!s_filt_mv) {
        s_filt_mv = snap.vbat_mv;
    } else {
        s_filt_mv += ((int32_t)snap.vbat_mv - (int32_t)s_filt_mv) >> EMA_SHIFT;
    }
    snap.vbat_filt_mv = s_filt_mv;

    uint32_t est_mv = s_filt_mv;
    bool charging = false;
    bool full = false;

    if (bq_err == ESP_OK) {
        charging = (snap.chg.chg == BQ25601_CHG_PRECHARGE || snap.chg.chg == BQ25601_CHG_FAST);
        full = (snap.chg.chg == BQ25601_CHG_DONE);
        if (snap.chg.fault) {
            snap.power = BAT_POWER_FAULT;
        } else if (full) {
            snap.power = BAT_POWER_FULL;
        } else if (charging) {
            snap.power = BAT_POWER_CHARGING;
        } else {
            snap.power = BAT_POWER_BATTERY;
        }
    } else {
        snap.power = BAT_POWER_BATTERY;
        ESP_LOGW(TAG, "bq status failed: %s", esp_err_to_name(bq_err));
    }

    if (full) {
        s_soc = 100;
    } else {
        if (charging) {
            /* Compensate charge IR so SOC doesn't jump high while empty-ish. */
            if (est_mv > CHARGE_IR_BIAS_MV) {
                est_mv -= CHARGE_IR_BIAS_MV;
            }
        }
        s_soc = settle_soc(s_soc, clamp_soc(mv_to_soc(est_mv)));
    }

    snap.soc_pct = s_soc;
    snap.valid = true;
    *out = snap;
    return ESP_OK;
}

const char *bat_power_str(bat_power_t p)
{
    switch (p) {
    case BAT_POWER_BATTERY:  return "batt";
    case BAT_POWER_CHARGING: return "chg";
    case BAT_POWER_FULL:     return "full";
    case BAT_POWER_FAULT:    return "fault";
    default:                 return "?";
    }
}
