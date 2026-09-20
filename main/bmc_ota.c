#include "bmc_ota.h"
#include "bmc_debug.h"
#include "bmc_link.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static portMUX_TYPE restart_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t restart_at;
static SemaphoreHandle_t ota_lock;
static struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    psa_hash_operation_t hash;
    uint32_t total, received;
    uint8_t expected[32];
    uint8_t prefix[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    size_t prefix_used;
    bool active, opened, staged, committed;
} stage;

esp_err_t bmc_ota_init(void)
{
    if (!ota_lock) ota_lock = xSemaphoreCreateMutex();
    return ota_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void discard(void)
{
    if (stage.opened) esp_ota_abort(stage.handle);
    psa_hash_abort(&stage.hash);
    memset(&stage, 0, sizeof(stage));
}

esp_err_t bmc_ota_stage_begin(uint32_t total, const uint8_t hash[32])
{
    if (!ota_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (stage.active || stage.staged || stage.committed) goto done;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!partition || !running || !boot || partition->address == running->address ||
        boot->address != running->address) goto done;
    err = ESP_ERR_INVALID_SIZE;
    if (total < sizeof(stage.prefix) || total > partition->size) goto done;
    err = ESP_ERR_INVALID_ARG;
    if (!hash) goto done;
    stage.hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    err = ESP_FAIL;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&stage.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        discard();
        goto done;
    }
    stage.partition = partition;
    stage.total = total;
    memcpy(stage.expected, hash, sizeof(stage.expected));
    stage.active = true;
    err = ESP_OK;
done:
    xSemaphoreGive(ota_lock);
    return err;
}

esp_err_t bmc_ota_stage_write(const uint8_t *data, size_t size)
{
    if (!ota_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!stage.active) goto done;
    err = ESP_ERR_INVALID_SIZE;
    if ((!data && size) || size > stage.total - stage.received) goto failed;
    if (!size) { err = ESP_OK; goto done; }
    err = ESP_FAIL;
    if (psa_hash_update(&stage.hash, data, size) != PSA_SUCCESS) goto failed;
    stage.received += size;
    if (!stage.opened) {
        size_t count = sizeof(stage.prefix) - stage.prefix_used;
        if (count > size) count = size;
        memcpy(stage.prefix + stage.prefix_used, data, count);
        stage.prefix_used += count;
        data += count;
        size -= count;
        if (stage.prefix_used < sizeof(stage.prefix)) { err = ESP_OK; goto done; }
        esp_image_header_t image;
        esp_app_desc_t app;
        memcpy(&image, stage.prefix, sizeof(image));
        memcpy(&app, stage.prefix + sizeof(image) + sizeof(esp_image_segment_header_t), sizeof(app));
        err = ESP_ERR_INVALID_ARG;
        if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
            app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            memcmp(app.project_name, esp_app_get_description()->project_name, sizeof(app.project_name))) goto failed;
        err = esp_ota_begin(stage.partition, OTA_WITH_SEQUENTIAL_WRITES, &stage.handle);
        if (err != ESP_OK) goto failed;
        stage.opened = true;
        err = esp_ota_write(stage.handle, stage.prefix, sizeof(stage.prefix));
        if (err != ESP_OK) goto failed;
    }
    err = size ? esp_ota_write(stage.handle, data, size) : ESP_OK;
    if (err == ESP_OK) goto done;
failed:
    discard();
done:
    xSemaphoreGive(ota_lock);
    return err;
}

esp_err_t bmc_ota_stage_end(void)
{
    if (!ota_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!stage.active) goto done;
    err = ESP_ERR_INVALID_SIZE;
    if (stage.received != stage.total || !stage.opened) goto failed;
    uint8_t actual[32];
    size_t size = 0;
    err = ESP_ERR_INVALID_CRC;
    if (psa_hash_finish(&stage.hash, actual, sizeof(actual), &size) != PSA_SUCCESS ||
        size != sizeof(actual) || memcmp(actual, stage.expected, sizeof(actual))) goto failed;
    err = esp_ota_end(stage.handle);
    stage.opened = false;
    if (err != ESP_OK) goto failed;
    psa_hash_abort(&stage.hash);
    stage.active = false;
    stage.staged = true;
    goto done;
failed:
    discard();
done:
    xSemaphoreGive(ota_lock);
    return err;
}

esp_err_t bmc_ota_stage_abort(void)
{
    if (!ota_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    esp_err_t err = stage.committed ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) discard();
    xSemaphoreGive(ota_lock);
    return err;
}

esp_err_t bmc_ota_commit(void)
{
    if (!ota_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (stage.committed) err = ESP_OK;
    else if (stage.staged) {
        err = esp_ota_set_boot_partition(stage.partition);
        if (err == ESP_OK) stage.committed = true;
    }
    xSemaphoreGive(ota_lock);
    return err;
}

esp_err_t bmc_ota_reboot(void)
{
    if (!ota_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    esp_err_t err = stage.committed ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        portENTER_CRITICAL(&restart_lock);
        if (!restart_at) restart_at = esp_timer_get_time() + 2000000;
        portEXIT_CRITICAL(&restart_lock);
    }
    xSemaphoreGive(ota_lock);
    return err;
}

bool bmc_ota_staged(void)
{
    if (!ota_lock) return false;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    bool value = stage.staged;
    xSemaphoreGive(ota_lock);
    return value;
}

bool bmc_ota_committed(void)
{
    if (!ota_lock) return false;
    xSemaphoreTake(ota_lock, portMAX_DELAY);
    bool value = stage.committed;
    xSemaphoreGive(ota_lock);
    return value;
}

void bmc_ota_poll(void)
{
    portENTER_CRITICAL(&restart_lock);
    int64_t deadline = restart_at;
    portEXIT_CRITICAL(&restart_lock);
    if (deadline && esp_timer_get_time() >= deadline) esp_restart();
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void json_string(char *out, const char *in, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size && in[i]; ++i) {
        unsigned char c = in[i];
        if (c == '"' || c == '\\') { *out++ = '\\'; *out++ = c; }
        else if (c < 32 || c >= 127) {
            memcpy(out, "\\u00", 4); out += 4;
            *out++ = digits[c >> 4]; *out++ = digits[c & 15];
        } else *out++ = c;
    }
    *out = 0;
}

static esp_err_t info(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    char running_name[103], boot_name[103], version[193], project[193], json[800];
    json_string(running_name, running ? running->label : "", 17);
    json_string(boot_name, boot ? boot->label : "", 17);
    json_string(version, app->version, sizeof(app->version));
    json_string(project, app->project_name, sizeof(app->project_name));
    snprintf(json, sizeof(json), "{\"running_partition\":\"%s\",\"boot_partition\":\"%s\",\"version\":\"%s\",\"project\":\"%s\",\"staged\":%s,\"committed\":%s}",
             running_name, boot_name, version, project, bmc_ota_staged() ? "true" : "false", bmc_ota_committed() ? "true" : "false");
    return reply(req, "200 OK", json);
}

esp_err_t bmc_ota_http(httpd_req_t *req, uint32_t epoch)
{
    if (req->method == HTTP_GET) return info(req);
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    /* Do not erase a previously committed image whose reboot is still pending. */
    if (!partition || !running || !boot || partition->address == running->address ||
        boot->address != running->address)
        return reply(req, "409 Conflict", "{\"error\":\"slot_unavailable\"}");
    const size_t prefix_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (req->content_len < prefix_size || req->content_len > partition->size)
        return reply(req, "413 Payload Too Large", "{\"error\":\"image_size\"}");
    char hash_text[65];
    uint8_t expected[32];
    if (httpd_req_get_hdr_value_len(req, "X-SHA256") != 64 ||
        httpd_req_get_hdr_value_str(req, "X-SHA256", hash_text, sizeof(hash_text)) != ESP_OK)
        return reply(req, "400 Bad Request", "{\"error\":\"sha256_required\"}");
    for (unsigned i = 0; i < sizeof(expected); ++i) {
        int a = hex(hash_text[i * 2]), b = hex(hash_text[i * 2 + 1]);
        if (a < 0 || b < 0) return reply(req, "400 Bad Request", "{\"error\":\"sha256_invalid\"}");
        expected[i] = (a << 4) | b;
    }
    if (bmc_debug_self_ota_begin() != ESP_OK)
        return reply(req, "409 Conflict", "{\"error\":\"busy\"}");

    uint8_t *buffer = malloc(4096);
    bool staged = false, started = false;
    const char *failure = "{\"error\":\"ota_failed\"}";
    const char *status = "500 Internal Server Error";
    if (!buffer) goto done;
    if (bmc_ota_stage_begin(req->content_len, expected) != ESP_OK) goto done;
    started = true;
    for (size_t offset = 0; offset < req->content_len;) {
        size_t count = req->content_len - offset;
        if (count > 4096) count = 4096;
        size_t received = 0;
        while (received < count) {
            if (!bmc_link_session_valid(epoch)) {
                status = "401 Unauthorized"; failure = "{\"error\":\"expired\"}"; goto done;
            }
            int n = httpd_req_recv(req, (char *)buffer + received, count - received);
            if (n <= 0) {
                status = "400 Bad Request"; failure = "{\"error\":\"body_truncated\"}"; goto done;
            }
            received += n;
        }
        if (!bmc_link_session_valid(epoch)) {
            status = "401 Unauthorized"; failure = "{\"error\":\"expired\"}"; goto done;
        }
        esp_err_t err = bmc_ota_stage_write(buffer, count);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_ARG) {
                status = "400 Bad Request"; failure = "{\"error\":\"wrong_image\"}";
            }
            goto done;
        }
        offset += count;
    }
    esp_err_t err = bmc_ota_stage_end();
    if (err != ESP_OK) {
        status = "400 Bad Request";
        failure = err == ESP_ERR_INVALID_CRC ? "{\"error\":\"sha256_mismatch\"}" : "{\"error\":\"invalid_image\"}";
        goto done;
    }
    staged = true;
done:
    free(buffer);
    if (!staged) {
        if (started) bmc_ota_stage_abort();
        bmc_debug_self_ota_end();
        return reply(req, status, failure);
    }
    char response[96];
    snprintf(response, sizeof(response), "{\"status\":\"staged\",\"partition\":\"%s\",\"reboot\":false}", partition->label);
    return reply(req, "200 OK", response);
}
