#include "app_ota_wire.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#endif
_Static_assert(sizeof(app_ota_frame_t) == APP_OTA_SIZE, "wire size");
uint32_t app_ota_crc(const void *bytes, size_t len)
{
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(0, bytes, len);
#else
    const uint8_t *p = bytes;
    uint32_t crc = ~0u;
    while (len--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
#endif
}
bool app_ota_valid(const app_ota_frame_t *f)
{
    if (f->magic != APP_OTA_MAGIC || f->length > APP_OTA_PAYLOAD ||
        f->crc != app_ota_crc(f, APP_OTA_SIZE - 4)) return false;
    for (size_t i = f->length; i < APP_OTA_PAYLOAD; i++) if (f->payload[i]) return false;
    return true;
}
void app_ota_seal(app_ota_frame_t *f)
{
    f->magic = APP_OTA_MAGIC;
    f->crc = app_ota_crc(f, APP_OTA_SIZE - 4);
}
bool app_ota_upload_valid(const app_ota_frame_t *f, const app_ota_status_t *s,
                          uint32_t total, uint32_t target)
{
    if (!app_ota_valid(f) || !f->session || f->arg || f->status) return false;
    if (f->op == 10) {
        uint32_t size, dest;
        memcpy(&size, f->payload, 4); memcpy(&dest, f->payload + 4, 4);
        return (s->state == 0 || s->state == 4) && f->session != s->session && f->seq == 1 && !f->offset && f->length == 40 && size &&
               ((s->stage == APP_OTA_STAGE_BMC && dest == APP_OTA_TARGET_BMC && size >= 288 && size <= 0x180000) ||
                (s->stage == 1 && dest == 4 && size <= 4u * 1024 * 1024) ||
                (s->stage == 2 && ((dest == 1 && size <= 10u * 1024 * 1024) ||
                 (dest == 2 && size <= 28u * 1024 * 1024 && !(size % (128u * 1024))) ||
                 (dest == 3 && size <= 1024u * 1024) || (dest == 5 && size <= 32u * 1024 * 1024) ||
                 (dest == 6 && size <= 63488 && !(size % 4)))));
    }
    if (s->state != 3 || f->session != s->session || f->seq != s->acknowledged_seq + 1 ||
        f->offset != s->consumed || s->consumed > total || !target) return false;
    if (f->op == 11) return f->length && f->length <= total - s->consumed;
    return f->op == 12 && !f->length && f->offset == total;
}
