#include "bat_adc.h"

#include "board_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

/* +BATT -- R33 200k -- BAT_ADC -- R34 60.4k -- GND  =>  Vbat = Vadc * 260.4/60.4 */
#define BAT_ADC_DIV_NUM   651
#define BAT_ADC_DIV_DEN   151
#define BAT_ADC_SAMPLES   16
#define BAT_ADC_ATTEN     ADC_ATTEN_DB_2_5

static const char *TAG = "bat_adc";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static adc_unit_t s_unit;
static bool s_calibrated;

static bool cali_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool ok = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cfg, &handle);
    if (ret == ESP_OK) {
        ok = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!ok) {
        adc_cali_line_fitting_config_t cfg = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cfg, &handle);
        if (ret == ESP_OK) {
            ok = true;
        }
    }
#endif

    *out = handle;
    if (!ok) {
        ESP_LOGW(TAG, "no ADC cali (%s), using raw scale", esp_err_to_name(ret));
    }
    return ok;
}

esp_err_t bat_adc_init(void)
{
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BMC_PIN_BAT_ADC, &s_unit, &s_chan), TAG, "io map");

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = s_unit,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_cfg, &s_adc), TAG, "unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BAT_ADC_ATTEN,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "chan");

    s_calibrated = cali_init(s_unit, s_chan, BAT_ADC_ATTEN, &s_cali);
    ESP_LOGI(TAG, "GPIO%d ADC%d CH%d div=%d/%d cali=%d",
             (int)BMC_PIN_BAT_ADC, (int)s_unit + 1, (int)s_chan,
             BAT_ADC_DIV_NUM, BAT_ADC_DIV_DEN, (int)s_calibrated);
    return ESP_OK;
}

esp_err_t bat_adc_read_mv(uint32_t *vbat_mv)
{
    if (!vbat_mv || !s_adc) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t acc = 0;
    for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, s_chan, &raw), TAG, "read");
        acc += raw;
    }
    int raw_avg = (int)(acc / BAT_ADC_SAMPLES);

    int vadc_mv = 0;
    if (s_calibrated) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw_avg, &vadc_mv), TAG, "cali");
    } else {
        /* 12-bit full-scale ~1050mV @ 2.5dB atten, rough fallback */
        vadc_mv = (raw_avg * 1050) / 4095;
    }

    *vbat_mv = (uint32_t)vadc_mv * BAT_ADC_DIV_NUM / BAT_ADC_DIV_DEN;
    return ESP_OK;
}
