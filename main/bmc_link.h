#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef esp_err_t (*bmc_link_channel_fn)(const uint8_t *data, size_t len,
                                        uint8_t *reply, size_t *reply_len);
esp_err_t bmc_link_init(void);
esp_err_t bmc_link_command(const char *cmd, char *response, size_t capacity);
esp_err_t bmc_link_register_channel(const char *name, bmc_link_channel_fn receive);
/* Called while the power interlock is held; never takes the command mutex. */
void bmc_link_invalidate(void);

bool bmc_link_session_valid(uint32_t epoch);
