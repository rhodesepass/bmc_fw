#include "ble_debug.h"
#include "app_ota.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEBUG_UUID(n) BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,n,0x00,0x40,0x6e)
#define COMMAND_MAX 244
#define PAYLOAD_MAX 244

static const ble_uuid128_t service_uuid = DEBUG_UUID(1);
static const ble_uuid128_t rx_uuid = DEBUG_UUID(2);
static const ble_uuid128_t tx_uuid = DEBUG_UUID(3);
static const ble_uuid128_t control_uuid = DEBUG_UUID(4);
static const ble_uuid128_t log_uuid = DEBUG_UUID(5);
static const ble_uuid128_t upload_uuid = DEBUG_UUID(6);
static const ble_uuid128_t ota_status_uuid = DEBUG_UUID(7);
static uint16_t tx_handle, log_handle;
static uint8_t own_addr_type;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE, mtu = 23;
static bool uart_notify, log_notify, command_busy;
static bool slow_advertising;
static uint16_t applied_adv_min, applied_adv_max;
static uint32_t dropped;
static char control_response[512] = "OK ready";
static ble_debug_callbacks_t callbacks;
static QueueHandle_t jobs;

typedef struct {
    bool control;
    uint16_t len;
    uint8_t data[PAYLOAD_MAX + 1];
} debug_job_t;

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields)) return;
    struct ble_hs_adv_fields scan = {0};
    scan.name = (const uint8_t *)"EPASS-BMC";
    scan.name_len = strlen((const char *)scan.name);
    scan.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&scan)) return;
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    portENTER_CRITICAL(&state_lock);
    bool slow = slow_advertising;
    portEXIT_CRITICAL(&state_lock);
    params.itvl_min = slow ? BLE_GAP_ADV_ITVL_MS(250) : BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = slow ? BLE_GAP_ADV_ITVL_MS(500) : BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    if (!ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL)) {
        portENTER_CRITICAL(&state_lock);
        applied_adv_min = params.itvl_min;
        applied_adv_max = params.itvl_max;
        portEXIT_CRITICAL(&state_lock);
    }
}

esp_err_t ble_debug_adv_command(const char *request, char *response, size_t capacity)
{
    bool change = strcmp(request, "ble-adv-status") != 0;
    bool slow = !strcmp(request, "ble-adv slow");
    if (change && !slow && strcmp(request, "ble-adv fast")) {
        snprintf(response, capacity, "use ble-adv fast|slow or ble-adv-status");
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&state_lock);
    if (change) slow_advertising = slow;
    slow = slow_advertising;
    uint16_t applied_min = applied_adv_min, applied_max = applied_adv_max;
    bool connected = connection != BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&state_lock);
    unsigned next_min = slow ? BLE_GAP_ADV_ITVL_MS(250) : BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    unsigned next_max = slow ? BLE_GAP_ADV_ITVL_MS(500) : BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    bool advertising = ble_gap_adv_active();
    snprintf(response, capacity,
             "mode=%s connected=%d advertising=%d active_min_us=%u active_max_us=%u next_min_us=%u next_max_us=%u pending=%d apply=next_advertise",
             slow ? "slow" : "fast", connected, advertising,
             advertising ? applied_min * 625u : 0, advertising ? applied_max * 625u : 0,
             next_min * 625u, next_max * 625u,
             applied_min != next_min || applied_max != next_max);
    return ESP_OK;
}

static void reset_state(void)
{
    portENTER_CRITICAL(&state_lock);
    connection = BLE_HS_CONN_HANDLE_NONE;
    uart_notify = log_notify = false;
    mtu = 23;
    portEXIT_CRITICAL(&state_lock);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            advertise();
        } else {
            portENTER_CRITICAL(&state_lock);
            connection = event->connect.conn_handle;
            portEXIT_CRITICAL(&state_lock);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        reset_state();
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&state_lock);
        mtu = event->mtu.value;
        portEXIT_CRITICAL(&state_lock);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        portENTER_CRITICAL(&state_lock);
        if (event->subscribe.attr_handle == tx_handle) uart_notify = event->subscribe.cur_notify;
        if (event->subscribe.attr_handle == log_handle) log_notify = event->subscribe.cur_notify;
        portEXIT_CRITICAL(&state_lock);
        break;
    default:
        break;
    }
    return 0;
}

static int access_characteristic(uint16_t conn, uint16_t attr,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    bool control = arg != NULL;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && control) {
        char response[sizeof(control_response)];
        portENTER_CRITICAL(&state_lock);
        memcpy(response, control_response, sizeof(response));
        portEXIT_CRITICAL(&state_lock);
        return os_mbuf_append(ctxt->om, response, strlen(response)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    debug_job_t job = {.control = control};
    size_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (!len || len > (control ? COMMAND_MAX : PAYLOAD_MAX)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, job.data, sizeof(job.data) - 1, &job.len)) return BLE_ATT_ERR_UNLIKELY;
    job.data[job.len] = 0;
    if (control) {
        for (size_t i = 0; i < len; ++i) {
            if (job.data[i] < 0x20 || job.data[i] > 0x7e) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        char busy[COMMAND_MAX + 6];
        int busy_len = snprintf(busy, sizeof(busy), "BUSY %s",
                                !strncmp((char *)job.data, "wifi-", 5) ? "wifi" : (char *)job.data);
        portENTER_CRITICAL(&state_lock);
        if (command_busy) {
            portEXIT_CRITICAL(&state_lock);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        command_busy = true;
        memcpy(control_response, busy, (size_t)busy_len + 1);
        portEXIT_CRITICAL(&state_lock);
    }
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        if (control) {
            portENTER_CRITICAL(&state_lock);
            command_busy = false;
            memcpy(control_response, "ERROR queue full", sizeof("ERROR queue full"));
            portEXIT_CRITICAL(&state_lock);
        }
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int access_ota(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    if (arg) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        app_ota_status_t status;
        app_ota_get_status(&status);
        return os_mbuf_append(ctxt->om, &status, sizeof(status)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint8_t data[PAYLOAD_MAX];
    uint16_t len;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &len)) return BLE_ATT_ERR_UNLIKELY;
    esp_err_t err = app_ota_upload(data, len);
    if (err == ESP_ERR_INVALID_STATE) return BLE_ATT_ERR_INSUFFICIENT_RES;
    return err == ESP_OK ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &rx_uuid.u, .access_cb = access_characteristic, .flags = BLE_GATT_CHR_F_WRITE},
            {.uuid = &tx_uuid.u, .access_cb = access_characteristic, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_handle},
            {.uuid = &control_uuid.u, .access_cb = access_characteristic, .arg = (void *)1, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE},
            {.uuid = &log_uuid.u, .access_cb = access_characteristic, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &log_handle},
            {.uuid = &upload_uuid.u, .access_cb = access_ota, .flags = BLE_GATT_CHR_F_WRITE},
            {.uuid = &ota_status_uuid.u, .access_cb = access_ota, .arg = (void *)1, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {0},
};

static void worker_task(void *arg)
{
    (void)arg;
    debug_job_t job;
    while (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
        if (!job.control) {
            esp_err_t rc = callbacks.uart_write(job.data, job.len);
            if (rc != ESP_OK) {
                portENTER_CRITICAL(&state_lock);
                ++dropped;
                portEXIT_CRITICAL(&state_lock);
            }
            continue;
        }
        char result[512] = {0};
        esp_err_t rc = callbacks.command((const char *)job.data, result + 6, sizeof(result) - 6);
        result[sizeof(result) - 1] = 0;
        size_t length = strlen(result + 6);
        if (rc == ESP_OK) {
            memmove(result + 3, result + 6, length + 1);
            memcpy(result, "OK ", 3);
            length += 3;
        } else {
            memcpy(result, "ERROR ", 6);
            length += 6;
        }
        portENTER_CRITICAL(&state_lock);
        memcpy(control_response, result, length + 1);
        command_busy = false;
        portEXIT_CRITICAL(&state_lock);
    }
    vTaskDelete(NULL);
}

static void on_sync(void)
{
    if (!ble_hs_util_ensure_addr(0) && !ble_hs_id_infer_auto(0, &own_addr_type)) advertise();
}

static void on_reset(int reason)
{
    (void)reason;
    reset_state();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_debug_init(const ble_debug_callbacks_t *cb)
{
    if (!cb || !cb->uart_write || !cb->command) return ESP_ERR_INVALID_ARG;
    if (jobs) return ESP_ERR_INVALID_STATE;
    jobs = xQueueCreate(8, sizeof(debug_job_t));
    if (!jobs) return ESP_ERR_NO_MEM;
    callbacks = *cb;
    esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) goto fail;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_svc_gap_device_name_set("EPASS-BMC") || ble_att_set_preferred_mtu(PAYLOAD_MAX + 3) ||
        ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
        rc = ESP_FAIL;
        nimble_port_deinit();
        goto fail;
    }
    if (xTaskCreate(worker_task, "ble_debug", 4096, NULL, 4, NULL) != pdPASS) {
        rc = ESP_ERR_NO_MEM;
        nimble_port_deinit();
        goto fail;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
fail:
    vQueueDelete(jobs);
    jobs = NULL;
    return rc;
}

static bool stop_started;
static esp_err_t stop_result = ESP_ERR_NOT_FINISHED;

static void stop_task(void *arg)
{
    (void)arg;
    int rc = nimble_port_stop();
    esp_err_t result = ESP_FAIL;
    if (!rc) {
        reset_state();
        result = nimble_port_deinit();
    }
    portENTER_CRITICAL(&state_lock);
    stop_result = result;
    portEXIT_CRITICAL(&state_lock);
    vTaskDelete(NULL);
}

esp_err_t ble_debug_stop(void)
{
    if (!jobs) return ESP_OK;
    /* NimBLE waits on its host task; keep the power manager's watchdog alive. */
    if (!stop_started) {
        if (xTaskCreate(stop_task, "ble_stop", 4096, NULL, 4, NULL) != pdPASS)
            return ESP_ERR_NO_MEM;
        stop_started = true;
    }
    portENTER_CRITICAL(&state_lock);
    esp_err_t result = stop_result;
    portEXIT_CRITICAL(&state_lock);
    return result;
}

static esp_err_t publish(const uint8_t *data, size_t len, bool log)
{
    if (!data && len) return ESP_ERR_INVALID_ARG;
    while (len) {
        portENTER_CRITICAL(&state_lock);
        uint16_t conn = connection;
        size_t chunk = mtu - 3;
        bool subscribed = log ? log_notify : uart_notify;
        portEXIT_CRITICAL(&state_lock);
        if (conn == BLE_HS_CONN_HANDLE_NONE || !subscribed) return ESP_ERR_INVALID_STATE;
        if (chunk > PAYLOAD_MAX) chunk = PAYLOAD_MAX;
        if (chunk > len) chunk = len;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, chunk);
        int rc = om ? ble_gatts_notify_custom(conn, log ? log_handle : tx_handle, om) : BLE_HS_ENOMEM;
        if (rc) {
            portENTER_CRITICAL(&state_lock);
            dropped += (len + chunk - 1) / chunk;
            portEXIT_CRITICAL(&state_lock);
            return ESP_FAIL;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

esp_err_t ble_debug_publish_uart(const uint8_t *data, size_t len) { return publish(data, len, false); }
esp_err_t ble_debug_publish_log(const uint8_t *data, size_t len) { return publish(data, len, true); }

#define STATE_GETTER(type, name, expression) \
    type name(void) { \
        portENTER_CRITICAL(&state_lock); \
        type value = (expression); \
        portEXIT_CRITICAL(&state_lock); \
        return value; \
    }
STATE_GETTER(bool, ble_debug_connected, connection != BLE_HS_CONN_HANDLE_NONE)
STATE_GETTER(bool, ble_debug_uart_subscribed, uart_notify)
STATE_GETTER(bool, ble_debug_log_subscribed, log_notify)
STATE_GETTER(uint16_t, ble_debug_mtu, mtu)
STATE_GETTER(uint32_t, ble_debug_dropped, dropped)
