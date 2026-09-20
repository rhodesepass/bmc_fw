#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

/*
 * ePASS NEXT mainboard BMC pin map (U10 ESP32-C3).
 * Source: epass_next/pcb/mainboard/bmc.kicad_sch (R0.3).
 * Override: idf.py menuconfig → "Board pins (ePASS NEXT BMC)".
 *
 * SPI is remapped via GPIO matrix (schematic note); native SPI pads NC.
 */

#ifndef CONFIG_BMC_GPIO_ESP_WAKE
#define CONFIG_BMC_GPIO_ESP_WAKE 0
#endif
#ifndef CONFIG_BMC_GPIO_BAT_ADC
#define CONFIG_BMC_GPIO_BAT_ADC 1
#endif
#ifndef CONFIG_BMC_GPIO_CHRG_IRQ
#define CONFIG_BMC_GPIO_CHRG_IRQ 2
#endif
#ifndef CONFIG_BMC_GPIO_APP_RESET
#define CONFIG_BMC_GPIO_APP_RESET 3
#endif
#ifndef CONFIG_BMC_GPIO_SPI_SCLK
#define CONFIG_BMC_GPIO_SPI_SCLK 4
#endif
#ifndef CONFIG_BMC_GPIO_SPI_CS
#define CONFIG_BMC_GPIO_SPI_CS 5
#endif
#ifndef CONFIG_BMC_GPIO_SPI_MOSI
#define CONFIG_BMC_GPIO_SPI_MOSI 6
#endif
#ifndef CONFIG_BMC_GPIO_SPI_MISO
#define CONFIG_BMC_GPIO_SPI_MISO 7
#endif
#ifndef CONFIG_BMC_GPIO_I2C_SCL
#define CONFIG_BMC_GPIO_I2C_SCL 8
#endif
#ifndef CONFIG_BMC_GPIO_I2C_SDA
#define CONFIG_BMC_GPIO_I2C_SDA 9
#endif
#ifndef CONFIG_BMC_GPIO_TO_APP_IRQ
#define CONFIG_BMC_GPIO_TO_APP_IRQ 10
#endif
#ifndef CONFIG_BMC_GPIO_APP_CORE_DIS
#define CONFIG_BMC_GPIO_APP_CORE_DIS 18
#endif
#ifndef CONFIG_BMC_GPIO_MAINSYS_DIS
#define CONFIG_BMC_GPIO_MAINSYS_DIS 19
#endif
#ifndef CONFIG_BMC_GPIO_UART_RX
#define CONFIG_BMC_GPIO_UART_RX 20
#endif
#ifndef CONFIG_BMC_GPIO_UART_TX
#define CONFIG_BMC_GPIO_UART_TX 21
#endif

#define BMC_PIN_ESP_WAKE      ((gpio_num_t)CONFIG_BMC_GPIO_ESP_WAKE)
#define BMC_PIN_BAT_ADC       ((gpio_num_t)CONFIG_BMC_GPIO_BAT_ADC)
#define BMC_PIN_CHRG_IRQ      ((gpio_num_t)CONFIG_BMC_GPIO_CHRG_IRQ)
#define BMC_PIN_APP_RESET     ((gpio_num_t)CONFIG_BMC_GPIO_APP_RESET)

#define BMC_PIN_SPI_SCLK      ((gpio_num_t)CONFIG_BMC_GPIO_SPI_SCLK)
#define BMC_PIN_SPI_CS        ((gpio_num_t)CONFIG_BMC_GPIO_SPI_CS)
#define BMC_PIN_SPI_MOSI      ((gpio_num_t)CONFIG_BMC_GPIO_SPI_MOSI)
#define BMC_PIN_SPI_MISO      ((gpio_num_t)CONFIG_BMC_GPIO_SPI_MISO)

#define BMC_PIN_I2C_SCL       ((gpio_num_t)CONFIG_BMC_GPIO_I2C_SCL)
#define BMC_PIN_I2C_SDA       ((gpio_num_t)CONFIG_BMC_GPIO_I2C_SDA)

#define BMC_PIN_TO_APP_IRQ    ((gpio_num_t)CONFIG_BMC_GPIO_TO_APP_IRQ)
#define BMC_PIN_APP_CORE_DIS  ((gpio_num_t)CONFIG_BMC_GPIO_APP_CORE_DIS)
#define BMC_PIN_MAINSYS_DIS   ((gpio_num_t)CONFIG_BMC_GPIO_MAINSYS_DIS)

#define BMC_PIN_UART_RX       ((gpio_num_t)CONFIG_BMC_GPIO_UART_RX)
#define BMC_PIN_UART_TX       ((gpio_num_t)CONFIG_BMC_GPIO_UART_TX)
