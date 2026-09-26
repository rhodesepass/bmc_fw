#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BMC_SLEEP_MAGIC 0x534c5031u
#define BMC_SLEEP_BOOTLOADER_MAGIC 0x53424c31u

static inline bool bmc_sleep_retained(const volatile uint32_t *state)
{
    return state[1] == BMC_SLEEP_MAGIC && state[2] == ~BMC_SLEEP_MAGIC;
}

static inline void bmc_sleep_retain(volatile uint32_t *state, bool sleeping)
{
    state[1] = sleeping ? BMC_SLEEP_MAGIC : 0;
    state[2] = sleeping ? ~BMC_SLEEP_MAGIC : 0;
}
