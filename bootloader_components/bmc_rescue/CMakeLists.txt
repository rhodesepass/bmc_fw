idf_component_register(SRCS "hooks.c" INCLUDE_DIRS "." REQUIRES bootloader_support esp_rom soc)

# The bootloader Kconfig omits main's board menu; use the same selected pins.
foreach(pin ESP_WAKE APP_RESET MAINSYS_DIS APP_CORE_DIS SPI_SCLK SPI_CS SPI_MOSI SPI_MISO)
    file(STRINGS "${SDKCONFIG}" pin_config REGEX "^CONFIG_BMC_GPIO_${pin}=[0-9]+$")
    if(NOT pin_config)
        message(FATAL_ERROR "Missing CONFIG_BMC_GPIO_${pin} for independent rescue")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "${pin_config}")
endforeach()
