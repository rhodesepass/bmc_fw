#include "bq25601.h"

#include "bmc_i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BQ25601_ADDR           0x6B
#define BQ25601_REG_POC        0x01
#define BQ25601_REG_CTTC       0x05
#define BQ25601_REG_SS         0x08
#define BQ25601_REG_FAULT      0x09
#define BQ25601_REG_PART       0x0B

#define BQ25601_CHG_ENABLE     (1u << 4)

#define I2C_TIMEOUT_MS         100

static const char *TAG = "bq25601";
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;

static const struct {
    uint8_t reg, mask, value;
} s_profile[] = {
    {0x00, 0x1f, 0x04},
    {0x02, 0x3f, 0x08},
    {0x03, 0xff, 0x10},
    {0x04, 0xf8, 0x58},
    {0x05, 0xbc, 0x8c},
};

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur;
    ESP_RETURN_ON_ERROR(reg_read(reg, &cur), TAG, "read 0x%02x", reg);
    uint8_t next = (cur & ~mask) | (value & mask);
    /* Do not echo the self-clearing watchdog command during a read-modify-write. */
    if (reg == BQ25601_REG_POC) next &= ~(1u << 6);
    if (next == cur) {
        return ESP_OK;
    }
    return reg_write(reg, next);
}

static esp_err_t config_read_locked(bq25601_config_t *out)
{
    *out = (bq25601_config_t){0};
    uint8_t regs[6];
    for (unsigned i = 0; i < sizeof(regs); ++i) {
        esp_err_t err = reg_read(i, &regs[i]);
        if (err != ESP_OK) return err;
    }
    out->enabled = (regs[1] & BQ25601_CHG_ENABLE) != 0;
    out->input_ma = 100 + (regs[0] & 0x1f) * 100;
    unsigned charge = regs[2] & 0x3f;
    out->charge_ma = (charge > 50 ? 50 : charge) * 60;
    unsigned voltage = regs[4] >> 3;
    out->voltage_mv = voltage == 15 ? 4352 : 3856 + (voltage > 24 ? 24 : voltage) * 32;
    unsigned precharge = regs[3] >> 4;
    out->precharge_ma = 60 + (precharge > 12 ? 12 : precharge) * 60;
    out->termination_ma = 60 + (regs[3] & 0x0f) * 60;
    out->verified = (regs[1] & 0x30) == BQ25601_CHG_ENABLE;
    for (unsigned i = 0; i < sizeof(s_profile) / sizeof(s_profile[0]); ++i)
        out->verified &= (regs[s_profile[i].reg] & s_profile[i].mask) == s_profile[i].value;
    return ESP_OK;
}

static esp_err_t fail_closed(esp_err_t cause)
{
    esp_err_t err = reg_update(BQ25601_REG_POC, BQ25601_CHG_ENABLE, 0);
    uint8_t poc = BQ25601_CHG_ENABLE;
    if (err == ESP_OK) err = reg_read(BQ25601_REG_POC, &poc);
    if (err != ESP_OK || (poc & BQ25601_CHG_ENABLE))
        ESP_LOGE(TAG, "charger error %s; charge disable UNCONFIRMED", esp_err_to_name(cause));
    else
        ESP_LOGE(TAG, "charger error %s; charging disabled", esp_err_to_name(cause));
    return cause;
}

static esp_err_t configure_locked(void)
{
    esp_err_t err = reg_update(BQ25601_REG_POC, 0x30, 0);
    uint8_t poc;
    if (err == ESP_OK) err = reg_read(BQ25601_REG_POC, &poc);
    if (err == ESP_OK && (poc & 0x30)) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) return fail_closed(err);

    for (unsigned i = 0; i < sizeof(s_profile) / sizeof(s_profile[0]); ++i) {
        err = reg_update(s_profile[i].reg, s_profile[i].mask, s_profile[i].value);
        if (err != ESP_OK) return fail_closed(err);
    }
    for (unsigned i = 0; i < sizeof(s_profile) / sizeof(s_profile[0]); ++i) {
        uint8_t value;
        err = reg_read(s_profile[i].reg, &value);
        if (err == ESP_OK && (value & s_profile[i].mask) != s_profile[i].value)
            err = ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) return fail_closed(err);
    }
    err = reg_update(BQ25601_REG_POC, 0x30, BQ25601_CHG_ENABLE);
    bq25601_config_t config;
    if (err == ESP_OK) err = config_read_locked(&config);
    if (err == ESP_OK && !config.verified) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) return fail_closed(err);
    ESP_LOGI(TAG, "verified: input=500mA charge=480mA voltage=4208mV pre=120mA term=60mA");
    return ESP_OK;
}

static esp_err_t init_locked(void)
{
    ESP_RETURN_ON_ERROR(bmc_i2c_init(), TAG, "i2c");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ25601_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!s_dev)
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bmc_i2c_bus(), &dev_cfg, &s_dev), TAG, "add dev");

    uint8_t part = 0;
    esp_err_t err = reg_read(BQ25601_REG_PART, &part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02x (%s)", BQ25601_ADDR, esp_err_to_name(err));
        return fail_closed(err);
    }
    if ((part & 0x78) != 0x10) return fail_closed(ESP_ERR_NOT_SUPPORTED);
    ESP_LOGI(TAG, "ok, PART=0x%02x", part);
    bq25601_config_t config;
    err = config_read_locked(&config);
    if (err != ESP_OK) return fail_closed(err);
    return config.verified ? ESP_OK : configure_locked();
}

esp_err_t bq25601_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = init_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bq25601_poll(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    if (!s_dev) {
        err = init_locked();
    } else {
        bq25601_config_t config;
        err = config_read_locked(&config);
        if (err != ESP_OK) err = fail_closed(err);
        else if (!config.verified) err = init_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bq25601_read_config(bq25601_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (bq25601_config_t){0};
    if (!s_lock || !s_dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = config_read_locked(out);
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t status_read_locked(bq25601_status_t *out)
{
    if (!out || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t ss = 0, fault = 0, part = 0;
    ESP_RETURN_ON_ERROR(reg_read(BQ25601_REG_SS, &ss), TAG, "ss");
    ESP_RETURN_ON_ERROR(reg_read(BQ25601_REG_FAULT, &fault), TAG, "fault");
    out->fault_latched = fault;
    ESP_RETURN_ON_ERROR(reg_read(BQ25601_REG_FAULT, &fault), TAG, "current fault");
    ESP_RETURN_ON_ERROR(reg_read(BQ25601_REG_PART, &part), TAG, "part");

    out->vbus = (bq25601_vbus_t)((ss >> 5) & 0x7);
    out->chg = (bq25601_chg_t)((ss >> 3) & 0x3);
    out->power_good = (ss & (1u << 2)) != 0;
    out->therm_regulation = (ss & (1u << 1)) != 0;
    out->vsys_min_hit = (ss & (1u << 0)) != 0;
    out->fault = fault;
    out->part_info = part;
    return ESP_OK;
}

esp_err_t bq25601_read_status(bq25601_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (bq25601_status_t){0};
    if (!s_lock || !s_dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = status_read_locked(out);
    xSemaphoreGive(s_lock);
    return err;
}

const char *bq25601_chg_str(bq25601_chg_t chg)
{
    switch (chg) {
    case BQ25601_CHG_NOT_CHARGING: return "idle";
    case BQ25601_CHG_PRECHARGE:    return "pre";
    case BQ25601_CHG_FAST:         return "fast";
    case BQ25601_CHG_DONE:         return "done";
    default:                       return "?";
    }
}

const char *bq25601_vbus_str(bq25601_vbus_t vbus)
{
    switch (vbus) {
    case BQ25601_VBUS_NONE:    return "none";
    case BQ25601_VBUS_USB_SDP: return "sdp";
    case BQ25601_VBUS_USB_CDP: return "cdp";
    case BQ25601_VBUS_USB_DCP: return "dcp";
    case BQ25601_VBUS_USB_OTG: return "otg";
    default:                   return "unk";
    }
}
