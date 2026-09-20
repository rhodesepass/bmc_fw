#pragma once
#include "esp_idf_stub.h"
#include <string.h>
#define pdFALSE 0
#define pdPASS 1
#define DRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
#define portYIELD_FROM_ISR() ((void)0)
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int portMUX_TYPE;
typedef int BaseType_t;
typedef int nvs_handle_t;
typedef struct httpd_req httpd_req_t;
typedef void *QueueHandle_t;
static uint8_t mock_pending;
static uint8_t staged_pending;
static bool nvs_fail_commit;
static unsigned mock_slot;
static int mock_hold;
static int nvs_open(const char *s, int mode, nvs_handle_t *h) { (void)s; (void)mode; *h = 1; return ESP_OK; }
static int nvs_get_u8(nvs_handle_t h, const char *s, uint8_t *v) { (void)h; (void)s; *v=mock_pending; return ESP_OK; }
static int nvs_set_u8(nvs_handle_t h, const char *s, uint8_t v) { (void)h; (void)s; staged_pending=v; return ESP_OK; }
static int nvs_commit(nvs_handle_t h) { (void)h; if(nvs_fail_commit)return ESP_ERR_INVALID_STATE;mock_pending=staged_pending;return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }
static size_t queue_size;
static uint8_t queued_copy[4096];
static bool queue_ready;
static int queue_wait;
static QueueHandle_t xQueueCreate(int n, int s) { (void)n; queue_size=s; return (void *)1; }
static int xQueueReceive(QueueHandle_t q, void *v, int t) { (void)q; queue_wait=t; if(!queue_ready)return 0; memcpy(v,queued_copy,queue_size);queue_ready=false;return 1; }
static int xQueueSendFromISR(QueueHandle_t q, const void *v, BaseType_t *w) { (void)q; memcpy(queued_copy,v,queue_size);queue_ready=true;*w=1;return 1; }
static int xQueueSend(QueueHandle_t q, const void *v, unsigned ticks) { (void)q; (void)ticks; if(queue_ready)return 0; memcpy(queued_copy,v,queue_size);queue_ready=true;return 1; }
int xTaskCreate(void (*fn)(void *), const char *s, unsigned n, void *arg, unsigned p, TaskHandle_t *h) { (void)fn; (void)s; (void)n; (void)arg; (void)p; (void)h; return pdPASS; }
static const char *esp_err_to_name(int e) { return e ? "ERROR" : "OK"; }

static void xQueueReset(QueueHandle_t q) { (void)q;queue_ready=false; }
void vTaskDelay(int ticks) { (void)ticks; }

#define ESP_ERR_NVS_NOT_FOUND 11
static void vQueueDelete(QueueHandle_t q) { (void)q; }

static unsigned signal_count;
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)2; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)3; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) { (void)s;(void)t;return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s;signal_count++;return 1; }
