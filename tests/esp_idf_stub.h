#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef int gpio_num_t;
#define ESP_ERR_INVALID_ARG 7
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_CRC 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_TIMEOUT 6
#define GPIO_MODE_OUTPUT_OD 1
#define ESP_PARTITION_TYPE_DATA 1
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define ESP_INTR_FLAG_IRAM 1
#define ESP_INTR_FLAG_LEVEL3 2
#define ESP_INTR_FLAG_LEVEL1 4
#define SPI_SLAVE_NO_RETURN_RESULT 1
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 0
#define CONFIG_BMC_SPL_SLOT 0
#define portMAX_DELAY 1000
#define IRAM_ATTR
#define DMA_ATTR
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))

typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef struct { size_t size; } esp_partition_t;
typedef struct spi_slave_transaction_t {
    size_t length;
    size_t trans_len;
    const void *tx_buffer;
    void *rx_buffer;
} spi_slave_transaction_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num;
    int quadwp_io_num, quadhd_io_num, max_transfer_sz, intr_flags;
} spi_bus_config_t;
typedef struct {
    int spics_io_num, mode, queue_size, flags;
    void (*post_setup_cb)(spi_slave_transaction_t *);
    void (*post_trans_cb)(spi_slave_transaction_t *);
} spi_slave_interface_config_t;

int gpio_set_level(gpio_num_t pin, int level);
esp_err_t gpio_config(const gpio_config_t *config);
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *out, size_t length);
void *heap_caps_malloc(size_t length, int flags);
void heap_caps_free(void *ptr);
esp_err_t spi_slave_initialize(int host, const spi_bus_config_t *bus, const spi_slave_interface_config_t *slave, int dma);
esp_err_t spi_slave_queue_trans(int host, spi_slave_transaction_t *trans, int timeout);
esp_err_t spi_slave_queue_trans_isr(int host, spi_slave_transaction_t *trans);
esp_err_t spi_slave_free(int host);
void vTaskDelay(int ticks);

#define pdMS_TO_TICKS(ms) (ms)
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { esp_err_t e = (expr); (void)(tag); if (e != ESP_OK) return e; } while (0)
esp_err_t spi_slave_queue_reset(int host);

static inline uint32_t esp_cpu_get_cycle_count(void) { static uint32_t c; return ++c; }

static inline int64_t esp_timer_get_time(void) { static int64_t t; return ++t; }

typedef int portMUX_TYPE;
typedef int *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t esp_partition_mmap_handle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_ISR(p) ((void)(p))
#define portEXIT_CRITICAL_ISR(p) ((void)(p))
#define portYIELD_FROM_ISR() ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define ESP_PARTITION_MMAP_DATA 0
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t mutex);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle);
unsigned ulTaskNotifyTake(int clear, unsigned timeout);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *wake);
esp_err_t esp_partition_mmap(const esp_partition_t *part, size_t offset, size_t size, int memory, const void **out, esp_partition_mmap_handle_t *handle);
