#include "bmc_ota_stub.h"
#include "../main/bmc_ota.c"

esp_err_t bmc_debug_self_ota_begin(void) { if (busy) return ESP_ERR_INVALID_STATE; busy=true; return ESP_OK; }
void bmc_debug_self_ota_end(void) { unlock_calls++; busy=false; }
bool bmc_link_session_valid(uint32_t epoch) { assert(epoch==42); return ++session_calls < expire_on_call; }
esp_err_t bmc_wifi_apply(esp_err_t (*fn)(void *), void *arg) { return interlock_error ? ESP_FAIL : fn(arg); }

static uint8_t image[9000];
static char digest[65];
static httpd_req_t request;
static void reset(void)
{
    discard();
    assert(bmc_ota_init() == ESP_OK);
    begin_calls=write_calls=end_calls=abort_calls=set_boot_calls=restart_calls=0;
    unlock_calls=session_calls=0; expire_on_call=UINT32_MAX;
    written_bytes=hashed_bytes=0; interrupt_at=SIZE_MAX;
    end_error=boot_error=write_error=send_error=0;
    busy=interlock_error=false; now=100; restart_at=0;
    boot_slot=&slots[0];
    current_app=(esp_app_desc_t){.magic_word=ESP_APP_DESC_MAGIC_WORD, .project_name="epass_bmc", .version="test"};
    memset(image,0xa5,sizeof(image));
    esp_image_header_t header={.magic=ESP_IMAGE_HEADER_MAGIC, .chip_id=CONFIG_IDF_FIRMWARE_CHIP_ID};
    memcpy(image,&header,sizeof(header));
    memcpy(image+sizeof(header)+sizeof(esp_image_segment_header_t),&current_app,sizeof(current_app));
    for (unsigned i=0;i<32;i++) memcpy(digest+2*i,"12",2);
    digest[64]=0; expected_body=image;
    request=(httpd_req_t){.method=HTTP_POST, .content_len=sizeof(image), .body=image, .digest=digest};
}
static void failed(const char *message, unsigned aborts)
{
    assert(bmc_ota_http(&request,42)==ESP_OK);
    assert(strstr(request.response,message));
    assert(boot_slot==&slots[0] && abort_calls==aborts && !restart_at);
    assert(!busy && unlock_calls==1);
    bmc_ota_poll(); assert(!restart_calls);
}
int main(void)
{
    reset(); request.method=HTTP_GET;
    strcpy(current_app.version,"v\"\\\n");
    assert(bmc_ota_http(&request,42)==ESP_OK);
    assert(strstr(request.response,"\"running_partition\":\"ota_0\""));
    assert(strstr(request.response,"v\\\"\\\\\\u000a"));
    assert(!begin_calls && !busy);
    reset(); digest[0]=0;
    assert(bmc_ota_http(&request,42)==ESP_OK && strstr(request.response,"sha256_required"));
    assert(!begin_calls && !busy);
    reset(); digest[0]='z';
    assert(bmc_ota_http(&request,42)==ESP_OK && strstr(request.response,"sha256_invalid"));
    assert(!begin_calls && !busy);
    reset(); busy=true;
    assert(bmc_ota_http(&request,42)==ESP_OK && strstr(request.response,"busy"));
    assert(!begin_calls && !unlock_calls && busy);
    reset(); request.content_len=1;
    assert(bmc_ota_http(&request,42)==ESP_OK && strstr(request.response,"image_size"));
    assert(!begin_calls && !busy);
    reset();
    assert(bmc_ota_http(&request,42)==ESP_OK);
    assert(!strcmp(request.status,"200 OK") && strstr(request.response,"\"status\":\"staged\""));
    assert(begin_calls==1 && write_calls==4 && end_calls==1 && !abort_calls && !set_boot_calls);
    assert(written_bytes==sizeof(image) && hashed_bytes==sizeof(image));
    assert(boot_slot==&slots[0] && busy && !unlock_calls && !restart_at);
    assert(bmc_ota_staged() && !bmc_ota_committed());
    assert(bmc_ota_reboot() == ESP_ERR_INVALID_STATE);
    assert(bmc_ota_commit() == ESP_OK);
    assert(bmc_ota_commit() == ESP_OK && set_boot_calls == 1);
    assert(bmc_ota_committed() && boot_slot==&slots[1] && !restart_at);
    assert(bmc_ota_stage_abort() == ESP_ERR_INVALID_STATE);
    assert(bmc_ota_reboot() == ESP_OK);
    now=restart_at-1; bmc_ota_poll(); assert(!restart_calls);
    now++; bmc_ota_poll(); assert(restart_calls==1);
    assert(bmc_ota_http(&request,42)==ESP_OK);
    assert(strstr(request.response,"slot_unavailable") && begin_calls==1);

    reset(); interrupt_at=4500; failed("body_truncated",1); assert(!end_calls && !set_boot_calls);
    reset(); digest[0]='0'; failed("sha256_mismatch",1); assert(!end_calls && !set_boot_calls);
    reset(); image[12]^=1; failed("wrong_image",0); assert(!begin_calls && !set_boot_calls);
    reset(); image[sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+offsetof(esp_app_desc_t,project_name)]^=1;
    failed("wrong_image",0); assert(!begin_calls && !set_boot_calls);
    reset(); request.content_len=slots[1].size+1;
    assert(bmc_ota_http(&request,42)==ESP_OK && strstr(request.response,"image_size"));
    assert(!begin_calls && !busy && !set_boot_calls);
    reset(); expire_on_call=36; failed("expired",1); assert(!end_calls && !set_boot_calls);
    reset(); end_error=ESP_FAIL; failed("invalid_image",0); assert(end_calls==1 && !set_boot_calls);
    reset(); boot_error=ESP_FAIL;
    assert(bmc_ota_http(&request,42)==ESP_OK);
    assert(bmc_ota_commit()==ESP_FAIL && bmc_ota_staged() && !bmc_ota_committed());
    assert(bmc_ota_stage_abort()==ESP_OK && !bmc_ota_staged());
    reset(); write_error=ESP_FAIL; failed("ota_failed",1); assert(!end_calls && !set_boot_calls);
    reset(); send_error=ESP_FAIL;
    assert(bmc_ota_http(&request,42)==ESP_FAIL);
    assert(boot_slot==&slots[0] && !restart_at && busy && bmc_ota_staged());

    uint8_t hash[32]; memset(hash,0x12,sizeof(hash));
    reset();
    assert(bmc_ota_commit()==ESP_ERR_INVALID_STATE);
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_OK);
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_ERR_INVALID_STATE);
    for (size_t i=0; i<sizeof(image); ++i) {
        assert(bmc_ota_stage_write(image+i,1)==ESP_OK);
        if (i+1<sizeof(stage.prefix)) assert(!begin_calls);
    }
    assert(bmc_ota_stage_end()==ESP_OK && bmc_ota_staged() && !set_boot_calls && !busy);
    assert(bmc_ota_stage_end()==ESP_ERR_INVALID_STATE && bmc_ota_staged());
    assert(bmc_ota_stage_write(image,1)==ESP_ERR_INVALID_STATE && bmc_ota_staged());
    assert(bmc_ota_stage_abort()==ESP_OK && !bmc_ota_staged());
    reset();
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_OK);
    assert(bmc_ota_stage_write(image,300)==ESP_OK);
    assert(bmc_ota_stage_end()==ESP_ERR_INVALID_SIZE && abort_calls==1);
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_OK);
    assert(bmc_ota_stage_abort()==ESP_OK && abort_calls==1);
    reset();
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_OK);
    assert(bmc_ota_stage_write(image,sizeof(image)+1)==ESP_ERR_INVALID_SIZE && !begin_calls);
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_OK);
    assert(bmc_ota_stage_abort()==ESP_OK);
    reset();
    assert(bmc_ota_stage_begin(1,hash)==ESP_ERR_INVALID_SIZE);
    assert(bmc_ota_stage_begin(slots[1].size+1,hash)==ESP_ERR_INVALID_SIZE);
    assert(bmc_ota_stage_begin(sizeof(image),NULL)==ESP_ERR_INVALID_ARG);
    assert(bmc_ota_stage_begin(sizeof(image),hash)==ESP_OK);
    assert(bmc_ota_stage_write(image,300)==ESP_OK);
    assert(bmc_ota_stage_abort()==ESP_OK && abort_calls==1 && !bmc_ota_staged());
    assert(!busy && !unlock_calls);
    puts("ESP self OTA: stage, tiny chunks, stream faults, explicit commit/reboot and abort checks passed");
    return 0;
}
