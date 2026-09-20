#include "bmc_recovery.h"
#include "bootloader_common.h"
#include "esp_rom_sys.h"
#include "../bootloader_components/bmc_rescue/recovery_state.h"

#if !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC || CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE < 4
#error "Build and flash BMC bootloader and application with shared custom RTC memory"
#endif
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "BMC custom RTC state must be excluded from IDF's retained-memory CRC"
#endif

void bmc_recovery_mark_app_ready(void)
{
    volatile uint32_t *state =
        (volatile uint32_t *)bootloader_common_get_rtc_retain_mem()->custom;
    *state = bmc_recovery_encode(0);
}

unsigned bmc_recovery_boot_attempts(void)
{
    volatile uint32_t *state =
        (volatile uint32_t *)bootloader_common_get_rtc_retain_mem()->custom;
    return bmc_recovery_decode(*state);
}

void bmc_recovery_request_rescue(void)
{
    volatile uint32_t *state =
        (volatile uint32_t *)bootloader_common_get_rtc_retain_mem()->custom;
    *state = bmc_recovery_encode(BMC_RECOVERY_FAILED_BOOTS);
    __sync_synchronize();
    esp_rom_software_reset_system();
}
