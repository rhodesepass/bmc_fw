#pragma once

#include <stdbool.h>
#include <stdint.h>

enum bmc_app_state { BMC_APP_RECOVERY, BMC_APP_RUNNING, BMC_APP_STOPPING, BMC_APP_OFF };
enum bmc_button_action { BMC_BUTTON_NONE, BMC_BUTTON_BOOT, BMC_BUTTON_CUT, BMC_BUTTON_RESCUE };
struct bmc_lifecycle {
    uint32_t epoch;
    enum bmc_app_state state;
    bool armed, released, pressed, fired;
    uint64_t pressed_at, shutdown_at;
};

void bmc_lifecycle_reset(struct bmc_lifecycle *s, uint32_t epoch);
unsigned bmc_lifecycle_request(struct bmc_lifecycle *s, unsigned op,
                               uint32_t epoch, uint64_t now_ms);
enum bmc_button_action bmc_lifecycle_button(struct bmc_lifecycle *s,
                                           bool pressed, uint64_t now_ms);
