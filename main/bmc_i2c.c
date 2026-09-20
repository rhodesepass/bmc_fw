#include "bmc_i2c.h"

#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bmc_i2c";
static i2c_master_bus_handle_t s_bus;

esp_err_t bmc_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BMC_PIN_I2C_SDA,
        .scl_io_num = BMC_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "bus create");
    ESP_LOGI(TAG, "I2C0 SCL=%d SDA=%d", (int)BMC_PIN_I2C_SCL, (int)BMC_PIN_I2C_SDA);
    return ESP_OK;
}

i2c_master_bus_handle_t bmc_i2c_bus(void)
{
    return s_bus;
}
