idf_component_register(
    SRCS
        "epass_bmc.c"
        "app_ota.c"
        "bmc_ota.c"
        "app_ota_wire.c"
        "bmc_power.c"
        "bmc_pm.c"
        "bmc_sleep.c"
        "bmc_runtime.c"
        "bmc_lifecycle.c"
        "bmc_recovery.c"
        "bmc_link.c"
        "spl_nand.c"
        "fast_spi_slave.c"
        "bmc_debug.c"
        "ble_debug.c"
        "bmc_i2c.c"
        "bq25601.c"
        "bat_adc.c"
        "bat_gauge.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        mbedtls
        bt
        esp_http_server
        esp_netif
        esp_event
        esp_wifi
        nvs_flash
        esp_timer
        esp_pm
        esp_app_format
        bootloader_support
        esp_driver_uart
        esp_driver_spi
        esp_partition
        esp_driver_gpio
        esp_driver_i2c
        esp_adc
)

target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../web/ota.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../web/ota.js" TEXT)
