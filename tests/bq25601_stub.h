#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef void *i2c_master_dev_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef struct {
    int dev_addr_length;
    unsigned device_address;
    unsigned scl_speed_hz;
} i2c_device_config_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_NO_MEM 0x101
#define I2C_ADDR_BIT_LEN_7 0
typedef int *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int mutex;
    return &mutex;
}
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout)
{
    (void)timeout;
    assert(mutex && !*mutex);
    *mutex = 1;
    return pdTRUE;
}
static inline int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex && *mutex);
    *mutex = 0;
    return pdTRUE;
}
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { \
    esp_err_t result_ = (expr); (void)(tag); \
    if (result_ != ESP_OK) return result_; \
} while (0)
#define ESP_GOTO_ON_ERROR(expr, label, tag, ...) do { \
    ret = (expr); (void)(tag); if (ret != ESP_OK) goto label; \
} while (0)
static inline void mock_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
static inline const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "OK" : "ERROR";
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                   const i2c_device_config_t *cfg,
                                   i2c_master_dev_handle_t *dev);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                    const uint8_t *tx, size_t tx_len,
                                    uint8_t *rx, size_t rx_len, int timeout);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
                            const uint8_t *tx, size_t len, int timeout);
