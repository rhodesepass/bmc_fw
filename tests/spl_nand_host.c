#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_idf_stub.h"

static uint8_t flash_data[512 * 1024];
static const esp_partition_t partition = { sizeof(flash_data) };
static spi_slave_interface_config_t callbacks;
static spi_slave_transaction_t *active;
static int reset_level = -1;
static unsigned reset_releases;
static unsigned queued, notifications, task_count, map_count;
static bool run_cache = true;
static int writer_mutex;
static bool invalidate_during_copy;
static const uint8_t *(*fast_done)(size_t, void *);
static void *copy_with_interrupt(void *dst, const void *src, size_t length);
#define memcpy copy_with_interrupt

#include "../main/spl_nand.c"
#undef memcpy
static void *copy_with_interrupt(void *dst, const void *src, size_t length)
{
    void *result = memcpy(dst, src, length);
    if (invalidate_during_copy && length && (uintptr_t)dst >= (uintptr_t)cache_frames &&
        (uintptr_t)dst < (uintptr_t)(cache_frames + CACHE_COUNT)) {
        cache_enabled = false;
        ++cache_epoch;
        invalidate_during_copy = false;
    }
    return result;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &writer_mutex; }
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout) { (void)timeout; assert(mutex && !*mutex); *mutex=1; return 1; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex && *mutex); *mutex=0; return 1; }
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle)
{
    (void)fn;(void)arg;assert(!strcmp(name,"spl_cache") && stack == 2048 && priority == 24);
    ++task_count;*handle=(void *)1;return pdPASS;
}
unsigned ulTaskNotifyTake(int clear,unsigned timeout) { (void)clear;(void)timeout;return 1; }
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *wake) { assert(task); ++notifications; *wake=pdTRUE; }
esp_err_t esp_partition_mmap(const esp_partition_t *part,size_t offset,size_t size,int memory,const void **out,esp_partition_mmap_handle_t *handle)
{
    assert(part == &partition && offset == 0 && size == 2*SLOT_SIZE && memory == ESP_PARTITION_MMAP_DATA);
    ++map_count;*out=flash_data;*handle=1;return ESP_OK;
}
#if CONFIG_BMC_FAST_SPI_SLAVE
esp_err_t fast_spi_slave_init(uint8_t *rx, const uint8_t *(*done)(size_t,void *),void *ctx)
{ assert(rx == rx_frame && !ctx); fast_done=done; return ESP_OK; }
esp_err_t fast_spi_slave_arm(const uint8_t *tx)
{ assert(reset_level==0); transaction.tx_buffer=tx; active=&transaction; armed=true; return ESP_OK; }
esp_err_t fast_spi_slave_pause(void)
{ assert(reset_level==0); active=NULL; return ESP_OK; }
#endif
void app_ota_spi_reset(void) {}
void bmc_runtime_reset(void) {}
void bmc_runtime_boot_seen(void) {}
const uint8_t *bmc_runtime_spi_done(const uint8_t *rx, size_t bits) { (void)rx; (void)bits; return NULL; }
const uint8_t *app_ota_spi_done(const uint8_t *rx, size_t bits) { (void)rx; (void)bits; return NULL; }

int gpio_set_level(gpio_num_t pin, int level)
{
    assert(pin == BMC_PIN_APP_RESET);
    if (level) {
        assert(active && armed);
        reset_releases++;
    }
    reset_level = level;
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(reset_level == 0);
    assert(config->mode == GPIO_MODE_OUTPUT_OD);
    return ESP_OK;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == 0x40);
    assert(strcmp(name, "d1s_spl") == 0);
    return &partition;
}

esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *out, size_t length)
{
    assert(part == &partition && offset + length <= sizeof(flash_data));
    memcpy(out, flash_data + offset, length);
    return ESP_OK;
}

void *heap_caps_malloc(size_t length, int flags)
{
    assert(flags == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    assert(length <= 4096);
    return malloc(length);
}

void heap_caps_free(void *ptr) { free(ptr); }

esp_err_t spi_slave_initialize(int host, const spi_bus_config_t *bus,
                               const spi_slave_interface_config_t *slave, int dma)
{
    assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    assert(bus->mosi_io_num == BMC_PIN_SPI_MOSI && bus->miso_io_num == BMC_PIN_SPI_MISO);
    assert(bus->sclk_io_num == BMC_PIN_SPI_SCLK && slave->spics_io_num == BMC_PIN_SPI_CS);
    assert(slave->mode == 0 && slave->queue_size == 1);
    callbacks = *slave;
    return ESP_OK;
}

esp_err_t spi_slave_queue_trans_isr(int host, spi_slave_transaction_t *trans)
{
    assert(host == SPI2_HOST);
    active = trans;
    queued++;
    callbacks.post_setup_cb(trans);
    return ESP_OK;
}

esp_err_t spi_slave_queue_trans(int host, spi_slave_transaction_t *trans, int timeout)
{
    (void)timeout;
    return spi_slave_queue_trans_isr(host, trans);
}

esp_err_t spi_slave_queue_reset(int host)
{
    assert(host == SPI2_HOST && reset_level == 0);
    active = NULL;
    return ESP_OK;
}

esp_err_t spi_slave_free(int host) { (void)host; return ESP_OK; }
void vTaskDelay(int ticks) { (void)ticks; }

static void put_le32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = value >> (8 * i);
}

static void exchange(const uint8_t *command, size_t length, uint8_t *response)
{
    assert(active && length * 8 <= active->length);
    if (response)
        memcpy(response, active->tx_buffer, length);
    /* Model GDMA dropping an incomplete receive word. */
    memcpy(active->rx_buffer, command, length & ~(size_t)3);
    active->trans_len = length * 8;
    unsigned before = queued;
#if CONFIG_BMC_FAST_SPI_SLAVE
    active->tx_buffer=fast_done(length*8,NULL);
    ++queued;
#else
    (void)fast_done;
    callbacks.post_trans_cb(active);
#endif
    assert(queued == before + 1);
    if (run_cache && notifications) { notifications=0; prefetch_pages(); }
}

static void check_cache(const uint8_t *expected)
{
    uint8_t command[4 + 2048] = { 0x0b, 0, 0, 0 };
    uint8_t response[sizeof(command)];
    exchange(command, sizeof(command), response);
    for (unsigned i = 0; i < 2048; i++)
        assert(response[4 + i] == (expected ? expected[i] : 0xff));
}

int main(int argc, char **argv)
{
    memset(flash_data, 0xff, sizeof(flash_data));
    for (unsigned i = 0; i < 128 * 1024; i++)
        flash_data[i] = (i / 2048 * 7 + i % 251) & 0xff;
    memcpy(flash_data + 4, "eGON.BT0", 8);
    unsigned image_size = argc>1 && !strcmp(argv[1],"52-pages") ? 104*1024 : 128*1024;
    put_le32(flash_data + 16, image_size);
    put_le32(flash_data + 12, 0x5f0a6c39);
    uint32_t checksum = 0;
    for (unsigned i = 0; i < image_size; i += 4)
        checksum += get_le32(flash_data + i);
    put_le32(flash_data + 12, checksum);

    if (argc > 1 && strcmp(argv[1], "bad-size") == 0) {
        const unsigned lengths[]={0,31,1023,1025,SLOT_SIZE+1024};
        for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
            put_le32(flash_data+16,lengths[i]);
            assert(load_frames(0)==ESP_ERR_INVALID_SIZE);
            assert(!cache_enabled && image_length==0);
        }
        put_le32(flash_data+16,1024);put_le32(flash_data+12,CHECKSUM_STAMP);
        uint32_t short_sum=0;
        for(unsigned i=0;i<1024;i+=4) short_sum+=get_le32(flash_data+i);
        put_le32(flash_data+12,short_sum);
        assert(load_frames(0)==ESP_OK && cache_tags[0]==0 && cache_tags[1]==INVALID_PAGE);
        assert(!memcmp(cache_frames[0]+HEADER_SIZE,flash_data,1024));
        for(unsigned i=1024;i<PAGE_SIZE;++i) assert(cache_frames[0][HEADER_SIZE+i]==0xff);
        puts("invalid image boundaries rejected; 1KiB image padded safely"); return 0;
    }
    if (argc > 1 && strcmp(argv[1], "52-pages") == 0) {
        assert(spl_nand_start()==ESP_OK);
        for(unsigned n=0;n<53;++n) {
            unsigned row=n ? n-1 : 0;
            uint8_t read[]={0x13,0,0,row};exchange(read,sizeof(read),NULL);
            check_cache(flash_data+row*PAGE_SIZE);
        }
        assert(stats.page_reads==53 && stats.cache_reads==53 && stats.cache_misses==0);
        assert(spl_nand_boot(false)==ESP_OK);
        assert(cache_tags[0]==0 && cache_tags[1]==1 && cache_tags[2]==2);
        assert(task_count==1 && map_count==1);
        puts("53 reads of 52-page SPL with repeat page zero passed");return 0;
    }
    if (argc > 1 && strcmp(argv[1], "bad-checksum") == 0) {
        flash_data[300] ^= 1;
        assert(spl_nand_start() == ESP_ERR_INVALID_CRC);
        assert(reset_level == 0 && reset_releases == 0 && active == NULL);
        puts("bad checksum keeps D1s reset asserted");
        assert(spl_nand_boot(true) == ESP_OK);
        uint8_t row0[] = {0x13, 0, 0, 0};
        exchange(row0, sizeof(row0), NULL);
        uint8_t expected[2048];
        memset(expected, 0xff, sizeof(expected));
        memcpy(expected, fel_spl_image, sizeof(fel_spl_image));
        check_cache(expected);
        assert(reset_releases == 1 && spl_nand_is_fel());
        return 0;
    }

    assert(spl_nand_start() == ESP_OK);
    assert(spl_nand_boot_slot(2) == ESP_ERR_INVALID_ARG);
    assert(spl_nand_boot_slot(3) == ESP_ERR_INVALID_ARG);
    assert(spl_nand_boot_slot(~0u) == ESP_ERR_INVALID_ARG);
    assert(reset_level == 1 && reset_releases == 1);
    check_cache(NULL);
    uint8_t reset[] = { 0xff };
    exchange(reset, sizeof(reset), NULL);
    check_cache(NULL);
    for (unsigned row = 0; row < 64; ++row) {
        uint8_t read[] = {0x13, 0, 0, row};
        exchange(read, sizeof(read), NULL);
        check_cache(flash_data + row * PAGE_SIZE);
        if (row > 1) {
            uint8_t back[] = {0x13, 0, 0, row - 1};
            exchange(back, sizeof(back), NULL);
            check_cache(flash_data + (row-1) * PAGE_SIZE);
            exchange(read, sizeof(read), NULL);
            check_cache(flash_data + row * PAGE_SIZE);
        }
    }
    assert(stats.cache_misses == 0);
    const unsigned rows[] = {0, 0, 64, 511, 512};
    for (unsigned i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i) {
        unsigned row=rows[i];
        uint8_t read[]={0x13,row>>16,row>>8,row};
        exchange(read,sizeof(read),NULL);
        check_cache(row == 511 || row == 512 ? NULL : flash_data);
    }
    assert(stats.cache_misses == 1);
    assert(spl_nand_boot(false) == ESP_OK);
    run_cache=false;
    for (unsigned row=0;row<4;++row) {
        uint8_t read[]={0x13,0,0,row};exchange(read,sizeof(read),NULL);
        check_cache(row<3 ? flash_data+row*PAGE_SIZE : NULL);
    }
    assert(stats.cache_misses == 1);
    run_cache=true;
    prefetch_pages();
    uint8_t retry[]={0x13,0,0,3};exchange(retry,sizeof(retry),NULL);
    check_cache(flash_data+3*PAGE_SIZE);
    assert(spl_nand_boot(false) == ESP_OK);
    run_cache=false;
    uint8_t page1[]={0x13,0,0,1};exchange(page1,sizeof(page1),NULL);
    uint8_t protected_page[FRAME_SIZE];memcpy(protected_page,transaction.tx_buffer,FRAME_SIZE);
    invalidate_during_copy=true;prefetch_pages();
    assert(!cache_enabled && !memcmp(protected_page,transaction.tx_buffer,FRAME_SIZE));
    for(unsigned i=0;i<CACHE_COUNT;++i) assert(cache_tags[i]!=3);
    run_cache=true;
    assert(spl_nand_boot(false) == ESP_OK);
    exchange(reset, sizeof(reset), NULL);
    check_cache(NULL);
    assert(spl_nand_start() == ESP_ERR_INVALID_STATE);
    assert(spl_nand_boot(true) == ESP_OK);
    uint8_t row0[] = {0x13, 0, 0, 0};
    exchange(row0, sizeof(row0), NULL);
    uint8_t expected[2048];
    memset(expected, 0xff, sizeof(expected));
    memcpy(expected, fel_spl_image, sizeof(fel_spl_image));
    check_cache(expected);
    assert(spl_nand_boot(false) == ESP_OK);
    exchange(row0, sizeof(row0), NULL);
    check_cache(flash_data);
    memcpy(flash_data + SLOT_SIZE, flash_data, SLOT_SIZE);
    flash_data[SLOT_SIZE+300]^=0x55;
    put_le32(flash_data+SLOT_SIZE+12,CHECKSUM_STAMP);
    checksum=0;
    for(unsigned i=0;i<SLOT_SIZE;i+=4) checksum+=get_le32(flash_data+SLOT_SIZE+i);
    put_le32(flash_data+SLOT_SIZE+12,checksum);
    assert(spl_nand_boot_slot(1) == ESP_OK);
    assert(loaded_slot == 1);
    exchange(row0, sizeof(row0), NULL);
    check_cache(flash_data + SLOT_SIZE);
    assert(spl_nand_boot_slot(0) == ESP_OK);
    assert(loaded_slot == 0);
    assert(task_count==1 && map_count==1);
    assert(sizeof(cache_frames)==5*2052);
    puts("bounded cache: sequential 64 pages, backtrack, repeat0, miss/retry, epoch, DMA exclusion, slot reset");
    return 0;
}
