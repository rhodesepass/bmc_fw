#pragma once
#include <stddef.h>

void bmc_sleep_boot(void);
void bmc_sleep_poll(void);
void bmc_sleep_status(char *out, size_t size);
