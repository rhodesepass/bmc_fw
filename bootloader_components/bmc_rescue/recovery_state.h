#pragma once

#include <stdint.h>

#define BMC_RECOVERY_FAILED_BOOTS 3u
#define BMC_RECOVERY_STATE_MAGIC 0xb3c10000u

static inline uint32_t bmc_recovery_encode(unsigned attempts)
{
    return BMC_RECOVERY_STATE_MAGIC | ((attempts & 0xffu) << 8) |
           ((attempts ^ 0xffu) & 0xffu);
}

static inline unsigned bmc_recovery_decode(uint32_t state)
{
    unsigned attempts = (state >> 8) & 0xffu;
    if ((state & 0xffff0000u) != BMC_RECOVERY_STATE_MAGIC ||
        (state & 0xffu) != (attempts ^ 0xffu))
        return 0;
    return attempts;
}
