#include "bq25601.h"

#include "bmc_i2c.h"
#include "esp_check.h"
#include "esp_log.h"

#define BQ25601_ADDR           0x6B
#define BQ25601_REG_POC        0x01
#define BQ25601_REG_CTTC       0x05
#define BQ25601_REG_SS         0x08
#define BQ25601_REG_FAULT      0x09
#define BQ25601_REG_PART       0x0B

#define BQ25601_POC_WD_RST     (1u << 6)
#define BQ25601_CTTC_WDT_MASK  (0x3u << 4)

#define I2C_TIMEOUT_MS         100

static const char *TAG = "bq25601";
static i2c_master_dev_handle_t s_dev;

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
    if (next == cur) {
        return ESP_OK;
    }
    return reg_write(reg, next);
}

esp_err_t bq25601_init(void)
{
    ESP_RETURN_ON_ERROR(bmc_i2c_init(), TAG, "i2c");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ25601_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bmc_i2c_bus(), &dev_cfg, &s_dev), TAG, "add dev");

    uint8_t part = 0;
    esp_err_t err = reg_read(BQ25601_REG_PART, &part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02x (%s)", BQ25601_ADDR, esp_err_to_name(err));
        return err;
    }

    /* Host mode: kick WDT once, then disable so we don't need periodic kicks yet. */
    ESP_RETURN_ON_ERROR(reg_update(BQ25601_REG_POC, BQ25601_POC_WD_RST, BQ25601_POC_WD_RST), TAG, "wd rst");
    ESP_RETURN_ON_ERROR(reg_update(BQ25601_REG_CTTC, BQ25601_CTTC_WDT_MASK, 0), TAG, "wd off");

    ESP_LOGI(TAG, "ok, PART=0x%02x", part);
    return ESP_OK;
}

esp_err_t bq25601_read_status(bq25601_status_t *out)
{
    if (!out || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t ss = 0, fault = 0, part = 0;
    ESP_RETURN_ON_ERROR(reg_read(BQ25601_REG_SS, &ss), TAG, "ss");
    ESP_RETURN_ON_ERROR(reg_read(BQ25601_REG_FAULT, &fault), TAG, "fault");
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
