#include "bmc_link_stub.h"
#define malloc tracked_malloc
#define free tracked_free
#include "../main/bmc_link.c"
#undef malloc
#undef free

const uint8_t html_start[] = "html";
const uint8_t html_end[] = "";
const uint8_t js_start[] = "js";
const uint8_t js_end[] = "";

static unsigned bmc_ota_calls;
esp_err_t bmc_ota_http(httpd_req_t *req, uint32_t epoch)
{
    (void)req;
    assert(bmc_link_session_valid(epoch));
    ++bmc_ota_calls;
    return ESP_OK;
}

bool bmc_mainsys_enabled(void) { return power_enabled; }
esp_err_t bmc_wifi_stop(void) { bmc_link_invalidate(); return ESP_OK; }
esp_err_t bmc_wifi_apply(esp_err_t (*apply)(void *),void *arg) { return power_enabled ? apply(arg) : ESP_ERR_INVALID_STATE; }
esp_err_t bmc_debug_link_command(const char *command,char *response,size_t capacity,uint32_t epoch)
{
    if (expire_at_control) bmc_link_invalidate();
    if (!bmc_link_session_valid(epoch)) return ESP_ERR_INVALID_STATE;
    ++control_calls;
    snprintf(response,capacity,"%s: ESP_OK",command);
    return ESP_OK;
}
esp_err_t app_ota_submit_frame(const uint8_t *data,size_t length)
{
    assert(length == APP_OTA_SIZE);
    ++submit_calls;
    if (submit_error) return submit_error;
    memcpy(submitted,data,length);
    const app_ota_frame_t *frame=(const app_ota_frame_t *)data;
    if(reject_seq && frame->seq==reject_seq)return ESP_ERR_INVALID_ARG;
    bool ack=ack_enabled && (!ack_stop_seq || frame->seq<ack_stop_seq);
    uint32_t old_ack=fake_status.session==frame->session ? fake_status.acknowledged_seq : 0;
    fake_status=(app_ota_status_t){.magic=APP_OTA_MAGIC,.session=frame->session,
        .acknowledged_seq=ack ? frame->seq : old_ack,.state=ack ? 3 : 2,.error=fake_error};
    return ESP_OK;
}
void app_ota_get_status(app_ota_status_t *out) { *out=fake_status; }
void app_ota_wait_status(uint32_t timeout_ms) { assert(timeout_ms==100); clock_ticks+=timeout_ms; }

static void ap_ready(char *auth)
{
    char response[512];
    assert(bmc_link_command("wifi-ap",response,sizeof(response))==ESP_OK);
    assert(!ready && active);
    fake_ip=1;
    event(NULL,WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
    assert(ready);
    snprintf(auth,40,"Bearer %s",token);
}
static httpd_req_t request(const char *uri,const char *auth,const void *body,size_t length)
{
    return (httpd_req_t){.uri=uri,.auth=auth,.body=body,.content_len=length};
}
int main(void)
{
    char response[512],auth[40],old_auth[40];
    assert(bmc_link_command("wifi-status",response,sizeof(response))==ESP_OK);
    assert(strstr(response,"\"state\":\"off\""));
    assert(strstr(response,"\"heap_free\":65536"));
    assert(strstr(response,"\"heap_largest\":32768"));
    assert(bmc_link_command("wifi-stop",response,sizeof(response))==ESP_OK);
    assert(!init_calls && !netif_init_calls);
    assert(bmc_link_command("wifi-sta 6550617373 313233",response,sizeof(response))==ESP_ERR_INVALID_ARG);
    assert(!init_calls && !netif_init_calls);
    power_enabled=false;
    assert(bmc_link_command("wifi-ap",response,sizeof(response))==ESP_ERR_INVALID_STATE);
    assert(!init_calls);
    power_enabled=true;
    init_error=ESP_ERR_NO_MEM;
    assert(bmc_link_command("wifi-ap",response,sizeof(response))==ESP_ERR_NO_MEM);
    assert(netif_created==2 && netif_destroyed==2);
    init_error=0;http_error=ESP_ERR_NO_MEM;
    assert(bmc_link_command("wifi-ap",response,sizeof(response))==ESP_ERR_NO_MEM);
    assert(netif_created==4 && netif_destroyed==4 && netif_init_calls==1);
    http_error=0;
    assert(bmc_link_command("wifi-ap",response,sizeof(response))==ESP_OK);
    assert(active && !ready && !strstr(response,"http://"));
    assert(session_open && session_open(server,42)==ESP_OK && socket_option_calls==1);
    socket_option_error=-1;
    assert(session_open(server,42)==ESP_FAIL && socket_option_calls==2);
    socket_option_error=0;
    fake_ip=0;event(NULL,WIFI_EVENT,WIFI_EVENT_AP_START,NULL);assert(!ready);
    fake_ip=1;event(NULL,WIFI_EVENT,WIFI_EVENT_AP_START,NULL);assert(ready);
    snprintf(auth,sizeof(auth),"Bearer %s",token);
    httpd_req_t r=request("/v1/control",NULL,"ota",3);api(&r);
    assert(strstr(r.status,"401") && !control_calls);
    r=request("/v1/bmc/firmware",NULL,NULL,0);api(&r);
    assert(strstr(r.status,"401") && !bmc_ota_calls);
    r=request("/v1/bmc/firmware",auth,NULL,0);api(&r);
    assert(bmc_ota_calls==1);
    r=request("/v1/control","Bearer 00000000000000000000000000000000","ota",3);api(&r);
    assert(strstr(r.status,"401") && !control_calls);
    r=request("/v1/control",auth,"ota",3);api(&r);
    assert(control_calls==1 && !strncmp(r.response,"OK ",3));
    r=request("/v1/control",auth,"wifi-stop",9);api(&r);
    assert(strstr(r.status,"400") && control_calls==1);
    r=request("/v1/control",auth,"ota\0boot",8);api(&r);
    assert(strstr(r.status,"400") && control_calls==1);
    r=request("/v1/channels/assets",auth,"x",1);api(&r);
    assert(strstr(r.status,"501"));
    r=request("/v1/capabilities",auth,NULL,0);api(&r);
    assert(strstr(r.response,"\"assets\":false"));
    app_ota_frame_t frame={.magic=APP_OTA_MAGIC,.session=17,.seq=3};
    r=request("/v1/channels/ota",auth,&frame,sizeof(frame));api(&r);
    assert(r.response_len==32 && submit_calls==1 && !memcmp(submitted,&frame,sizeof(frame)));
    r=request("/v1/channels/ota",auth,&frame,sizeof(frame));api(&r);
    assert(r.response_len==32 && submit_calls==2);
    submit_error=ESP_ERR_INVALID_ARG;
    r=request("/v1/channels/ota",auth,&frame,sizeof(frame));api(&r);
    assert(strstr(r.status,"409"));submit_error=0;
    fake_error=1;r=request("/v1/channels/ota",auth,&frame,sizeof(frame));api(&r);
    assert(strstr(r.status,"502"));fake_error=0;
    ack_enabled=false;r=request("/v1/channels/ota",auth,&frame,sizeof(frame));api(&r);
    assert(strstr(r.status,"504"));ack_enabled=true;
    app_ota_frame_t batch[16];
    for(unsigned i=0;i<16;++i)batch[i]=(app_ota_frame_t){.magic=APP_OTA_MAGIC,.session=42,.seq=i+1};
    r=request("/v1/channels/ota",auth,batch,sizeof(batch));r.chunk_limit=127;api(&r);
    assert(r.response_len==32 && fake_status.acknowledged_seq==16 && max_allocation==APP_OTA_SIZE && !live_allocations);
    r=request("/v1/channels/ota",auth,batch,APP_OTA_SIZE+1);api(&r);assert(strstr(r.status,"413"));
    r=request("/v1/channels/ota",auth,batch,17*APP_OTA_SIZE);api(&r);assert(strstr(r.status,"413"));
    r=request("/v1/channels/ota",auth,batch,0);api(&r);assert(strstr(r.status,"413"));
    batch[1].session=43;
    r=request("/v1/channels/ota",auth,batch,3*APP_OTA_SIZE);api(&r);
    assert(strstr(r.status,"409") && fake_status.acknowledged_seq==1);batch[1].session=42;
    batch[1].seq=3;
    r=request("/v1/channels/ota",auth,batch,3*APP_OTA_SIZE);api(&r);
    assert(strstr(r.status,"409") && fake_status.acknowledged_seq==1);batch[1].seq=2;
    r=request("/v1/channels/ota",auth,batch,3*APP_OTA_SIZE);r.cutoff=APP_OTA_SIZE+99;api(&r);
    assert(strstr(r.status,"400") && fake_status.acknowledged_seq==1 && !live_allocations);
    reject_seq=2;r=request("/v1/channels/ota",auth,batch,3*APP_OTA_SIZE);api(&r);
    assert(strstr(r.status,"409") && fake_status.acknowledged_seq==1);reject_seq=0;
    ack_stop_seq=2;r=request("/v1/channels/ota",auth,batch,3*APP_OTA_SIZE);api(&r);
    assert(strstr(r.status,"504") && fake_status.acknowledged_seq==1 && r.offset==2*APP_OTA_SIZE);ack_stop_seq=0;
    r=request("/v1/status",auth,NULL,0);api(&r);
    app_ota_status_t acknowledged;memcpy(&acknowledged,r.response,sizeof(acknowledged));assert(acknowledged.acknowledged_seq==1);
    r=request("/v1/channels/ota",auth,batch+1,2*APP_OTA_SIZE);api(&r);
    assert(r.response_len==32 && fake_status.acknowledged_seq==3 && !live_allocations);
    r=request("/v1/channels/ota",auth,batch,3*APP_OTA_SIZE);r.expire_after=2*APP_OTA_SIZE;api(&r);
    assert(strstr(r.status,"401") && fake_status.acknowledged_seq==1);ap_ready(auth);
    static uint8_t benchmark_data[256*1024];
    for(unsigned i=0;i<sizeof(benchmark_data);++i)benchmark_data[i]=(uint8_t)i;
    unsigned unchanged_submits=submit_calls;
    r=request("/v1/benchmark",NULL,benchmark_data,9);api(&r);assert(strstr(r.status,"401"));
    r=request("/v1/benchmark",auth,"123456789",9);r.chunk_limit=2;api(&r);
    assert(!strcmp(r.response,"{\"bytes\":9,\"crc32\":3421780262}"));
    r=request("/v1/benchmark",auth,benchmark_data,sizeof(benchmark_data));r.chunk_limit=397;api(&r);
    assert(!strcmp(r.response,"{\"bytes\":262144,\"crc32\":3348152310}"));
    r=request("/v1/benchmark",auth,benchmark_data,0);api(&r);assert(strstr(r.status,"413"));
    r=request("/v1/benchmark",auth,benchmark_data,sizeof(benchmark_data)+1);api(&r);assert(strstr(r.status,"413"));
    r=request("/v1/benchmark",auth,benchmark_data,4096);r.cutoff=1100;api(&r);assert(strstr(r.status,"400"));
    r=request("/v1/benchmark",auth,benchmark_data,4096);r.expire_after=1024;api(&r);assert(strstr(r.status,"401"));
    assert(submit_calls==unchanged_submits && !live_allocations);ap_ready(auth);
    unsigned before=submit_calls;
    r=request("/v1/channels/ota",auth,&frame,sizeof(frame));r.expire_on_recv=1;api(&r);
    assert(strstr(r.status,"401") && submit_calls==before);
    ap_ready(auth);
    r=request("/v1/control",auth,"ota",3);r.expire_on_recv=1;api(&r);
    assert(strstr(r.status,"401") && control_calls==1);
    ap_ready(auth);expire_at_control=true;
    r=request("/v1/control",auth,"ota",3);api(&r);
    assert(strstr(r.status,"409") && control_calls==1);expire_at_control=false;
    ap_ready(auth);memcpy(old_auth,auth,sizeof(auth));
    assert(bmc_link_command("wifi-stop",response,sizeof(response))==ESP_OK);
    r=request("/v1/status",old_auth,NULL,0);api(&r);assert(strstr(r.status,"401"));
    ap_ready(auth);assert(strcmp(auth,old_auth));
    r=request("/v1/status",old_auth,NULL,0);api(&r);assert(strstr(r.status,"401"));
    assert(bmc_link_command("wifi-sta 6550617373 -",response,sizeof(response))==ESP_OK);
    assert(active && !ready && !ap_mode);
    event(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);assert(connect_calls==1 && !ready);
    ip_event_got_ip_t got={.ip_info.ip.addr=0};
    event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&got);assert(!ready);
    got.ip_info.ip.addr=1;
    event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&got);assert(ready);
    event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);assert(!ready);
    bmc_link_invalidate();event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&got);assert(!ready);
    memset(ssid, 1, 32); ssid[32] = 0;
    active = ready = true; ap_mode = true;
    strcpy(ip, "255.255.255.255");
    memset(token, 'f', 32); token[32] = 0;
    memset(ap_password, 'f', 16); ap_password[16] = 0;
    last_error = -2147483647;
    snprintf(auth,sizeof(auth),"Bearer %s",token);
    for(unsigned i=0;i<2;i++) {
        const char *command=i ? "ota-reboot" : "ota-bmc";
        unsigned old_calls=control_calls;
        r=request("/v1/control",auth,command,strlen(command));api(&r);
        assert(control_calls==old_calls+1 && !strncmp(r.response,"OK ",3));
    }
    status_json(response, 500);
    assert(strlen(response) < 499 && response[strlen(response)-1] == '}');
    puts("actual HTTP handlers, Wi-Fi events, lazy init, cleanup and token lifecycle passed");
    return 0;
}
