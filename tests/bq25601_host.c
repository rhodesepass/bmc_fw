#include "bq25601_stub.h"
#include "../main/bq25601.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t regs[12] = {0x17, 0x1a, 0xa2, 0x22, 0xb3, 0x97, 0x66, 0x4c, 0, 0, 0, 0x10};
static const uint8_t masks[] = {0x1f, 0x30, 0x3f, 0xff, 0xf8, 0xbc};
static const uint8_t values[] = {0x04, 0x10, 0x08, 0x10, 0x58, 0x8c};
static int operations, writes, adds, fail_at, drop_reg = -1;
static bool bus_down;

static void assert_profile(void)
{
    for (unsigned i = 0; i < sizeof(masks); ++i)
        assert((regs[i] & masks[i]) == values[i]);
    assert((regs[1] & 0x0f) == 0x0a);
    assert((regs[2] & 0xc0) == 0x80);
    assert((regs[4] & 0x07) == 0x03);
    assert((regs[5] & 0x43) == 0x03);
}

esp_err_t bmc_i2c_init(void) { return ESP_OK; }
i2c_master_bus_handle_t bmc_i2c_bus(void) { return (void *)1; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                   const i2c_device_config_t *cfg,
                                   i2c_master_dev_handle_t *dev)
{
    assert(bus && cfg->device_address == 0x6b);
    ++adds;
    *dev = (void *)2;
    return ESP_OK;
}

static bool failed(void)
{
    ++operations;
    return bus_down || operations == fail_at;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                    const uint8_t *tx, size_t tx_len,
                                    uint8_t *rx, size_t rx_len, int timeout)
{
    assert(dev && tx_len == 1 && rx_len == 1 && *tx < sizeof(regs));
    (void)timeout;
    if (failed()) return ESP_ERR_TIMEOUT;
    *rx = regs[*tx];
    if (*tx == 9) regs[9] = 0;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
                            const uint8_t *tx, size_t len, int timeout)
{
    assert(dev && len == 2 && tx[0] < sizeof(regs));
    (void)timeout;
    if (failed()) return ESP_ERR_TIMEOUT;
    ++writes;
    if (tx[0] != 1) assert(!(regs[1] & 0x10));
    if (tx[0] == 1 && (tx[1] & 0x10)) {
        for (unsigned i = 0; i < sizeof(masks); ++i) {
            if (i != 1) assert((regs[i] & masks[i]) == values[i]);
        }
        assert(!(tx[1] & 0x20));
    }
    if (tx[0] != drop_reg) regs[tx[0]] = tx[1];
    return ESP_OK;
}

static void assert_config(void)
{
    bq25601_config_t cfg;
    int before = writes;
    assert(bq25601_read_config(&cfg) == ESP_OK);
    assert(writes == before);
    assert(cfg.enabled && cfg.verified);
    assert(cfg.input_ma == 500 && cfg.charge_ma == 480);
    assert(cfg.voltage_mv == 4208 && cfg.precharge_ma == 120);
    assert(cfg.termination_ma == 60);
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "init-failure")) {
        assert(argc == 3);
        fail_at = atoi(argv[2]);
        assert(bq25601_init() != ESP_OK);
        assert(!(regs[1] & 0x10));
        fail_at = 0;
        assert(bq25601_init() == ESP_OK);
        assert_profile();
        assert_config();
        assert(adds == 1);
    } else if (!strcmp(argv[1], "mismatch")) {
        drop_reg = 2;
        assert(bq25601_init() != ESP_OK);
        assert(!(regs[1] & 0x10));
    } else if (!strcmp(argv[1], "bus-down")) {
        bus_down = true;
        assert(bq25601_init() != ESP_OK);
        bq25601_config_t cfg;
        assert(bq25601_read_config(&cfg) != ESP_OK);
        assert(!cfg.verified);
    } else if (!strcmp(argv[1], "wrong-part")) {
        regs[11] = 0;
        assert(bq25601_init() != ESP_OK);
        assert(!(regs[1] & 0x10));
    } else {
        assert(bq25601_init() == ESP_OK);
        int init_operations = operations;
        assert_profile();
        assert_config();
        assert(bq25601_init() == ESP_OK);
        assert(adds == 1);
        int before = writes;
        assert(bq25601_poll() == ESP_OK);
        assert(writes == before);
        regs[0] = (regs[0] & ~0x1f) | 0x17;
        bq25601_config_t cfg;
        assert(bq25601_read_config(&cfg) == ESP_OK);
        assert(!cfg.verified && cfg.input_ma == 2400);
        assert(writes == before);
        assert(bq25601_poll() == ESP_OK);
        assert_profile();
        assert_config();
        for (unsigned reg = 0; reg < sizeof(masks); ++reg) {
            regs[reg] ^= reg == 1 ? 0x20 : masks[reg];
            assert(bq25601_read_config(&cfg) == ESP_OK);
            assert(!cfg.verified);
            assert(bq25601_poll() == ESP_OK);
            assert_profile();
        }
        regs[8] = 0x54;
        regs[9] = 0x80;
        bq25601_status_t status;
        assert(bq25601_read_status(&status) == ESP_OK);
        assert(status.fault_latched == 0x80 && status.fault == 0);
        assert(status.chg == BQ25601_CHG_FAST && status.power_good);
        for (int read = 1; read <= 6; ++read) {
            before = writes;
            fail_at = operations + read;
            cfg.verified = true;
            assert(bq25601_read_config(&cfg) != ESP_OK);
            assert(!cfg.verified && writes == before);
            fail_at = 0;
        }
        fail_at = operations + 1;
        assert(bq25601_poll() != ESP_OK);
        assert(!(regs[1] & 0x10));
        fail_at = 0;
        assert(bq25601_poll() == ESP_OK);
        assert_profile();
        assert_config();
        printf("init_operations=%d\n", init_operations);
    }
    return 0;
}
