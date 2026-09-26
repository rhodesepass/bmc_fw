#include "sdkconfig.h"
#include "bootloader_common.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/efuse_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/reset_reasons.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "recovery_state.h"
#include "sleep_state.h"

#if !CONFIG_IDF_TARGET_ESP32C3
#error "BMC ROM rescue is specific to ESP32-C3"
#endif
#if !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC || CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE < 16
#error "BMC rescue needs shared bootloader custom RTC memory"
#endif
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "BMC custom RTC state must be excluded from IDF's retained-memory CRC"
#endif

void bootloader_hooks_include(void) {}

static void output_level(unsigned pin, unsigned level, bool open_drain)
{
    gpio_ll_set_level(&GPIO, pin, level);
    esp_rom_gpio_pad_select_gpio(pin);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    if (open_drain) gpio_ll_od_enable(&GPIO, pin);
    else gpio_ll_od_disable(&GPIO, pin);
    gpio_ll_output_enable(&GPIO, pin);
    gpio_ll_hold_dis(&GPIO, pin);
}

static bool rescue_button_released(void)
{
    unsigned pin = CONFIG_BMC_GPIO_ESP_WAKE;
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_input_enable(&GPIO, pin);
    esp_rom_gpio_pad_pullup_only(pin);
    if (gpio_ll_get_level(&GPIO, pin)) return false;
    unsigned released_at = UINT32_MAX;
    for (unsigned ms = 0; ms <= 5000; ++ms) {
        if (gpio_ll_get_level(&GPIO, pin)) {
            if (released_at == UINT32_MAX) {
                if (ms < 2000) return false;
                released_at = ms;
            }
            if (ms - released_at >= 30) return true;
        } else {
            released_at = UINT32_MAX;
        }
        if (ms == 5000) break;
        esp_rom_delay_us(1000);
    }
    return false;
}

static void enter_rom_rescue(void)
{
    if ((REG_READ(EFUSE_RD_REPEAT_DATA0_REG) & EFUSE_DIS_FORCE_DOWNLOAD) ||
        (REG_READ(EFUSE_RD_REPEAT_DATA3_REG) & EFUSE_DIS_DOWNLOAD_MODE))
        return;

    output_level(CONFIG_BMC_GPIO_APP_RESET, 0, true);
    output_level(CONFIG_BMC_GPIO_MAINSYS_DIS, 0, false);
    output_level(CONFIG_BMC_GPIO_APP_CORE_DIS, 0, false);
    const unsigned spi_pins[] = {CONFIG_BMC_GPIO_SPI_SCLK, CONFIG_BMC_GPIO_SPI_CS,
                                CONFIG_BMC_GPIO_SPI_MOSI, CONFIG_BMC_GPIO_SPI_MISO};
    for (unsigned i = 0; i < sizeof(spi_pins) / sizeof(spi_pins[0]); ++i) {
        esp_rom_gpio_pad_select_gpio(spi_pins[i]);
        gpio_ll_output_disable(&GPIO, spi_pins[i]);
        gpio_ll_hold_dis(&GPIO, spi_pins[i]);
    }
    const unsigned quiet_pins[] = {CONFIG_BMC_GPIO_UART_RX, CONFIG_BMC_GPIO_UART_TX,
                                  CONFIG_BMC_GPIO_TO_APP_IRQ};
    for (unsigned i = 0; i < sizeof(quiet_pins) / sizeof(quiet_pins[0]); ++i) {
        gpio_ll_output_disable(&GPIO, quiet_pins[i]);
        gpio_ll_hold_dis(&GPIO, quiet_pins[i]);
    }
    esp_rom_delay_us(30000);
    /* C3 reset releases APP_RESET while the silent SPI bus lets D1s fall into FEL. */
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
}

void bootloader_before_init(void)
{
    rtc_retain_mem_t *retained = bootloader_common_get_rtc_retain_mem();
    bool cold = esp_rom_get_reset_reason(0) == RESET_REASON_CHIP_POWER_ON;
    if (cold) {
        bootloader_common_reset_rtc_retain_mem();
        /* A zero IDF header hashes to its invalid 0xffffffff CRC sentinel. */
        bootloader_common_update_rtc_retain_mem(NULL, true);
    }
    volatile uint32_t *state = (volatile uint32_t *)retained->custom;
    state[3] = BMC_SLEEP_BOOTLOADER_MAGIC;
    if (esp_rom_get_reset_reason(0) == RESET_REASON_CORE_DEEP_SLEEP && bmc_sleep_retained(state)) {
        if (rescue_button_released()) enter_rom_rescue();
        return;
    }
    bmc_sleep_retain(state, false);
    unsigned attempts = bmc_recovery_decode(*state);
    if (attempts >= BMC_RECOVERY_FAILED_BOOTS || rescue_button_released())
        enter_rom_rescue();
    if (attempts < BMC_RECOVERY_FAILED_BOOTS) ++attempts;
    *state = bmc_recovery_encode(attempts);
    /* Establish IDF's CRC before its later boot bookkeeping can clear custom[]. */
    bootloader_common_update_rtc_retain_mem(NULL, false);
}
