#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../main/app_ota_wire.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_AP_START 3
#define IP_EVENT_STA_GOT_IP 4
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_AP 1
#define WIFI_MODE_STA 2
#define WIFI_IF_AP 1
#define WIFI_IF_STA 2
#define WIFI_PS_NONE 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_AUTH_OPEN 0
#define HTTP_GET 1
#define HTTP_POST 2
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portMAX_DELAY 1000
#define pdMS_TO_TICKS(n) (n)
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) 192u, 168u, 4u, ((p)->addr ? 1u : 0u)
typedef int portMUX_TYPE;
typedef int *SemaphoreHandle_t;
typedef unsigned TickType_t;
typedef int esp_event_base_t;
typedef int esp_event_handler_instance_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { int unused; } esp_netif_t;
typedef struct { int static_rx_buf_num, dynamic_rx_buf_num, dynamic_tx_buf_num, ampdu_rx_enable, ampdu_tx_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
typedef union {
    struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold; } sta;
    struct { uint8_t ssid[32], password[64]; unsigned ssid_len, authmode, max_connection, channel; } ap;
} wifi_config_t;
typedef struct { const char *uri; size_t content_len; const uint8_t *body; size_t offset, cutoff, chunk_limit, expire_after; const char *auth; int expire_on_recv; char response[512], status[64]; size_t response_len; } httpd_req_t;
typedef void *httpd_handle_t;
typedef struct { int stack_size, max_open_sockets, max_uri_handlers; bool lru_purge_enable; int recv_wait_timeout, send_wait_timeout; esp_err_t (*open_fn)(httpd_handle_t, int); } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() {0}
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
static unsigned clock_ticks, init_calls, netif_init_calls, netif_created, netif_destroyed, connect_calls, control_calls, submit_calls;
static bool power_enabled = true, expire_at_control;
static int init_error, http_error, submit_error, fake_error, socket_option_error;
static unsigned socket_option_calls;
static esp_err_t (*session_open)(httpd_handle_t, int);
static bool ack_enabled = true;
static uint32_t ack_stop_seq, reject_seq;
static size_t max_allocation, live_allocations;
static esp_netif_t fake_netif;
static uint32_t fake_ip;
static app_ota_status_t fake_status;
static uint8_t submitted[APP_OTA_SIZE];
void bmc_link_invalidate(void);
bool bmc_link_session_valid(uint32_t epoch);
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int mutex; return &mutex; }
static inline int xSemaphoreTake(SemaphoreHandle_t p, int timeout) { (void)p; (void)timeout; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t p) { (void)p; return 1; }
static inline TickType_t xTaskGetTickCount(void) { return clock_ticks; }
static inline void vTaskDelay(unsigned n) { clock_ticks += n; }
static inline esp_err_t esp_wifi_connect(void) { ++connect_calls; return ESP_OK; }
static inline esp_err_t esp_netif_init(void) { ++netif_init_calls; return ESP_OK; }
static inline esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void) { ++netif_created; return &fake_netif; }
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void) { ++netif_created; return &fake_netif; }
static inline void esp_netif_destroy_default_wifi(esp_netif_t *netif) { (void)netif; ++netif_destroyed; }
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *config) { (void)config; ++init_calls; return init_error; }
static inline esp_err_t esp_wifi_deinit(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_storage(int n) { (void)n; return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_register(int base, int id, void (*handler)(void *,int,int,void *), void *arg, int *instance) { (void)base;(void)id;(void)handler;(void)arg;*instance=1;return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_unregister(int base,int id,int instance) { (void)base;(void)id;(void)instance;return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(int mode,const wifi_config_t *config) { (void)mode;(void)config;return ESP_OK; }
static inline esp_err_t esp_wifi_start(void) { assert(power_enabled); return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(int mode) { (void)mode; return ESP_OK; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,esp_netif_ip_info_t *info) { (void)netif;info->ip.addr=fake_ip;return ESP_OK; }
static inline void esp_fill_random(void *buffer,size_t length) { static unsigned seed; memset(buffer, ++seed, length); }
static inline esp_err_t httpd_start(httpd_handle_t *server,const httpd_config_t *config) { session_open=config->open_fn;*server=(void *)1;return http_error; }
static inline esp_err_t httpd_stop(httpd_handle_t server) { (void)server;return ESP_OK; }
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *uri) { (void)server;(void)uri;return ESP_OK; }
static inline esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *status) { snprintf(r->status,sizeof(r->status),"%s",status);return ESP_OK; }
static inline esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *type) { (void)r;(void)type;return ESP_OK; }
static inline esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *key,const char *value) { (void)r;(void)key;(void)value;return ESP_OK; }
static inline esp_err_t httpd_resp_send(httpd_req_t *r,const char *data,size_t length) { assert(length < sizeof(r->response));memcpy(r->response,data,length);r->response[length]=0;r->response_len=length;return ESP_OK; }
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *data) { return httpd_resp_send(r,data,strlen(data)); }
static inline size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *key) { (void)key;return r->auth ? strlen(r->auth) : 0; }
static inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *key,char *out,size_t n) { (void)key;snprintf(out,n,"%s",r->auth);return ESP_OK; }
static inline int httpd_req_recv(httpd_req_t *r,char *out,size_t n) {
    if(r->cutoff && r->offset>=r->cutoff) return -1;
    if(n>r->content_len-r->offset)n=r->content_len-r->offset;
    if(r->cutoff && n>r->cutoff-r->offset)n=r->cutoff-r->offset;
    if(r->chunk_limit && n>r->chunk_limit)n=r->chunk_limit;
    memcpy(out,r->body+r->offset,n);r->offset+=n;
    if(r->expire_on_recv || (r->expire_after && r->offset>=r->expire_after))bmc_link_invalidate();
    return n;
}
esp_err_t bmc_debug_link_command(const char *command,char *response,size_t capacity,uint32_t epoch);

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static inline size_t heap_caps_get_free_size(unsigned caps) { assert(caps == 3); return 65536; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps == 3); return 32768; }

#define IPPROTO_TCP 6
#define TCP_NODELAY 1
static inline int setsockopt(int socket_fd, int level, int option, const void *value, size_t length)
{
    assert(socket_fd == 42 && level == IPPROTO_TCP && option == TCP_NODELAY);
    assert(length == sizeof(int) && *(const int *)value == 1);
    ++socket_option_calls;
    return socket_option_error;
}

static inline void *tracked_malloc(size_t length)
{
    if(length>max_allocation)max_allocation=length;
    void *result=malloc(length);if(result)++live_allocations;return result;
}
static inline void tracked_free(void *memory) { if(memory)--live_allocations;free(memory); }
static inline uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t *data,uint32_t length)
{
    crc=~crc;
    for(uint32_t i=0;i<length;++i) {
        crc^=data[i];
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
