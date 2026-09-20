#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define APP_OTA_MAGIC 0x31555042u
#define APP_OTA_SIZE 2052u
#define APP_OTA_PAYLOAD 2016u
#define APP_OTA_TARGET_BMC 7u
#define APP_OTA_STAGE_BMC 3u
typedef struct {
    uint32_t magic, op, session, seq, offset, length, arg, status;
    uint8_t payload[APP_OTA_PAYLOAD];
    uint32_t crc;
} app_ota_frame_t;
typedef struct {
    uint32_t magic, session, acknowledged_seq, consumed, durable, state, error, stage;
} app_ota_status_t;
uint32_t app_ota_crc(const void *bytes, size_t len);
bool app_ota_valid(const app_ota_frame_t *f);
void app_ota_seal(app_ota_frame_t *f);
bool app_ota_upload_valid(const app_ota_frame_t *f, const app_ota_status_t *s,
                          uint32_t total, uint32_t target);
