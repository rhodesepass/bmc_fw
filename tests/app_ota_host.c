#include <assert.h>
#include "app_ota_stub.h"
#include "../main/app_ota.c"
#include "../main/app_ota_wire.c"
esp_err_t spl_nand_hold(void) { mock_hold++; return ESP_OK; }
esp_err_t spl_nand_boot_slot(unsigned s) { mock_slot = s; return ESP_OK; }
esp_err_t bmc_mainsys_set(bool enabled) { assert(enabled); return ESP_OK; }
esp_err_t bmc_runtime_power_on(void) { return ESP_OK; }
static bool staged_bmc, committed_bmc;
static unsigned bmc_received, bmc_total, bmc_commits, bmc_reboots;
static esp_err_t bmc_end_error;
esp_err_t bmc_ota_init(void) { return ESP_OK; }
esp_err_t bmc_ota_stage_begin(uint32_t total_bytes, const uint8_t hash[32])
{ (void)hash; bmc_total=total_bytes; bmc_received=0; staged_bmc=false; return ESP_OK; }
esp_err_t bmc_ota_stage_write(const uint8_t *data_bytes, size_t size_bytes)
{ (void)data_bytes; bmc_received+=size_bytes; return ESP_OK; }
esp_err_t bmc_ota_stage_end(void)
{ assert(bmc_received==bmc_total); if(bmc_end_error)return bmc_end_error; staged_bmc=true; return ESP_OK; }
esp_err_t bmc_ota_stage_abort(void)
{ if(committed_bmc)return ESP_ERR_INVALID_STATE; staged_bmc=false; return ESP_OK; }
bool bmc_ota_staged(void) { return staged_bmc; }
bool bmc_ota_committed(void) { return committed_bmc; }
esp_err_t bmc_ota_commit(void)
{
    if(!staged_bmc)return ESP_ERR_INVALID_STATE;
    app_ota_frame_t next={.op=10,.session=101,.seq=1,.length=40};
    uint32_t size=288,dest=7;
    memcpy(next.payload,&size,4);memcpy(next.payload+4,&dest,4);app_ota_seal(&next);
    uint8_t part[20]={0};memcpy(part+4,&next,16);
    assert(app_ota_upload(part,sizeof(part))==ESP_ERR_INVALID_STATE);
    assert(app_ota_submit_frame((const uint8_t *)&next,sizeof(next))==ESP_ERR_INVALID_STATE);
    assert(finishing && !queued && !assembled);
    if(!committed_bmc)bmc_commits++;
    committed_bmc=true;
    return ESP_OK;
}
esp_err_t bmc_ota_reboot(void)
{ if(!committed_bmc)return ESP_ERR_INVALID_STATE; bmc_reboots++; return ESP_OK; }
static void upload(app_ota_frame_t *f)
{
    app_ota_seal(f);
    for (uint32_t off = 0; off < sizeof(*f);) {
        uint8_t fragment[20];
        size_t n = sizeof(*f) - off;
        if (n > 16) n = 16;
        memcpy(fragment, &off, 4);
        memcpy(fragment + 4, (uint8_t *)f + off, n);
        assert(app_ota_upload(fragment, n + 4) == ESP_OK);
        off += n;
    }
}
static void ack(uint32_t seq, uint32_t consumed, uint32_t durable)
{
    app_ota_frame_t f = {.op=3, .session=42, .seq=seq, .offset=consumed, .arg=durable};
    app_ota_seal(&f); receive(&f, epoch);
}
static void enter_updater(void)
{
    app_ota_frame_t hello = {.op=1, .arg=2};
    app_ota_seal(&hello); receive(&hello, epoch);
    assert(status.stage == 0 && !protocol_mode && !completed_fit);
    hello.arg=1; app_ota_seal(&hello); receive(&hello, epoch);
    app_ota_frame_t f = {.op=10, .session=41, .seq=1, .length=40};
    uint32_t size=3, dest=4;
    memcpy(f.payload,&size,4); memcpy(f.payload+4,&dest,4);
    upload(&f);
    app_ota_frame_t reply = {.op=3, .session=41, .seq=1};
    app_ota_seal(&reply); receive(&reply, epoch);
    f=(app_ota_frame_t){.op=11, .session=41, .seq=2, .length=3};
    memcpy(f.payload,"fit",3); upload(&f);
    reply.seq=2; reply.offset=3; app_ota_seal(&reply); receive(&reply, epoch);
    hello.arg=2; app_ota_seal(&hello); receive(&hello, epoch);
    assert(status.stage == 1 && !completed_fit);
    f=(app_ota_frame_t){.op=12, .session=41, .seq=3, .offset=3};
    upload(&f);
    reply.seq=3; app_ota_seal(&reply); receive(&reply, epoch);
    assert(completed_fit && !completed_image);
    app_ota_seal(&hello); receive(&hello, epoch);
    assert(status.stage == 2);
    hello.arg=1; app_ota_seal(&hello); receive(&hello, epoch);
    assert(status.stage == 2);
}

static void stage_bmc_image(bool http)
{
    char response[256];
    assert(app_ota_command("ota-bmc", response, sizeof(response)) == ESP_OK);
    assert(status.stage==APP_OTA_STAGE_BMC && requested && pending && local_mode);
    assert(app_ota_command("ota-finish", response, sizeof(response)) != ESP_OK);
    assert(app_ota_command("ota-reboot", response, sizeof(response)) != ESP_OK);
    app_ota_frame_t f={.op=10,.session=99,.seq=1,.length=40};
    uint32_t size=288, dest=APP_OTA_TARGET_BMC;
    memcpy(f.payload,&size,4); memcpy(f.payload+4,&dest,4); app_ota_seal(&f);
    if(http) assert(app_ota_submit_frame((const uint8_t *)&f,sizeof(f))==ESP_OK);
    else upload(&f);
    assert(queued && status.state==2 && bmc_total!=288);
    assert(app_ota_spi_done((const uint8_t *)&f, APP_OTA_SIZE*8)==NULL);
    receive_one();
    assert(!queued && status.state==3 && status.acknowledged_seq==1 && bmc_total==288);
    assert(app_ota_command("ota", response, sizeof(response))!=ESP_OK);
    f=(app_ota_frame_t){.op=11,.session=99,.seq=2,.length=288};
    app_ota_seal(&f);
    if(http) assert(app_ota_submit_frame((const uint8_t *)&f,sizeof(f))==ESP_OK);
    else upload(&f);
    receive_one();
    assert(status.consumed==288 && !status.durable && bmc_received==288);
    if(http) assert(app_ota_submit_frame((const uint8_t *)&f,sizeof(f))==ESP_OK);
    else upload(&f);
    assert(!queue_ready && bmc_received==288);
    f=(app_ota_frame_t){.op=12,.session=99,.seq=3,.offset=288}; app_ota_seal(&f);
    if(http) assert(app_ota_submit_frame((const uint8_t *)&f,sizeof(f))==ESP_OK);
    else upload(&f);
    receive_one();
    assert(!bmc_commits && !bmc_reboots);
    if(bmc_end_error) assert(status.state==5 && !staged_bmc && !status.durable);
    else {
        assert(status.state==4 && status.durable==288 && staged_bmc && !committed_bmc);
        if(http) assert(app_ota_submit_frame((const uint8_t *)&f,sizeof(f))==ESP_OK);
        else upload(&f);
        assert(!queue_ready);
    }
}

int main(void)
{
    char msg[256];
    assert(app_ota_crc("123456789", 9) == 0xcbf43926);
    assert(app_ota_init() == ESP_OK);
    app_ota_frame_t hello = {.op=1,.arg=2};
    app_ota_seal(&hello); hello.crc ^= 1; receive(&hello, epoch);
    assert(!protocol_mode);
    app_ota_seal(&hello); receive(&hello, epoch);
    assert(protocol_mode && published->arg == 0);
    assert(app_ota_spi_done((const uint8_t *)&hello, 8) == NULL);
    assert(!protocol_mode);
    receive(&hello, epoch);
    assert(protocol_mode);
    uint32_t old_epoch = epoch;
    app_ota_spi_reset();
    receive(&hello, old_epoch); assert(!protocol_mode);
    assert(app_ota_command("ota", msg, sizeof(msg)) == ESP_OK);
    assert(mock_pending && mock_slot == CONFIG_BMC_SPL_SLOT && mock_hold);
    assert(app_ota_command("ota-finish", msg, sizeof(msg)) != ESP_OK);
    enter_updater();
    app_ota_frame_t begin = {.op=10,.session=42,.seq=1,.length=40};
    uint32_t size=3, dest=1;
    memcpy(begin.payload,&size,4); memcpy(begin.payload+4,&dest,4);
    upload(&begin);
    assert(status.state == 2 && status.acknowledged_seq == 0);
    uint8_t busy_fragment[5] = {0};
    assert(app_ota_upload(busy_fragment, sizeof(busy_fragment)) == ESP_ERR_INVALID_STATE);
    dma_active = published;
    const app_ota_frame_t *previous_dma = dma_active;
    ack(1,1,0); assert(queued);
    ack(1,0,0); assert(!queued && status.state == 3);
    ack(1,0,0); assert(status.state == 3);
    app_ota_frame_t data = {.op=11,.session=42,.seq=2,.length=3};
    memcpy(data.payload,"abc",3); upload(&data);
    assert(published != previous_dma);
    ack(2,3,0); assert(status.consumed == 3 && status.durable == 0);
    app_ota_frame_t end = {.op=12,.session=42,.seq=3,.offset=3};
    upload(&end); ack(3,3,0); assert(queued);
    ack(3,3,3); assert(status.state == 4 && mock_pending);
    assert(app_ota_command("ota-finish",msg,sizeof(msg)) == ESP_OK);
    assert(!mock_pending && !requested);
    assert(app_ota_command("ota-rescue",msg,sizeof(msg)) == ESP_OK);
    assert(mock_slot == CONFIG_BMC_SPL_SLOT && mock_pending);
    hello.arg=1; app_ota_seal(&hello); receive(&hello, epoch);
    app_ota_seal(&begin);
    assert(!app_ota_upload_valid(&begin,&status,0,0));
    dest=4; memcpy(begin.payload+4,&dest,4); upload(&begin); ack(1,0,0);
    upload(&data); ack(2,3,0); upload(&end); ack(3,3,0);
    assert(status.state == 4 && !completed_image);
    assert(app_ota_command("ota-finish",msg,sizeof(msg)) != ESP_OK);
    assert(app_ota_command("ota-abort",msg,sizeof(msg)) == ESP_OK);
    assert(mock_pending && !requested);
    app_ota_normal_boot(); assert(!protocol_mode && published->arg == 0);
    assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
    enter_updater();
    dest=1; memcpy(begin.payload+4,&dest,4); upload(&begin);
    app_ota_frame_t error = {.op=4,.session=42,.seq=9,.status=100};
    app_ota_seal(&error); receive(&error,epoch); assert(queued);
    error.seq=1; app_ota_seal(&error); receive(&error,epoch);
    assert(status.state == 5 && status.error == 100 && !queued);
    assert(app_ota_command("ota-finish",msg,sizeof(msg)) != ESP_OK);
    assert(mock_pending);
    assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
    enter_updater();
    error.session=42; error.seq=1; app_ota_seal(&error); receive(&error,epoch);
    assert(status.state == 4);
    error.session=41; error.seq=3; app_ota_seal(&error); receive(&error,epoch);
    assert(status.state == 5 && status.error == 100);
    assert(app_ota_command("ota-finish",msg,sizeof(msg)) != ESP_OK);
    assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
    enter_updater(); upload(&begin); ack(1,0,0);
    error.session=42; error.seq=0; app_ota_seal(&error); receive(&error,epoch);
    assert(status.state == 3);
    error.seq=1; app_ota_seal(&error); receive(&error,epoch);
    assert(status.state == 5 && status.error == 100 && mock_pending);
    incoming = NULL; pending = false;
    assert(app_ota_init() == ESP_OK && pending);
    app_ota_status_t bounds = {.stage=2};
    size=28u*1024*1024+128*1024; dest=2;
    memcpy(begin.payload,&size,4); memcpy(begin.payload+4,&dest,4); app_ota_seal(&begin);
    assert(!app_ota_upload_valid(&begin,&bounds,0,0));
    size=128*1024+1; memcpy(begin.payload,&size,4); app_ota_seal(&begin);
    assert(!app_ota_upload_valid(&begin,&bounds,0,0));
    size=128*1024; memcpy(begin.payload,&size,4); app_ota_seal(&begin);
    assert(app_ota_upload_valid(&begin,&bounds,0,0));
    bounds.session=42;
    assert(!app_ota_upload_valid(&begin,&bounds,0,0));
    assert(app_ota_command("ota", msg, sizeof(msg)) == ESP_OK);
    hello = (app_ota_frame_t){.op=1,.arg=1};
    app_ota_seal(&hello); receive(&hello,epoch);
    begin = (app_ota_frame_t){.op=10,.session=42,.seq=1,.length=40};
    size=3; dest=4;
    memcpy(begin.payload,&size,4); memcpy(begin.payload+4,&dest,4); app_ota_seal(&begin);
    assert(app_ota_submit_frame((uint8_t *)&begin, sizeof(begin)-1) == ESP_ERR_INVALID_SIZE);
    app_ota_frame_t corrupt=begin; corrupt.crc^=1;
    assert(app_ota_submit_frame((uint8_t *)&corrupt,sizeof(corrupt)) == ESP_ERR_INVALID_ARG);
    assert(upload_owner==UPLOAD_NONE && !ingress_busy && !queued);
    assert(app_ota_submit_frame((uint8_t *)&begin,sizeof(begin)) == ESP_OK);
    assert(upload_owner==UPLOAD_HTTP && queued);
    assert(app_ota_submit_frame((uint8_t *)&begin,sizeof(begin)) == ESP_OK);
    corrupt=begin; corrupt.payload[8]=1; app_ota_seal(&corrupt);
    assert(app_ota_submit_frame((uint8_t *)&corrupt,sizeof(corrupt)) == ESP_ERR_INVALID_ARG);
    assert(app_ota_upload(busy_fragment,sizeof(busy_fragment)) == ESP_ERR_INVALID_STATE);
    ack(1,0,0);
    assert(app_ota_submit_frame((uint8_t *)&begin,sizeof(begin)) == ESP_OK && !queued);
    data=(app_ota_frame_t){.op=11,.session=42,.seq=2,.length=3};
    memcpy(data.payload,"fit",3); app_ota_seal(&data);
    assert(app_ota_submit_frame((uint8_t *)&data,sizeof(data)) == ESP_OK);
    assert(app_ota_submit_frame((uint8_t *)&begin,sizeof(begin)) == ESP_ERR_INVALID_STATE);
    ack(2,3,0);
    end=(app_ota_frame_t){.op=12,.session=42,.seq=3,.offset=3}; app_ota_seal(&end);
    assert(app_ota_submit_frame((uint8_t *)&end,sizeof(end)) == ESP_OK);
    ack(3,3,0);
    assert(completed_fit && !queued);
    assert(app_ota_submit_frame((uint8_t *)&end,sizeof(end)) == ESP_OK);
    assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
    hello.arg=1; app_ota_seal(&hello); receive(&hello,epoch);
    uint8_t fragment[20]={0}; memcpy(fragment+4,&begin,16);
    assert(app_ota_upload(fragment,sizeof(fragment)) == ESP_OK);
    assert(app_ota_submit_frame((uint8_t *)&begin,sizeof(begin)) == ESP_ERR_INVALID_STATE);
    assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
    app_ota_seal(&hello); receive(&hello,epoch);
    assembly=begin; ingress_busy=true;
    app_ota_status_t snapshot=status;
    uint32_t stale=epoch;
    assert(app_ota_submit_frame((uint8_t *)&begin,sizeof(begin)) == ESP_ERR_INVALID_STATE);
    app_ota_spi_reset();
    assert(publish_assembly(&snapshot,stale,0,0,true) == ESP_ERR_INVALID_STATE);
    assert(!queued && !ingress_busy && last_http==NULL && upload_owner==UPLOAD_NONE);
    assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
    app_ota_spi_done((const uint8_t *)&hello,sizeof(hello)*8);
    assert(queue_ready && queue_size==sizeof(rx_item_t));
    rx_item_t queued_item; memcpy(&queued_item,queued_copy,sizeof(queued_item));
    assert(queued_item.generation==epoch);
    receive_one();
    assert(queue_wait==portMAX_DELAY && signal_count && status.stage==1);
    app_ota_spi_reset();
    memcpy(queued_copy,&queued_item,sizeof(queued_item));queue_ready=true;
    receive_one();assert(!protocol_mode);
    bounds=(app_ota_status_t){.stage=2};
    begin=(app_ota_frame_t){.op=10,.session=42,.seq=1,.length=40};
    dest=6;
    for (size=0; size<=63492; size++) {
        memcpy(begin.payload,&size,4); memcpy(begin.payload+4,&dest,4); app_ota_seal(&begin);
        assert(app_ota_upload_valid(&begin,&bounds,0,0) == (size && size<=63488 && !(size%4)));
    }
    size=4; dest=7;
    memcpy(begin.payload,&size,4); memcpy(begin.payload+4,&dest,4); app_ota_seal(&begin);
    assert(!app_ota_upload_valid(&begin,&bounds,0,0));
    dest=6; memcpy(begin.payload+4,&dest,4); app_ota_seal(&begin);
    bounds.stage=1; assert(!app_ota_upload_valid(&begin,&bounds,0,0));
    for (unsigned fail=0; fail<2; fail++) {
        assert(app_ota_command("ota",msg,sizeof(msg)) == ESP_OK);
        enter_updater(); upload(&begin); ack(1,0,0);
        data=(app_ota_frame_t){.op=11,.session=42,.seq=2,.length=4};
        memcpy(data.payload,"CH32",4); upload(&data);
        ack(2,4,4); assert(queued && !status.durable);
        ack(2,4,0); assert(!queued && status.consumed==4 && !status.durable);
        assert(app_ota_command("ota-finish",msg,sizeof(msg)) != ESP_OK);
        end=(app_ota_frame_t){.op=12,.session=42,.seq=3,.offset=4}; upload(&end);
        ack(3,4,0); assert(queued && !completed_image);
        if (fail) {
            error=(app_ota_frame_t){.op=4,.session=42,.seq=3,.status=100};
            app_ota_seal(&error); receive(&error,epoch);
            assert(status.state==5 && !status.durable && !completed_image);
            assert(app_ota_command("ota-finish",msg,sizeof(msg)) != ESP_OK && mock_pending);
        } else {
            ack(3,4,4); assert(status.state==4 && status.durable==4 && completed_image);
            assert(app_ota_command("ota-finish",msg,sizeof(msg)) == ESP_OK && !mock_pending);
        }
    }
    for(unsigned use_http=0;use_http<2;use_http++) {
        staged_bmc=committed_bmc=false; bmc_total=bmc_commits=bmc_reboots=0;
        stage_bmc_image(use_http);
        if(use_http) {
            nvs_fail_commit=true;
            assert(app_ota_command("ota-finish",msg,sizeof(msg))!=ESP_OK);
            assert(committed_bmc && finishing && pending && !transaction_finished);
            assert(app_ota_command("ota-reboot",msg,sizeof(msg))!=ESP_OK);
            nvs_fail_commit=false;
        }
        assert(app_ota_command("ota-finish",msg,sizeof(msg))==ESP_OK);
        assert(!pending && bmc_commits==1 && !bmc_reboots && transaction_finished);
        assert(app_ota_command("ota-finish",msg,sizeof(msg))==ESP_OK && bmc_commits==1);
        assert(app_ota_command("ota-abort",msg,sizeof(msg))!=ESP_OK);
        assert(app_ota_command("ota",msg,sizeof(msg))!=ESP_OK);
        assert(app_ota_command("ota-reboot",msg,sizeof(msg))==ESP_OK && bmc_reboots==1);
    }
    for(unsigned fail=0;fail<2;fail++) {
        staged_bmc=committed_bmc=false; bmc_total=bmc_commits=bmc_reboots=0;
        stage_bmc_image(false);
        assert(app_ota_command("ota",msg,sizeof(msg))==ESP_OK && staged_bmc);
        enter_updater();
        begin=(app_ota_frame_t){.op=10,.session=42,.seq=1,.length=40};
        size=3;dest=1;memcpy(begin.payload,&size,4);memcpy(begin.payload+4,&dest,4);
        upload(&begin);ack(1,0,0);
        data=(app_ota_frame_t){.op=11,.session=42,.seq=2,.length=3};upload(&data);ack(2,3,0);
        end=(app_ota_frame_t){.op=12,.session=42,.seq=3,.offset=3};upload(&end);ack(3,3,3);
        begin.session=43;size=4;dest=6;memcpy(begin.payload,&size,4);memcpy(begin.payload+4,&dest,4);
        upload(&begin);
        app_ota_frame_t reply={.op=3,.session=43,.seq=1};app_ota_seal(&reply);receive(&reply,epoch);
        data=(app_ota_frame_t){.op=11,.session=43,.seq=2,.length=4};upload(&data);
        reply.seq=2;reply.offset=4;app_ota_seal(&reply);receive(&reply,epoch);
        end=(app_ota_frame_t){.op=12,.session=43,.seq=3,.offset=4};upload(&end);
        reply.seq=3;reply.arg=4;
        if(fail){reply.op=4;reply.status=100;}
        app_ota_seal(&reply);receive(&reply,epoch);
        if(fail) {
            assert(app_ota_command("ota-finish",msg,sizeof(msg))!=ESP_OK);
            assert(app_ota_command("ota-reboot",msg,sizeof(msg))!=ESP_OK);
            assert(!bmc_commits && pending && staged_bmc);
            assert(app_ota_command("ota-abort",msg,sizeof(msg))==ESP_OK && !staged_bmc);
        } else {
            assert(app_ota_command("ota-finish",msg,sizeof(msg))==ESP_OK);
            assert(bmc_commits==1 && !bmc_reboots && !pending);
        }
    }
    staged_bmc=committed_bmc=false; bmc_total=bmc_commits=bmc_reboots=0;bmc_end_error=ESP_ERR_INVALID_ARG;
    stage_bmc_image(false);
    assert(app_ota_command("ota-finish",msg,sizeof(msg))!=ESP_OK && pending);
    assert(app_ota_command("ota",msg,sizeof(msg))!=ESP_OK);
    assert(app_ota_command("ota-abort",msg,sizeof(msg))==ESP_OK);
    puts("BMC local BLE/HTTP frames, duplicate ACKs, staging, mixed transactions and explicit reboot passed");
    puts("touch size/alignment, SPL rejection, DATA durability and END success/failure passed");
    puts("HTTP full-frame credit, exact retry, ingress ownership and reset generation passed");
    puts("CRC, fragmented credit, ACK bounds, durable END, pending, rescue and abort passed");
}
