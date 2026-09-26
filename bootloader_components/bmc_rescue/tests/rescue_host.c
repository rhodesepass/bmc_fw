#include "rescue_stub.h"
#include "../recovery_state.h"
#include "../sleep_state.h"
#include "../../../main/bmc_recovery.h"
#include <assert.h>
#include <stdio.h>

rtc_retain_mem_t retained;
unsigned registers[3], reset_reason, button_level = 1, delays, crc_updates;
unsigned button_release_us = UINT32_MAX, bounce_start_us = UINT32_MAX, bounce_end_us;
unsigned levels[22], outputs[22];
int GPIO;
jmp_buf rom_jump;
void bootloader_before_init(void);

static void expect_normal_boot(void)
{
    if (setjmp(rom_jump)) assert(!"must not enter ROM download");
    bootloader_before_init();
}

static void expect_rom(void)
{
    if (!setjmp(rom_jump)) {
        bootloader_before_init();
        assert(!"must enter ROM download");
    }
    assert(registers[2] & RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    assert(outputs[3] && !levels[3]);
    assert(outputs[18] && !levels[18]);
    assert(outputs[19] && !levels[19]);
    for (unsigned pin = 4; pin <= 7; ++pin) assert(!outputs[pin]);
}

int main(void)
{
    bootloader_common_reset_rtc_retain_mem();
    retained.custom[0] = bmc_recovery_encode(1);
    bootloader_common_update_rtc_retain_mem(NULL, false);
    assert(retained.crc == UINT32_MAX);
    bootloader_common_update_rtc_retain_mem(NULL, true);
    assert(retained.crc == 0x4743989au);
    assert(bmc_recovery_boot_attempts() == 0);
    crc_updates = 0;

    retained.custom[0] = bmc_recovery_encode(3);
    reset_reason = RESET_REASON_CHIP_POWER_ON;
    expect_normal_boot();
    assert(bmc_recovery_boot_attempts() == 1);
    assert(crc_updates == 2 && delays == 0);
    assert(retained.crc == 0x4743989au);
    bootloader_common_update_rtc_retain_mem(NULL, true);
    assert(bmc_recovery_boot_attempts() == 1);
    reset_reason = 12;
    expect_normal_boot();
    bootloader_common_update_rtc_retain_mem(NULL, true);
    assert(bmc_recovery_boot_attempts() == 2);
    expect_normal_boot();
    bootloader_common_update_rtc_retain_mem(NULL, true);
    assert(bmc_recovery_boot_attempts() == 3);
    expect_rom();
    assert(delays == 30000);

    bmc_recovery_mark_app_ready();
    assert(bmc_recovery_boot_attempts() == 0);
    reset_reason = RESET_REASON_CORE_DEEP_SLEEP;
    bmc_sleep_retain(retained.custom, true);
    for (unsigned i = 0; i < 20; ++i) expect_normal_boot();
    assert(bmc_recovery_boot_attempts() == 0);
    assert(bmc_sleep_retained(retained.custom));
    retained.custom[2] ^= 1;
    expect_normal_boot();
    assert(bmc_recovery_boot_attempts() == 1);
    assert(!bmc_sleep_retained(retained.custom));
    bmc_recovery_mark_app_ready();
    reset_reason = 12;
    expect_normal_boot();
    assert(bmc_recovery_boot_attempts() == 1);
    button_level = 0;
    delays = 0;
    button_release_us = 2000000;
    expect_rom();
    assert(delays == 2060000);

    bmc_recovery_mark_app_ready();
    delays = 0;
    button_release_us = 1999000;
    expect_normal_boot();
    assert(delays == 1999000 && bmc_recovery_boot_attempts() == 1);

    bmc_recovery_mark_app_ready();
    delays = 0;
    button_release_us = UINT32_MAX;
    expect_normal_boot();
    assert(delays == 5000000 && bmc_recovery_boot_attempts() == 1);

    bmc_recovery_mark_app_ready();
    delays = 0;
    button_release_us = 4971000;
    expect_normal_boot();
    assert(delays == 5000000 && bmc_recovery_boot_attempts() == 1);

    bmc_recovery_mark_app_ready();
    delays = 0;
    button_release_us = 4970000;
    expect_rom();
    assert(delays == 5030000);

    bmc_recovery_mark_app_ready();
    delays = 0;
    button_release_us = 2000000;
    bounce_start_us = 2020000;
    bounce_end_us = 2030000;
    expect_rom();
    assert(delays == 2090000);

    retained.custom[0] = bmc_recovery_encode(3);
    delays = 0;
    button_release_us = UINT32_MAX;
    bounce_start_us = UINT32_MAX;
    expect_rom();
    assert(delays == 30000);

    bmc_recovery_mark_app_ready();
    registers[2] = 0;
    button_level = 1;
    if (!setjmp(rom_jump)) {
        bmc_recovery_request_rescue();
        assert(!"physical recovery request must reset");
    }
    assert(bmc_recovery_boot_attempts() == BMC_RECOVERY_FAILED_BOOTS);
    expect_rom();

    registers[0] = EFUSE_DIS_FORCE_DOWNLOAD;
    retained.custom[0] = bmc_recovery_encode(3);
    expect_normal_boot();
    assert(bmc_recovery_boot_attempts() == 3);
    retained.custom[0] ^= 1;
    assert(bmc_recovery_boot_attempts() == 0);
    puts("rescue host: CRC sentinel, boot bookkeeping, READY, released-key timing/bounce/deadline, automatic rescue bypass, GPIO, eFuse PASS");
}
