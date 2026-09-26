#include "bmc_runtime_stub.h"
#include "../main/bmc_lifecycle.c"
#include "../main/bmc_runtime.c"

bool bmc_mainsys_enabled(void) { return mainsys; }
static bool boot_performance;
esp_err_t bmc_pm_boot(bool active) { boot_performance = active; return ESP_OK; }
static unsigned ready_marks;
void bmc_recovery_mark_app_ready(void) { ready_marks++; }
static unsigned rescue_requests;
void bmc_recovery_request_rescue(void) { rescue_requests++; }
unsigned bmc_recovery_boot_attempts(void) { return 0; }
esp_err_t bmc_wifi_stop(void) { return stop_error; }
esp_err_t bmc_mainsys_force_off(void)
{
    cuts++;
    mainsys = false;
    return ESP_OK;
}
esp_err_t bmc_mainsys_set(bool enabled)
{
    if (!enabled && stop_error) return stop_error;
    if (!enabled) cuts++;
    mainsys = enabled;
    return ESP_OK;
}
esp_err_t spl_nand_hold(void)
{
    holds++;
    if (!hold_error) bmc_runtime_reset();
    return hold_error;
}
esp_err_t spl_nand_boot_slot(unsigned slot)
{
    assert(slot == CONFIG_BMC_SPL_SLOT);
    assert(uart_routed && spi_routed);
    boots++;
    bmc_runtime_reset();
    return ESP_OK;
}
bool app_ota_pending(void) { return ota_active; }
void app_ota_normal_boot(void) {}

static uint32_t sequence;
static uint32_t frame[513], zero[513], response[513];

static void advance(unsigned milliseconds)
{
    fake_us += (int64_t)milliseconds * 1000;
    bmc_runtime_poll();
}
static void fresh(void)
{
    fake_key = 1;
    fake_core = 0;
    mainsys = true;
    ota_active = false;
    stop_error = hold_error = 0;
    cuts = holds = boots = 0;
    fake_us += 100000;
    assert(bmc_runtime_init() == ESP_OK);
    advance(50);
}
static void sign_frame(void)
{
    frame[512] = esp_rom_crc32_le(0, (const uint8_t *)frame, 2048);
}
static void make_frame(unsigned op, uint32_t epoch)
{
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x31434d42;
    frame[1] = 1;
    frame[2] = ++sequence;
    frame[3] = op;
    frame[4] = epoch;
    sign_frame();
}
static void send_frame(void)
{
    bmc_runtime_spi_done((const uint8_t *)frame, sizeof(frame) * 8);
    advance(20);
}
static void transact(unsigned op, uint32_t epoch)
{
    make_frame(op, epoch);
    send_frame();
    const uint8_t *reply = bmc_runtime_spi_done((const uint8_t *)zero, sizeof(zero) * 8);
    assert(reply);
    memcpy(response, reply, sizeof(response));
    assert(response[0] == 0x31434d42 && response[1] == 1);
    assert(response[2] == sequence && response[3] == (op | 0x80000000u));
    assert(response[512] == esp_rom_crc32_le(0, (const uint8_t *)response, 2048));
}
static void press(void)
{
    fake_key = 0;
    advance(1);
    advance(40);
}
static void release(void)
{
    fake_key = 1;
    advance(1);
    advance(40);
}

static void test_protocol(void)
{
    fresh();
    assert(esp_rom_crc32_le(0, (const uint8_t *)"123456789", 9) == 0xcbf43926);
    bat_gauge_snapshot_t sample = {.valid = true, .vbat_mv = 3871, .vbat_filt_mv = 3871,
        .soc_pct = 67, .power = BAT_POWER_CHARGING, .chg = {.power_good = true}};
    bmc_runtime_battery(&sample);
    transact(1, 0);
    assert(response[4] == lifecycle.epoch && !response[5] && response[6] == 0);
    assert(response[7] == (2 | 4 | 16) && response[8] == 3871 && response[9] == 67);
    assert(!strcmp((char *)&response[16], "runtime-host-v1"));
    assert(response[10] == (uint32_t)(fake_us / 1000000));
    sample.vbat_mv = 17;
    bmc_runtime_battery(&sample);
    transact(1, 0);
    assert(!(response[7] & 2) && !response[8] && !response[9]);
    assert(response[7] & 16);
    sample.vbat_mv = 3871;
    bmc_runtime_battery(&sample);
    transact(3, lifecycle.epoch);
    assert(response[5] == 2 && !cuts && !lifecycle.shutdown_at);
    transact(2, lifecycle.epoch - 1);
    assert(response[5] == 1 && !lifecycle.armed);
    assert(boot_performance);
    for (unsigned field = 0; field < 6; field++) {
        uint32_t previous = published[2];
        make_frame(2, lifecycle.epoch);
        if (field == 0) frame[512] ^= 1;
        if (field == 1) { frame[1] = 2; sign_frame(); }
        if (field == 2) { frame[2] = 0; sign_frame(); }
        if (field == 3) { frame[3] = 4; sign_frame(); }
        if (field == 4) { frame[5] = 1; sign_frame(); }
        if (field == 5) { frame[511] = 1; sign_frame(); }
        send_frame();
        assert(published[2] == previous && !lifecycle.armed);
    }
    assert(!bmc_runtime_spi_done((const uint8_t *)frame, 16));
    uint32_t bad[513] = {0x12345678};
    assert(!bmc_runtime_spi_done((const uint8_t *)bad, sizeof(bad) * 8));
    bad[0] = 0;
    bad[511] = 1;
    assert(!bmc_runtime_spi_done((const uint8_t *)bad, sizeof(bad) * 8));
    advance(6001);
    transact(1, 0);
    assert(!(response[7] & 2) && !response[8] && !response[9]);
}

static void test_shutdown_and_reset(void)
{
    fresh();
    assert(boot_performance);
    transact(2, lifecycle.epoch);
    assert(lifecycle.armed && response[6] == 1);
    assert(!boot_performance);
    transact(3, lifecycle.epoch);
    uint64_t deadline = lifecycle.shutdown_at;
    assert(deadline == (uint64_t)(fake_us / 1000) + 300);
    advance(100);
    transact(3, lifecycle.epoch);
    assert(lifecycle.shutdown_at == deadline);
    transact(2, lifecycle.epoch);
    assert(response[5] == 2 && lifecycle.shutdown_at == deadline);
    advance((unsigned)(deadline - fake_us / 1000 - 1));
    assert(!cuts);
    advance(1);
    assert(cuts == 1 && holds == 1 && fake_core == 1 && !mainsys);
    assert(lifecycle.state == BMC_APP_OFF && !lifecycle.armed);
    assert(uart_direction == GPIO_MODE_INPUT && miso_direction == GPIO_MODE_INPUT);
    advance(10000);
    assert(cuts == 1 && !boots);
    release();
    press();
    assert(boots == 1 && mainsys && !fake_core && !lifecycle.armed);
    assert(uart_direction == GPIO_MODE_OUTPUT && miso_direction == GPIO_MODE_INPUT_OUTPUT);
    assert(uart_routed && spi_routed);
    advance(10000);
    assert(boots == 1 && cuts == 1);

    fresh();
    uint32_t epoch = lifecycle.epoch;
    make_frame(2, epoch);
    bmc_runtime_spi_done((const uint8_t *)frame, sizeof(frame) * 8);
    bmc_runtime_reset();
    advance(20);
    assert(lifecycle.epoch != epoch && !lifecycle.armed && !published);
    transact(2, epoch);
    assert(response[5] == 1 && !lifecycle.armed);
    transact(2, lifecycle.epoch);
    transact(3, lifecycle.epoch);
    epoch = lifecycle.epoch;
    bmc_runtime_boot_seen();
    assert(boot_performance);
    advance(300);
    assert(!cuts && !lifecycle.armed && !lifecycle.shutdown_at && lifecycle.epoch != epoch);
}

static void test_reset_during_ready(void)
{
    fresh();
    transact(2, lifecycle.epoch);
    assert(!boot_performance);
    unsigned previous_marks = ready_marks;
    receive_hook = bmc_runtime_boot_seen;
    transact(2, lifecycle.epoch);
    receive_hook = NULL;
    assert(boot_seen && boot_performance && ready_marks == previous_marks);
    advance(20);
    assert(!lifecycle.armed && boot_performance);
    transact(2, lifecycle.epoch);
    assert(lifecycle.armed && !boot_performance && ready_marks == previous_marks + 1);
}

static void test_button(void)
{
    fresh();
    press();
    advance(6000);
    assert(!cuts && !lifecycle.armed);
    transact(2, lifecycle.epoch);
    advance(6000);
    assert(!cuts);
    release();
    press();
    advance(4999);
    assert(!cuts);
    advance(1);
    assert(cuts == 1 && lifecycle.state == BMC_APP_OFF);
    assert(!bmc_runtime_can_sleep());
    advance(10000);
    assert(cuts == 1 && !boots);
    release();
    assert(bmc_runtime_can_sleep());
    press();
    assert(boots == 1 && !fake_core);
    assert(!bmc_runtime_can_sleep());

    fresh();
    transact(2, lifecycle.epoch);
    fake_key = 0;
    advance(1);
    advance(39);
    fake_key = 1;
    advance(1);
    advance(6000);
    assert(!cuts);
}

static void test_protected_rescue(void)
{
    fresh();
    rescue_requests = 0;
    press();
    advance(2000);
    assert(!rescue_requests && !cuts);
    release();
    assert(rescue_requests == 1 && !cuts && mainsys);
    advance(10000);
    assert(rescue_requests == 1);
    press();
    advance(5100);
    release();
    assert(rescue_requests == 1 && !cuts);
}

static void test_failed_cut(void)
{
    fresh();
    transact(2, lifecycle.epoch);
    stop_error = ESP_FAIL;
    transact(3, lifecycle.epoch);
    advance(300);
    assert(!cuts && !holds && mainsys && !fake_core && !lifecycle.shutdown_at);
    fresh();
    transact(2, lifecycle.epoch);
    hold_error = ESP_FAIL;
    transact(3, lifecycle.epoch);
    advance(300);
    assert(holds == 1 && mainsys && !fake_core && !lifecycle.shutdown_at);
    fresh();
    transact(2, lifecycle.epoch);
    stop_error = ESP_FAIL;
    press();
    advance(5000);
    assert(cuts == 1 && fake_core && !mainsys);
}

static void test_dma_and_queue(void)
{
    fresh();
    transact(1, 0);
    const uint32_t *active = dma_active;
    uint32_t preserved[513];
    memcpy(preserved, active, sizeof(preserved));
    make_frame(1, 0);
    bmc_runtime_spi_done((const uint8_t *)frame, sizeof(frame) * 8);
    advance(20);
    assert(!memcmp(active, preserved, sizeof(preserved)));
    assert(published != active);

    make_frame(1, 0);
    bmc_runtime_spi_done((const uint8_t *)frame, sizeof(frame) * 8);
    make_frame(1, 0);
    bmc_runtime_spi_done((const uint8_t *)frame, sizeof(frame) * 8);
    make_frame(2, lifecycle.epoch);
    bmc_runtime_spi_done((const uint8_t *)frame, sizeof(frame) * 8);
    advance(20);
    advance(20);
    advance(20);
    assert(!lifecycle.armed);
    transact(2, lifecycle.epoch);
    assert(lifecycle.armed);
}

int main(void)
{
    test_protocol();
    test_shutdown_and_reset();
    test_reset_during_ready();
    test_button();
    test_protected_rescue();
    test_failed_cut();
    test_dma_and_queue();
    puts("BMC runtime: protocol, telemetry, readiness, shutdown, button and recovery passed");
    return 0;
}
