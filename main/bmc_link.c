#include "bmc_link.h"
#include "bmc_power.h"
#include "bmc_debug.h"
#include "app_ota.h"
#include "bmc_ota.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t commands;
static httpd_handle_t server;
static esp_netif_t *sta_netif, *ap_netif;
static bool initialized, netif_initialized, active, ready, ap_mode;
static char ssid[33], ip[16], token[33], ap_password[17];
static esp_err_t last_error;
static uint32_t generation;
static bmc_link_channel_fn assets;
static esp_event_handler_instance_t wifi_events, ip_events;
extern const uint8_t html_start[] asm("_binary_ota_html_start");
extern const uint8_t html_end[] asm("_binary_ota_html_end");
extern const uint8_t js_start[] asm("_binary_ota_js_start");
extern const uint8_t js_end[] asm("_binary_ota_js_end");

void bmc_link_invalidate(void)
{
    portENTER_CRITICAL(&state_lock);
    active = ready = false;
    ++generation;
    memset(token, 0, sizeof(token));
    memset(ap_password, 0, sizeof(ap_password));
    ip[0] = 0;
    portEXIT_CRITICAL(&state_lock);
}

static esp_err_t connect_sta(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&state_lock);
    bool connect = active && !ap_mode;
    portEXIT_CRITICAL(&state_lock);
    return connect ? esp_wifi_connect() : ESP_OK;
}

static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        bmc_wifi_apply(connect_sta, NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(ap_netif, &info) == ESP_OK && info.ip.addr) {
            char address[16];
            snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
            portENTER_CRITICAL(&state_lock);
            if (active && ap_mode) { memcpy(ip, address, sizeof(ip)); ready = true; }
            portEXIT_CRITICAL(&state_lock);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&state_lock);
        if (active && !ap_mode) { ready = false; ip[0] = 0; last_error = ESP_FAIL; }
        portEXIT_CRITICAL(&state_lock);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = data;
        if (!got->ip_info.ip.addr) return;
        char address[16];
        snprintf(address, sizeof(address), IPSTR, IP2STR(&got->ip_info.ip));
        portENTER_CRITICAL(&state_lock);
        if (active && !ap_mode) { memcpy(ip, address, sizeof(ip)); ready = true; last_error = ESP_OK; }
        portEXIT_CRITICAL(&state_lock);
    }
}

static esp_err_t error_reply(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, message);
}

static bool authorized(httpd_req_t *req, uint32_t *epoch)
{
    char auth[48], expected[40];
    if (httpd_req_get_hdr_value_len(req, "Authorization") != 39 ||
        httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) return false;
    portENTER_CRITICAL(&state_lock);
    bool valid = active && ready;
    memcpy(expected, "Bearer ", 7);
    memcpy(expected + 7, token, 33);
    *epoch = generation;
    portEXIT_CRITICAL(&state_lock);
    unsigned difference = 0;
    for (unsigned i = 0; i < 39; ++i) difference |= (unsigned char)auth[i] ^ (unsigned char)expected[i];
    return valid && difference == 0;
}

bool bmc_link_session_valid(uint32_t epoch)
{
    portENTER_CRITICAL(&state_lock);
    bool valid = active && ready && generation == epoch;
    portEXIT_CRITICAL(&state_lock);
    return valid;
}

typedef struct { const uint8_t *frame; uint32_t epoch; } submit_args_t;
static esp_err_t submit_under_power(void *opaque)
{
    submit_args_t *args = opaque;
    if (!bmc_link_session_valid(args->epoch)) return ESP_ERR_INVALID_STATE;
    return app_ota_submit_frame(args->frame, APP_OTA_SIZE);
}

static bool receive_body(httpd_req_t *req, uint8_t *bytes, size_t length)
{
    size_t received = 0;
    while (received < length) {
        int n = httpd_req_recv(req, (char *)bytes + received, length - received);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

static esp_err_t page(httpd_req_t *req)
{
    bool js = !strcmp(req->uri, "/ota.js");
    httpd_resp_set_type(req, js ? "application/javascript" : "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    return httpd_resp_send(req, (const char *)(js ? js_start : html_start),
                           (js ? js_end - js_start : html_end - html_start) - 1);
}

static esp_err_t benchmark(httpd_req_t *req, uint32_t epoch)
{
    if (!req->content_len || req->content_len > 256u * 1024u)
        return error_reply(req, "413 Payload Too Large", "{\"error\":\"benchmark_size\"}");
    uint8_t scratch[1024];
    size_t received = 0;
    uint32_t crc = 0;
    while (received < req->content_len) {
        size_t count = req->content_len - received;
        if (count > sizeof(scratch)) count = sizeof(scratch);
        if (!bmc_link_session_valid(epoch)) return error_reply(req, "401 Unauthorized", "{\"error\":\"expired\"}");
        if (!receive_body(req, scratch, count)) return error_reply(req, "400 Bad Request", "{\"error\":\"body_truncated\"}");
        if (!bmc_link_session_valid(epoch)) return error_reply(req, "401 Unauthorized", "{\"error\":\"expired\"}");
        crc = esp_rom_crc32_le(crc, scratch, count);
        received += count;
    }
    char response[80];
    snprintf(response, sizeof(response), "{\"bytes\":%lu,\"crc32\":%lu}",
             (unsigned long)received, (unsigned long)crc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t ota_stream(httpd_req_t *req, uint32_t epoch)
{
    if (!req->content_len || req->content_len % APP_OTA_SIZE || req->content_len > 16u * APP_OTA_SIZE)
        return error_reply(req, "413 Payload Too Large", "{\"error\":\"frame_size\"}");
    app_ota_frame_t *frame = malloc(APP_OTA_SIZE);
    if (!frame) return error_reply(req, "503 Service Unavailable", "{\"error\":\"memory\"}");
    app_ota_status_t status;
    uint32_t session = 0, previous_seq = 0;
    const char *failure_status = NULL, *failure_body = NULL;
    for (size_t offset = 0; offset < req->content_len; offset += APP_OTA_SIZE) {
        if (!receive_body(req, (uint8_t *)frame, APP_OTA_SIZE)) {
            failure_status = "400 Bad Request"; failure_body = "{\"error\":\"body_truncated\"}"; break;
        }
        if (!bmc_link_session_valid(epoch)) {
            failure_status = "401 Unauthorized"; failure_body = "{\"error\":\"expired\"}"; break;
        }
        if (offset && (frame->session != session || previous_seq == UINT32_MAX || frame->seq != previous_seq + 1)) {
            failure_status = "409 Conflict"; failure_body = "{\"error\":\"frame_sequence\"}"; break;
        }
        session = frame->session;
        previous_seq = frame->seq;
        submit_args_t submission = {.frame = (const uint8_t *)frame, .epoch = epoch};
        if (bmc_wifi_apply(submit_under_power, &submission) != ESP_OK) {
            failure_status = "409 Conflict"; failure_body = "{\"error\":\"frame_rejected\"}"; break;
        }
        TickType_t started = xTaskGetTickCount();
        bool acknowledged = false;
        do {
            if (!bmc_link_session_valid(epoch)) {
                failure_status = "401 Unauthorized"; failure_body = "{\"error\":\"expired\"}"; break;
            }
            app_ota_get_status(&status);
            if (status.error || status.state == 5) {
                failure_status = "502 Bad Gateway"; failure_body = "{\"error\":\"device_nack\"}"; break;
            }
            if (status.session == session && status.acknowledged_seq == previous_seq && (status.state == 3 || status.state == 4)) {
                acknowledged = true;
                break;
            }
            app_ota_wait_status(100);
        } while (xTaskGetTickCount() - started < pdMS_TO_TICKS(30000));
        if (failure_status) break;
        if (!acknowledged) {
            failure_status = "504 Gateway Timeout"; failure_body = "{\"error\":\"device_ack_timeout\"}"; break;
        }
    }
    free(frame);
    if (failure_status) return error_reply(req, failure_status, failure_body);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)&status, sizeof(status));
}

static esp_err_t api(httpd_req_t *req)
{
    uint32_t epoch;
    if (!authorized(req, &epoch)) return error_reply(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!strcmp(req->uri, "/v1/bmc/firmware")) return bmc_ota_http(req, epoch);
    if (!strcmp(req->uri, "/v1/capabilities")) {
        portENTER_CRITICAL(&state_lock);
        bool have_assets = assets != NULL;
        portEXIT_CRITICAL(&state_lock);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, have_assets ? "{\"protocol\":1,\"bmc_ota\":true,\"channels\":{\"ota\":true,\"assets\":true}}" :
            "{\"protocol\":1,\"bmc_ota\":true,\"channels\":{\"ota\":true,\"assets\":false}}");
    }
    if (!strcmp(req->uri, "/v1/status")) {
        app_ota_status_t status;
        app_ota_get_status(&status);
        httpd_resp_set_type(req, "application/octet-stream");
        return httpd_resp_send(req, (const char *)&status, sizeof(status));
    }
    if (!strcmp(req->uri, "/v1/control")) {
        char command[32], response[128];
        if (!req->content_len || req->content_len >= sizeof(command))
            return error_reply(req, "400 Bad Request", "{\"error\":\"command_size\"}");
        if (!receive_body(req, (uint8_t *)command, req->content_len)) return ESP_FAIL;
        command[req->content_len] = 0;
        if (strlen(command) != req->content_len || (strcmp(command, "ota") && strcmp(command, "ota-finish") &&
            strcmp(command, "ota-bmc") && strcmp(command, "ota-reboot") &&
            strcmp(command, "ota-abort") && strcmp(command, "boot")))
            return error_reply(req, "400 Bad Request", "{\"error\":\"command_not_allowed\"}");
        if (!bmc_link_session_valid(epoch)) return error_reply(req, "401 Unauthorized", "{\"error\":\"expired\"}");
        esp_err_t err = bmc_debug_link_command(command, response, sizeof(response), epoch);
        if (err != ESP_OK) return error_reply(req, "409 Conflict", "{\"error\":\"control_failed\"}");
        char confirmed[132];
        snprintf(confirmed, sizeof(confirmed), "OK %s", response);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, confirmed);
    }
    if (!strcmp(req->uri, "/v1/benchmark")) return benchmark(req, epoch);
    if (!strcmp(req->uri, "/v1/channels/ota")) return ota_stream(req, epoch);
    portENTER_CRITICAL(&state_lock);
    bmc_link_channel_fn receiver = assets;
    portEXIT_CRITICAL(&state_lock);
    if (!receiver) return error_reply(req, "501 Not Implemented", "{\"error\":\"assets_unavailable\"}");
    if (!req->content_len || req->content_len > APP_OTA_SIZE)
        return error_reply(req, "413 Payload Too Large", "{\"error\":\"frame_size\"}");
    uint8_t *frame = malloc(APP_OTA_SIZE);
    if (!frame) return error_reply(req, "503 Service Unavailable", "{\"error\":\"memory\"}");
    if (!receive_body(req, frame, req->content_len)) { free(frame); return ESP_FAIL; }
    if (!bmc_link_session_valid(epoch)) { free(frame); return error_reply(req, "401 Unauthorized", "{\"error\":\"expired\"}"); }
    uint8_t reply[256]; size_t length = sizeof(reply);
    esp_err_t err = receiver(frame, req->content_len, reply, &length);
    free(frame);
    if (err != ESP_OK || length > sizeof(reply)) return error_reply(req, "409 Conflict", "{\"error\":\"channel_failed\"}");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)reply, length);
}

esp_err_t bmc_link_register_channel(const char *name, bmc_link_channel_fn receive)
{
    if (!name || strcmp(name, "assets")) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&state_lock);
    assets = receive;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}

static esp_err_t open_socket(httpd_handle_t server_handle, int socket_fd)
{
    (void)server_handle;
    const int enabled = 1;
    /* Per-frame ACKs must not wait for Nagle and the peer's delayed ACK timer. */
    return setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t bmc_link_init(void)
{
    if (initialized) return ESP_OK;
    if (!commands) commands = xSemaphoreCreateMutex();
    if (!commands) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    if (!netif_initialized) {
        err = esp_netif_init();
        if (err != ESP_OK) return err;
        netif_initialized = true;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
    if (!sta_netif || !ap_netif) { err = ESP_ERR_NO_MEM; goto fail_netif; }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.static_rx_buf_num = 4;
    config.dynamic_rx_buf_num = 8;
    config.dynamic_tx_buf_num = 8;
    config.ampdu_rx_enable = 0;
    config.ampdu_tx_enable = 0;
    err = esp_wifi_init(&config);
    if (err != ESP_OK) goto fail_netif;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail_wifi;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL, &wifi_events);
    if (err != ESP_OK) goto fail_wifi;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL, &ip_events);
    if (err != ESP_OK) goto fail_events;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 6144;
    http.max_open_sockets = 2;
    http.max_uri_handlers = 10;
    http.lru_purge_enable = true;
    http.recv_wait_timeout = 5;
    http.send_wait_timeout = 5;
    http.open_fn = open_socket;
    err = httpd_start(&server, &http);
    if (err != ESP_OK) goto fail_ip;
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=page},
        {.uri="/ota.js", .method=HTTP_GET, .handler=page},
        {.uri="/v1/capabilities", .method=HTTP_GET, .handler=api},
        {.uri="/v1/benchmark", .method=HTTP_POST, .handler=api},
        {.uri="/v1/status", .method=HTTP_GET, .handler=api},
        {.uri="/v1/control", .method=HTTP_POST, .handler=api},
        {.uri="/v1/channels/ota", .method=HTTP_POST, .handler=api},
        {.uri="/v1/channels/assets", .method=HTTP_POST, .handler=api},
        {.uri="/v1/bmc/firmware", .method=HTTP_GET, .handler=api},
        {.uri="/v1/bmc/firmware", .method=HTTP_POST, .handler=api},
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) { httpd_stop(server); server = NULL; goto fail_ip; }
    }
    initialized = true;
    return ESP_OK;
fail_ip:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_events);
fail_events:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events);
fail_wifi:
    esp_wifi_deinit();
fail_netif:
    if (sta_netif) esp_netif_destroy_default_wifi(sta_netif);
    if (ap_netif) esp_netif_destroy_default_wifi(ap_netif);
    sta_netif = ap_netif = NULL;
    return err;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool utf8_valid(const uint8_t *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        uint8_t lead = text[i++];
        if (lead < 0x80) continue;
        unsigned count;
        uint32_t value, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; value = lead & 0x1f; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; value = lead & 0x0f; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; value = lead & 7; minimum = 0x10000; }
        else return false;
        if (count > length - i) return false;
        while (count--) {
            uint8_t next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static int decode(const char *text, uint8_t *out, size_t capacity)
{
    size_t n = strlen(text);
    if (!n || n % 2 || n / 2 > capacity) return -1;
    for (size_t i = 0; i < n / 2; ++i) {
        int a = hex_digit(text[i * 2]), b = hex_digit(text[i * 2 + 1]);
        if (a < 0 || b < 0 || !(a || b)) return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return utf8_valid(out, n / 2) ? (int)(n / 2) : -1;
}

static void random_hex(char *out, size_t bytes)
{
    uint8_t value[16];
    esp_fill_random(value, bytes);
    for (size_t i = 0; i < bytes; ++i) sprintf(out + i * 2, "%02x", value[i]);
}

typedef struct { bool ap; wifi_config_t config; char session_token[33]; } start_args_t;
static esp_err_t start_link(void *opaque)
{
    start_args_t *args = opaque;
    esp_err_t err = esp_wifi_set_mode(args->ap ? WIFI_MODE_AP : WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(args->ap ? WIFI_IF_AP : WIFI_IF_STA, &args->config);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&state_lock);
    ap_mode = args->ap;
    active = true; ready = false; last_error = ESP_OK;
    memcpy(token, args->session_token, sizeof(token));
    memset(ssid, 0, sizeof(ssid));
    memcpy(ssid, args->ap ? args->config.ap.ssid : args->config.sta.ssid, 32);
    if (args->ap) memcpy(ap_password, args->config.ap.password, sizeof(ap_password));
    portEXIT_CRITICAL(&state_lock);
    err = esp_wifi_start();
    if (err != ESP_OK) { bmc_link_invalidate(); return err; }
    esp_wifi_set_ps(WIFI_PS_NONE);
    return err;
}

static void status_json(char *response, size_t capacity)
{
    char escaped[193], name[33], address[16], secret[33], password[17];
    bool enabled, usable, ap;
    esp_err_t error;
    portENTER_CRITICAL(&state_lock);
    enabled = active; usable = ready; ap = ap_mode; error = last_error;
    memcpy(name, ssid, sizeof(name)); memcpy(address, ip, sizeof(address));
    memcpy(secret, token, sizeof(secret)); memcpy(password, ap_password, sizeof(password));
    portEXIT_CRITICAL(&state_lock);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || *p == '"' || *p == '\\') n += snprintf(escaped+n, sizeof(escaped)-n, "\\u%04x", *p);
        else escaped[n++] = *p;
    }
    escaped[n] = 0;
    snprintf(response, capacity,
        "{\"mode\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"url\":\"%s%s\",\"token\":\"%s\",\"ap_password\":\"%s\",\"error\":%d,\"heap_free\":%lu,\"heap_largest\":%lu}",
        enabled ? (ap ? "ap" : "sta") : "off", !enabled ? "off" : usable ? "ready" : error ? "error" : "connecting",
        escaped, usable ? address : "", usable ? "http://" : "", usable ? address : "",
        usable ? secret : "", enabled && ap ? password : "", (int)error,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

esp_err_t bmc_link_command(const char *cmd, char *response, size_t capacity)
{
    if (!cmd || !response || capacity < 500) return ESP_ERR_INVALID_ARG;
    if (!commands) commands = xSemaphoreCreateMutex();
    if (!commands) return ESP_ERR_NO_MEM;
    xSemaphoreTake(commands, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!strcmp(cmd, "wifi-status")) goto done;
    if (!strcmp(cmd, "wifi-stop")) {
        bmc_link_invalidate();
        if (initialized) err = bmc_wifi_stop();
        goto done;
    }
    start_args_t args = {0};
    args.ap = !strcmp(cmd, "wifi-ap");
    if (args.ap) {
        char suffix[9]; random_hex(suffix, 4);
        snprintf((char *)args.config.ap.ssid, sizeof(args.config.ap.ssid), "ePass-%s", suffix);
        random_hex((char *)args.config.ap.password, 8);
        args.config.ap.ssid_len = strlen((char *)args.config.ap.ssid);
        args.config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        args.config.ap.max_connection = 1;
        args.config.ap.channel = 1;
    } else if (!strncmp(cmd, "wifi-sta ", 9)) {
        char network[65], password[127], extra;
        if (sscanf(cmd + 9, "%64s %126s %c", network, password, &extra) != 2 ||
            decode(network, args.config.sta.ssid, 32) < 1 ||
            (strcmp(password, "-") && decode(password, args.config.sta.password, 63) < 0)) {
            err = ESP_ERR_INVALID_ARG; goto done;
        }
        if (strcmp(password, "-") && strlen((char *)args.config.sta.password) < 8) {
            err = ESP_ERR_INVALID_ARG; goto done;
        }
        args.config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else { err = ESP_ERR_INVALID_ARG; goto done; }
    if (!bmc_mainsys_enabled()) { err = ESP_ERR_INVALID_STATE; goto done; }
    err = bmc_link_init();
    if (err == ESP_OK) err = bmc_wifi_stop();
    if (err == ESP_OK) {
        random_hex(args.session_token, 16);
        err = bmc_wifi_apply(start_link, &args);
    }
    memset(&args, 0, sizeof(args));
done:
    if (err != ESP_OK) { portENTER_CRITICAL(&state_lock); last_error = err; portEXIT_CRITICAL(&state_lock); }
    status_json(response, capacity);
    xSemaphoreGive(commands);
    return err;
}
