#include "app_ota.h"
#include "spl_nand.h"
#include "bmc_power.h"
#include "bmc_runtime.h"
#include "bmc_ota.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static DMA_ATTR app_ota_frame_t idle, idle_request, outgoing[2];
static app_ota_frame_t assembly;
static const app_ota_frame_t *volatile published;
static const app_ota_frame_t *volatile dma_active;
typedef struct { uint32_t generation; bool local; app_ota_frame_t frame; } rx_item_t;
static DRAM_ATTR rx_item_t rx_staging;
static QueueHandle_t incoming;
static SemaphoreHandle_t status_changed;
static SemaphoreHandle_t local_operation;
static app_ota_status_t status = {.magic = APP_OTA_MAGIC};
static uint32_t assembled, total, target;
static bool pending, requested, completed_image, completed_fit;
static bool transaction_finished;
static bool finishing;
static volatile bool local_mode;
static volatile bool protocol_mode;
static bool queued, ingress_busy;
static enum { UPLOAD_NONE, UPLOAD_BLE, UPLOAD_HTTP } upload_owner;
static const app_ota_frame_t *last_http;
static const app_ota_frame_t *last_local;
static uint32_t epoch;

static esp_err_t persist(bool value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_ota", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "pending", value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) pending = value;
    return err;
}

void app_ota_spi_reset(void)
{
    portENTER_CRITICAL(&lock);
    ++epoch;
    if (incoming) xQueueReset(incoming);
    protocol_mode = false;
    upload_owner = UPLOAD_NONE;
    last_http = NULL;
    last_local = NULL;
    dma_active = NULL;
    published = requested ? &idle_request : &idle;
    portEXIT_CRITICAL(&lock);
}

void app_ota_normal_boot(void)
{
    portENTER_CRITICAL(&lock);
    requested = false;
    local_mode = false;
    finishing = false;
    completed_fit = false;
    queued = false;
    assembled = 0;
    status = (app_ota_status_t){.magic = APP_OTA_MAGIC};
    portEXIT_CRITICAL(&lock);
    app_ota_spi_reset();
}

const uint8_t *IRAM_ATTR app_ota_spi_done(const uint8_t *rx, size_t bits)
{
    if (!incoming) return NULL;
    if (local_mode) return NULL;
    /* BootROM reset must get back to NAND emulation after an APP-side reboot. */
    if (bits == 8) {
        protocol_mode = false;
        return NULL;
    }
    if (bits == APP_OTA_SIZE * 8 && *(const uint32_t *)rx == APP_OTA_MAGIC) {
        BaseType_t wake = pdFALSE;
        rx_staging.generation = epoch;
        rx_staging.local = false;
        memcpy(&rx_staging.frame, rx, APP_OTA_SIZE);
        xQueueSendFromISR(incoming, &rx_staging, &wake);
        if (wake) portYIELD_FROM_ISR();
    }
    /* Only the task's fully validated HELLO changes the protocol mode. */
    if (!protocol_mode) return NULL;
    dma_active = published;
    return (const uint8_t *)dma_active;
}

static void receive(const app_ota_frame_t *f, uint32_t generation)
{
    if (!app_ota_valid(f) || f->length || f->op < 1 || f->op > 4) return;
    portENTER_CRITICAL(&lock);
    if (generation != epoch || local_mode || finishing) { portEXIT_CRITICAL(&lock); return; }
    if (f->op == 1 && !f->session && !f->seq && !f->offset && !f->status &&
        (f->arg == 1 || f->arg == 2) &&
        (!requested || (f->arg == 1 && !completed_fit) || (f->arg == 2 && completed_fit))) {
        protocol_mode = true;
        status.stage = f->arg;
    } else if (requested && protocol_mode && !queued && f->op == 4 && f->status &&
               f->session == status.session && f->seq == status.acknowledged_seq) {
        status.error = f->status;
        status.state = 5;
        assembled = 0;
        published = &idle_request;
    } else if (requested && protocol_mode && queued && f->session == published->session &&
               f->seq == published->seq && (f->op == 3 || f->op == 4)) {
        const app_ota_frame_t *q = published;
        uint32_t consumed = q->offset + (q->op == 11 ? q->length : 0);
        if (f->op == 4 || f->status) {
            status.error = f->status ? f->status : 1;
            status.state = 5;
            queued = false;
            published = requested ? &idle_request : &idle;
        } else if (f->offset == consumed && f->arg >= status.durable && f->arg <= consumed &&
                   (target != 4 || !f->arg) &&
                   (target != 6 || q->op == 12 || !f->arg) &&
                   (q->op != 12 || target == 4 || target == 5 || f->arg == total)) {
            status.session = f->session;
            status.acknowledged_seq = f->seq;
            status.consumed = consumed;
            status.durable = f->arg;
            status.state = q->op == 12 ? 4 : 3;
            if (q->op == 12 && target == 4) completed_fit = true;
            if (q->op == 12 && ((target >= 1 && target <= 3) || target == 6)) completed_image = true;
            queued = false;
            published = requested ? &idle_request : &idle;
        }
    }
    portEXIT_CRITICAL(&lock);
}
static void receive_local(const rx_item_t *item)
{
    xSemaphoreTake(local_operation, portMAX_DELAY);
    portENTER_CRITICAL(&lock);
    bool current = item->generation == epoch && local_mode && requested && queued;
    portEXIT_CRITICAL(&lock);
    if (current) {
        const app_ota_frame_t *f = &item->frame;
        esp_err_t err;
        if (f->op == 10) {
            uint32_t size;
            memcpy(&size, f->payload, sizeof(size));
            err = bmc_ota_stage_begin(size, f->payload + 8);
        } else if (f->op == 11) err = bmc_ota_stage_write(f->payload, f->length);
        else err = bmc_ota_stage_end();
        if (err != ESP_OK) bmc_ota_stage_abort();
        portENTER_CRITICAL(&lock);
        if (item->generation == epoch) {
            queued = false;
            if (err == ESP_OK) {
                status.acknowledged_seq = f->seq;
                status.consumed = f->offset + (f->op == 11 ? f->length : 0);
                status.state = f->op == 12 ? 4 : 3;
                status.durable = f->op == 12 ? total : 0;
                if (f->op == 12) completed_image = true;
            } else {
                status.state = 5;
                status.error = (uint32_t)err;
            }
        }
        portEXIT_CRITICAL(&lock);
    }
    xSemaphoreGive(local_operation);
}

static void receive_one(void)
{
    rx_item_t item;
    if (xQueueReceive(incoming, &item, portMAX_DELAY) == pdTRUE) {
        if (item.local) receive_local(&item);
        else receive(&item.frame, item.generation);
        xSemaphoreGive(status_changed);
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) receive_one();
}

void app_ota_wait_status(uint32_t timeout_ms)
{
    if (status_changed) xSemaphoreTake(status_changed, pdMS_TO_TICKS(timeout_ms) + 1);
}

esp_err_t app_ota_init(void)
{
    if (incoming) return ESP_ERR_INVALID_STATE;
    esp_err_t core_err = bmc_ota_init();
    if (core_err != ESP_OK) return core_err;
    if (!local_operation) local_operation = xSemaphoreCreateMutex();
    if (!local_operation) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_ota", NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t value = 0;
        err = nvs_get_u8(h, "pending", &value);
        nvs_close(h);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        pending = value != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    app_ota_seal(&idle);
    idle_request.arg = 1;
    app_ota_seal(&idle_request);
    published = requested ? &idle_request : &idle;
    if (!status_changed) status_changed = xSemaphoreCreateBinary();
    if (!status_changed) return ESP_ERR_NO_MEM;
    incoming = xQueueCreate(4, sizeof(rx_item_t));
    if (!incoming) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "app_ota", 6144, NULL, 6, NULL) != pdPASS) {
        vQueueDelete(incoming);
        incoming = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool app_ota_pending(void) { return pending; }
void app_ota_get_status(app_ota_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    if (assembled && !queued) out->state = 1;
    portEXIT_CRITICAL(&lock);
}
static esp_err_t publish_assembly(const app_ota_status_t *snapshot, uint32_t generation,
                                  uint32_t image_total, uint32_t image_target, bool http)
{
    bool valid = app_ota_upload_valid(&assembly, snapshot, image_total, image_target);
    rx_item_t local_item;
    portENTER_CRITICAL(&lock);
    esp_err_t err = ESP_OK;
    if (!requested || finishing || queued || generation != epoch || status.stage != snapshot->stage)
        err = ESP_ERR_INVALID_STATE;
    else if (local_mode && last_local && assembly.session == last_local->session && assembly.seq == last_local->seq) {
        err = memcmp(&assembly, last_local, sizeof(assembly)) ? ESP_ERR_INVALID_ARG : ESP_OK;
        assembled = 0;
        ingress_busy = false;
        portEXIT_CRITICAL(&lock);
        return err;
    } else if (!valid)
        err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        app_ota_frame_t *dst = dma_active == &outgoing[0] ? &outgoing[1] : &outgoing[0];
        memcpy(dst, &assembly, sizeof(*dst));
        if (dst->op == 10) {
            memcpy(&total, dst->payload, 4); memcpy(&target, dst->payload + 4, 4);
            status.session = dst->session;
            status.acknowledged_seq = status.consumed = status.durable = status.error = 0;
        }
        status.state = 2;
        queued = true;
        if (local_mode) {
            local_item.generation = generation;
            local_item.local = true;
            local_item.frame = *dst;
            if (xQueueSend(incoming, &local_item, 0) != pdTRUE) {
                queued = false;
                status.state = 5;
                status.error = ESP_ERR_NO_MEM;
                err = ESP_ERR_NO_MEM;
            } else last_local = dst;
        } else published = dst;
        if (http) { upload_owner = UPLOAD_HTTP; last_http = dst; }
    }
    assembled = 0;
    ingress_busy = false;
    portEXIT_CRITICAL(&lock);
    return err;
}

esp_err_t app_ota_upload(const uint8_t *data, size_t len)
{
    uint32_t offset;
    if (!data || len <= 4 || len > 244) return ESP_ERR_INVALID_SIZE;
    memcpy(&offset, data, 4);
    portENTER_CRITICAL(&lock);
    if (!requested || finishing || !status.stage || queued || status.state == 5 ||
        ingress_busy || upload_owner == UPLOAD_HTTP) {
        portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_STATE;
    }
    if (offset == 0) assembled = 0;
    if (offset != assembled || len - 4 > APP_OTA_SIZE - assembled) {
        portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_ARG;
    }
    upload_owner = UPLOAD_BLE;
    memcpy((uint8_t *)&assembly + assembled, data + 4, len - 4);
    assembled += len - 4;
    if (assembled != APP_OTA_SIZE) { portEXIT_CRITICAL(&lock); return ESP_OK; }
    ingress_busy = true;
    app_ota_status_t snapshot = status;
    uint32_t generation = epoch, image_total = total, image_target = target;
    portEXIT_CRITICAL(&lock);
    return publish_assembly(&snapshot, generation, image_total, image_target, false);
}

esp_err_t app_ota_submit_frame(const uint8_t *data, size_t len)
{
    uint32_t session_id, sequence;
    if (!data || len != APP_OTA_SIZE) return ESP_ERR_INVALID_SIZE;
    memcpy(&session_id, data + 8, 4); memcpy(&sequence, data + 12, 4);
    portENTER_CRITICAL(&lock);
    if (!requested || finishing || !status.stage || status.state == 5 || ingress_busy ||
        upload_owner == UPLOAD_BLE || assembled) {
        portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_STATE;
    }
    /* Keep the last published DMA frame for retries after a lost HTTP response. */
    if (last_http && session_id == last_http->session && sequence == last_http->seq) {
        bool same = memcmp(last_http, data, len) == 0;
        portEXIT_CRITICAL(&lock);
        return same ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (queued) { portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_STATE; }
    ingress_busy = true;
    memcpy(&assembly, data, len);
    app_ota_status_t snapshot = status;
    uint32_t generation = epoch, image_total = total, image_target = target;
    portEXIT_CRITICAL(&lock);
    return publish_assembly(&snapshot, generation, image_total, image_target, true);
}

static esp_err_t command_locked(const char *cmd, char *response, size_t capacity)
{
    esp_err_t err = ESP_OK;
    if (!strcmp(cmd, "ota-status")) {
        app_ota_status_t s; app_ota_get_status(&s);
        snprintf(response, capacity, "pending=%u requested=%u stage=%lu state=%lu session=%lu ack=%lu consumed=%lu durable=%lu error=%lu bmc_staged=%u bmc_committed=%u",
                 pending, requested, (unsigned long)s.stage, (unsigned long)s.state, (unsigned long)s.session,
                 (unsigned long)s.acknowledged_seq, (unsigned long)s.consumed, (unsigned long)s.durable, (unsigned long)s.error,
                 bmc_ota_staged(), bmc_ota_committed());
        return ESP_OK;
    }
    if (!strcmp(cmd, "ota-finish")) {
        portENTER_CRITICAL(&lock);
        bool finished = transaction_finished;
        if (!finished && (!completed_image || queued || assembled || ingress_busy || status.state != 4 || !requested)) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) finishing = true;
        portEXIT_CRITICAL(&lock);
        if (err == ESP_OK && !finished && bmc_ota_staged()) err = bmc_ota_commit();
        if (err == ESP_OK && !finished) err = persist(false);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&lock);
            requested = false;
            published = &idle;
            transaction_finished = true;
            portEXIT_CRITICAL(&lock);
        } else if (!bmc_ota_committed()) {
            portENTER_CRITICAL(&lock);
            finishing = false;
            portEXIT_CRITICAL(&lock);
        }
    } else if (!strcmp(cmd, "ota-reboot")) {
        err = transaction_finished && !pending ? bmc_ota_reboot() : ESP_ERR_INVALID_STATE;
    } else if (!strcmp(cmd, "ota-bmc") || !strcmp(cmd, "ota") || !strcmp(cmd, "ota-rescue") || !strcmp(cmd, "ota-abort")) {
        if (bmc_ota_committed()) return ESP_ERR_INVALID_STATE;
        bool local = !strcmp(cmd, "ota-bmc"), aborting = !strcmp(cmd, "ota-abort");
        portENTER_CRITICAL(&lock);
        bool unfinished_local = local_mode && requested && status.state != 4;
        portEXIT_CRITICAL(&lock);
        if (unfinished_local && !local && !aborting) return ESP_ERR_INVALID_STATE;
        if (local || aborting) err = bmc_ota_stage_abort();
        if (err == ESP_OK) err = spl_nand_hold();
        if (err == ESP_OK) err = persist(true);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&lock);
            requested = !aborting;
            local_mode = local;
            transaction_finished = false;
            finishing = false;
            status = (app_ota_status_t){.magic = APP_OTA_MAGIC, .stage = local ? APP_OTA_STAGE_BMC : 0};
            queued = false; assembled = total = target = 0; completed_image = completed_fit = false;
            portEXIT_CRITICAL(&lock);
            app_ota_spi_reset();
            if (requested && !local) {
                err = bmc_runtime_power_on();
                if (err == ESP_OK) err = spl_nand_boot_slot(CONFIG_BMC_SPL_SLOT);
            }
        }
    } else err = ESP_ERR_INVALID_ARG;
    snprintf(response, capacity, "%s: %s", cmd, esp_err_to_name(err));
    return err;
}

esp_err_t app_ota_command(const char *cmd, char *response, size_t capacity)
{
    if (!local_operation) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(local_operation, portMAX_DELAY);
    esp_err_t err = command_locked(cmd, response, capacity);
    xSemaphoreGive(local_operation);
    return err;
}
