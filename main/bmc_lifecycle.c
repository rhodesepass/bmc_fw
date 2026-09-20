#include "bmc_lifecycle.h"

void bmc_lifecycle_reset(struct bmc_lifecycle *s, uint32_t epoch)
{
    *s = (struct bmc_lifecycle){.epoch = epoch ? epoch : 1};
}

unsigned bmc_lifecycle_request(struct bmc_lifecycle *s, unsigned op,
                               uint32_t epoch, uint64_t now_ms)
{
    if (op == 1) return 0;
    if (epoch != s->epoch) return 1;
    if (op == 2) {
        if (s->state == BMC_APP_OFF || s->state == BMC_APP_STOPPING) return 2;
        s->armed = true;
        s->state = BMC_APP_RUNNING;
        return 0;
    }
    if (op != 3) return 1;
    if (!s->armed || s->state == BMC_APP_OFF) return 2;
    /* Retransmissions must not keep postponing an accepted shutdown. */
    if (s->state != BMC_APP_STOPPING) s->shutdown_at = now_ms + 300;
    s->state = BMC_APP_STOPPING;
    return 0;
}

enum bmc_button_action bmc_lifecycle_button(struct bmc_lifecycle *s,
                                           bool pressed, uint64_t now_ms)
{
    if (!pressed) {
        bool rescue = s->pressed && !s->fired && !s->armed &&
            s->state == BMC_APP_RECOVERY && now_ms - s->pressed_at >= 2000 &&
            now_ms - s->pressed_at <= 5000;
        s->released = true;
        s->pressed = s->fired = false;
        return rescue ? BMC_BUTTON_RESCUE : BMC_BUTTON_NONE;
    }
    /* A held boot key or the same hold that cut power must never boot again. */
    if (!s->released || s->fired) return BMC_BUTTON_NONE;
    if (!s->pressed) {
        s->pressed = true;
        s->pressed_at = now_ms;
        if (s->state == BMC_APP_OFF) {
            s->fired = true;
            return BMC_BUTTON_BOOT;
        }
    }
    if (s->armed && now_ms - s->pressed_at >= 5000) {
        s->fired = true;
        return BMC_BUTTON_CUT;
    }
    return BMC_BUTTON_NONE;
}
