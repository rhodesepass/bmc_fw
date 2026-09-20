#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_SIZE -3
#define ESP_ERR_INVALID_ARG -4
#define ESP_ERR_INVALID_CRC -5
#define ESP_ERR_NO_MEM -6
#define portMAX_DELAY 0
typedef int *SemaphoreHandle_t;
static int mutex_taken;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_taken; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned delay) { (void)delay; assert(!*mutex); *mutex=1; return 1; }
static int xSemaphoreGive(SemaphoreHandle_t mutex) { assert(*mutex); *mutex=0; return 1; }
#define HTTP_GET 0
#define HTTP_POST 1
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
#define CONFIG_IDF_FIRMWARE_CHIP_ID 5
#define OTA_WITH_SEQUENTIAL_WRITES ((size_t)-1)
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
#define PSA_HASH_OPERATION_INIT {0}
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef int portMUX_TYPE;
typedef unsigned esp_ota_handle_t;
typedef struct { uint8_t magic, pad[11]; uint16_t chip_id; uint8_t rest[10]; } esp_image_header_t;
typedef struct { uint32_t address, size; } esp_image_segment_header_t;
typedef struct { uint32_t magic_word; uint8_t pad[12]; char version[32], project_name[32]; uint8_t rest[176]; } esp_app_desc_t;
typedef struct { uint32_t address, size; char label[17]; } esp_partition_t;
typedef struct { size_t bytes; } psa_hash_operation_t;
typedef struct {
    int method;
    size_t content_len, offset;
    const uint8_t *body;
    const char *digest;
    char status[64], response[256];
} httpd_req_t;

static esp_partition_t slots[] = {{0x10000, 16384, "ota_0"}, {0x50000, 16384, "ota_1"}};
static const esp_partition_t *boot_slot;
static esp_app_desc_t current_app;
static unsigned begin_calls, write_calls, end_calls, abort_calls, set_boot_calls, restart_calls;
static unsigned unlock_calls, session_calls, expire_on_call;
static size_t written_bytes, hashed_bytes, interrupt_at;
static int end_error, boot_error, write_error, send_error;
static bool busy, interlock_error;
static int64_t now;
static const uint8_t *expected_body;
static esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *v) { strcpy(r->status, v); return ESP_OK; }
static esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *v) { (void)r; assert(!strcmp(v, "application/json")); return ESP_OK; }
static esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *v) { snprintf(r->response, sizeof(r->response), "%s", v); return send_error; }
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name) { assert(!strcmp(name,"X-SHA256")); return strlen(r->digest); }
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t n) { (void)name; snprintf(out,n,"%s",r->digest); return ESP_OK; }
static int httpd_req_recv(httpd_req_t *r, char *out, size_t n) {
    if (r->offset >= interrupt_at) return -1;
    if (n > 127) n = 127;
    if (n > interrupt_at - r->offset) n = interrupt_at - r->offset;
    assert(r->offset + n <= r->content_len);
    memcpy(out, r->body + r->offset, n); r->offset += n; return (int)n;
}
static const esp_partition_t *esp_ota_get_running_partition(void) { return &slots[0]; }
static const esp_partition_t *esp_ota_get_boot_partition(void) { return boot_slot; }
static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { (void)p; return &slots[1]; }
static const esp_app_desc_t *esp_app_get_description(void) { return &current_app; }
static esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) { assert(p == &slots[1] && n == OTA_WITH_SEQUENTIAL_WRITES); begin_calls++; *h=7; return ESP_OK; }
static esp_err_t esp_ota_write(esp_ota_handle_t h, const void *b, size_t n) { assert(h==7); assert(!memcmp(b,expected_body+written_bytes,n)); write_calls++; written_bytes+=n; return write_error; }
static esp_err_t esp_ota_end(esp_ota_handle_t h) { assert(h==7); end_calls++; return end_error; }
static esp_err_t esp_ota_abort(esp_ota_handle_t h) { assert(h==7); abort_calls++; return ESP_OK; }
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { assert(p==&slots[1]); set_boot_calls++; if (!boot_error) boot_slot=p; return boot_error; }
static int64_t esp_timer_get_time(void) { return now; }
static void esp_restart(void) { restart_calls++; }
static int psa_crypto_init(void) { return PSA_SUCCESS; }
static int psa_hash_setup(psa_hash_operation_t *h, int algorithm) { assert(algorithm==PSA_ALG_SHA_256); h->bytes=0; return PSA_SUCCESS; }
static int psa_hash_update(psa_hash_operation_t *h, const uint8_t *b, size_t n) { assert(!memcmp(b,expected_body+h->bytes,n)); h->bytes+=n; hashed_bytes+=n; return PSA_SUCCESS; }
static int psa_hash_finish(psa_hash_operation_t *h, uint8_t *out, size_t n, size_t *written) { (void)h; assert(n==32); memset(out,0x12,n); *written=n; return PSA_SUCCESS; }
static int psa_hash_abort(psa_hash_operation_t *h) { h->bytes=0; return PSA_SUCCESS; }
