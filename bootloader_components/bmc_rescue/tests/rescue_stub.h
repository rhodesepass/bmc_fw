#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

#define CONFIG_IDF_TARGET_ESP32C3 1
#define CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC 1
#define CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE 16
#define CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC 0
#define CONFIG_BMC_GPIO_ESP_WAKE 0
#define CONFIG_BMC_GPIO_APP_RESET 3
#define CONFIG_BMC_GPIO_MAINSYS_DIS 19
#define CONFIG_BMC_GPIO_APP_CORE_DIS 18
#define CONFIG_BMC_GPIO_SPI_SCLK 4
#define CONFIG_BMC_GPIO_SPI_CS 5
#define CONFIG_BMC_GPIO_SPI_MOSI 6
#define CONFIG_BMC_GPIO_SPI_MISO 7
#define RESET_REASON_CHIP_POWER_ON 1
#define EFUSE_RD_REPEAT_DATA0_REG 0
#define EFUSE_RD_REPEAT_DATA3_REG 1
#define EFUSE_DIS_FORCE_DOWNLOAD 1
#define EFUSE_DIS_DOWNLOAD_MODE 2
#define RTC_CNTL_OPTION1_REG 2
#define RTC_CNTL_FORCE_DOWNLOAD_BOOT 4
#define SIG_GPIO_OUT_IDX 128
#define REG_READ(reg) registers[reg]
#define REG_SET_BIT(reg, bit) (registers[reg] |= (bit))

typedef struct {
    uint32_t partition[2];
    uint16_t reboot_counter;
    uint8_t flags, reserve;
    uint32_t custom[4];
    uint32_t crc;
} rtc_retain_mem_t;
extern rtc_retain_mem_t retained;
extern unsigned registers[3], reset_reason, button_level, delays, crc_updates;
extern unsigned button_release_us, bounce_start_us, bounce_end_us;
extern unsigned levels[22], outputs[22];
extern int GPIO;
extern jmp_buf rom_jump;
static inline rtc_retain_mem_t *bootloader_common_get_rtc_retain_mem(void) { return &retained; }
static inline void bootloader_common_reset_rtc_retain_mem(void) { memset(&retained, 0, sizeof(retained)); }
static inline uint32_t retained_crc(void)
{
    uint32_t crc = 0;
    const uint8_t *bytes = (const uint8_t *)&retained;
    for (unsigned i = 0; i < offsetof(rtc_retain_mem_t, custom); ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static inline void bootloader_common_update_rtc_retain_mem(void *p, bool increment)
{
    (void)p;
    if (increment) {
        if (retained.crc == UINT32_MAX || retained.crc != retained_crc())
            bootloader_common_reset_rtc_retain_mem();
        if (retained.reboot_counter != UINT16_MAX) ++retained.reboot_counter;
    }
    retained.crc = retained_crc();
    ++crc_updates;
}
static inline int esp_rom_get_reset_reason(int cpu) { (void)cpu; return reset_reason; }
static inline void esp_rom_gpio_pad_select_gpio(unsigned pin) { (void)pin; }
static inline void esp_rom_gpio_pad_pullup_only(unsigned pin) { (void)pin; }
static inline void esp_rom_gpio_connect_out_signal(unsigned pin, unsigned signal, bool out, bool oen) { (void)pin; (void)signal; (void)out; (void)oen; }
static inline void gpio_ll_set_level(int *hw, unsigned pin, unsigned value) { (void)hw; levels[pin] = value; }
static inline void gpio_ll_output_enable(int *hw, unsigned pin) { (void)hw; outputs[pin] = 1; }
static inline void gpio_ll_output_disable(int *hw, unsigned pin) { (void)hw; outputs[pin] = 0; }
static inline void gpio_ll_input_enable(int *hw, unsigned pin) { (void)hw; (void)pin; }
static inline void gpio_ll_od_enable(int *hw, unsigned pin) { (void)hw; (void)pin; }
static inline void gpio_ll_od_disable(int *hw, unsigned pin) { (void)hw; (void)pin; }
static inline unsigned gpio_ll_get_level(int *hw, unsigned pin)
{
    (void)hw; (void)pin;
    if (delays >= bounce_start_us && delays < bounce_end_us) return 0;
    return delays >= button_release_us ? 1 : button_level;
}
static inline void esp_rom_delay_us(unsigned us) { delays += us; }
static inline void esp_rom_software_reset_system(void) { longjmp(rom_jump, 1); }
