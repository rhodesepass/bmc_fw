#pragma once

#include "bat_gauge.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t bmc_runtime_init(void);
esp_err_t bmc_runtime_power_on(void);
void bmc_runtime_battery(const bat_gauge_snapshot_t *snapshot);
/* Reset/boot paths and poll run under the debug control lock. */
void bmc_runtime_reset(void);
void bmc_runtime_boot_seen(void);
void bmc_runtime_poll(void);
bool bmc_runtime_can_sleep(void);
void bmc_runtime_status(char *out, size_t size);
const uint8_t *bmc_runtime_spi_done(const uint8_t *rx, size_t bits);
