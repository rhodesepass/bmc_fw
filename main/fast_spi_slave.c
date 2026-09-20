#include "fast_spi_slave.h"
#include "board_pins.h"

#include <stdbool.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_memory_utils.h"
#include "esp_private/spi_common_internal.h"
#include "hal/gdma_ll.h"
#include "hal/spi_ll.h"
#include "esp_rom_gpio.h"
#include "soc/spi_periph.h"
#include "soc/gpio_sig_map.h"

#if !CONFIG_IDF_TARGET_ESP32C3
#error "fast_spi_slave requires ESP32-C3"
#endif

static DMA_ATTR dma_descriptor_t rx_desc;
static DMA_ATTR dma_descriptor_t tx_desc;
static DRAM_ATTR struct {
    intr_handle_t intr;
    fast_spi_slave_done_cb_t done;
    void *ctx;
    uint8_t *rx;
    const uint8_t *tx;
    int rx_channel;
    int tx_channel;
    bool bus_allocated;
    bool dma_allocated;
    bool io_initialized;
    bool cs_initialized;
    bool initialized;
} state;

static inline __attribute__((always_inline)) void prepare_frame(void)
{
    spi_dev_t *hw = SPI_LL_GET_HW(SPI2_HOST);
    gdma_dev_t *dma = GDMA_LL_GET_HW(0);

    spi_ll_slave_reset(hw);
    gdma_ll_rx_reset_channel(dma, state.rx_channel);
    gdma_ll_tx_reset_channel(dma, state.tx_channel);
    const uint32_t rx_control = FAST_SPI_SLAVE_FRAME_SIZE | (1u << 30) | (1u << 31);
    const uint32_t tx_control = rx_control | (FAST_SPI_SLAVE_FRAME_SIZE << 12);
    __builtin_memcpy(&rx_desc.dw0, &rx_control, sizeof(rx_control));
    __builtin_memcpy(&tx_desc.dw0, &tx_control, sizeof(tx_control));
    tx_desc.buffer = (void *)state.tx;
    /* Publish descriptors before GDMA can fetch them. C3 has no data cache. */
    __asm__ volatile ("fence rw, rw" ::: "memory");
    spi_ll_dma_rx_fifo_reset(hw);
    spi_ll_infifo_full_clr(hw);
    spi_ll_dma_rx_enable(hw, true);
    spi_ll_dma_tx_fifo_reset(hw);
    spi_ll_outfifo_empty_clr(hw);
    spi_ll_dma_tx_enable(hw, true);
    gdma_ll_rx_set_desc_addr(dma, state.rx_channel, (uint32_t)&rx_desc);
    gdma_ll_tx_set_desc_addr(dma, state.tx_channel, (uint32_t)&tx_desc);
    gdma_ll_rx_start(dma, state.rx_channel);
    gdma_ll_tx_start(dma, state.tx_channel);
    spi_ll_clear_int_stat(hw);
    spi_ll_user_start(hw);
}

static void IRAM_ATTR transaction_done(void *arg)
{
    (void)arg;
    size_t bits = spi_ll_slave_get_rcv_bitlen(SPI_LL_GET_HW(SPI2_HOST));
    if (bits == FAST_SPI_SLAVE_FRAME_SIZE * 8 - 1)
        bits++;
    __asm__ volatile ("fence rw, rw" ::: "memory");
    const uint8_t *next = state.done(bits, state.ctx);
    if (next)
        state.tx = next;
    prepare_frame();

}

static bool valid_buffer(const void *buffer)
{
    return buffer && !((uintptr_t)buffer & 3) && esp_ptr_dma_capable(buffer) &&
           esp_ptr_dma_capable((const uint8_t *)buffer + FAST_SPI_SLAVE_FRAME_SIZE - 1);
}

void fast_spi_slave_deinit(void)
{
    if (state.intr) {
        esp_intr_disable(state.intr);
        esp_intr_free(state.intr);
    }
    if (state.bus_allocated) {
        spi_ll_disable_int(SPI_LL_GET_HW(SPI2_HOST));
        spi_ll_slave_reset(SPI_LL_GET_HW(SPI2_HOST));
    }
    if (state.dma_allocated) {
        gdma_ll_rx_stop(GDMA_LL_GET_HW(0), state.rx_channel);
        gdma_ll_tx_stop(GDMA_LL_GET_HW(0), state.tx_channel);
        spicommon_dma_chan_free(SPI2_HOST);
    }
    if (state.cs_initialized)
        spicommon_cs_free_io(BMC_PIN_SPI_CS, &spi_bus_get_attr(SPI2_HOST)->gpio_reserve);
    if (state.io_initialized)
        spicommon_bus_free_io_cfg(SPI2_HOST);
    if (state.bus_allocated)
        spicommon_bus_free(SPI2_HOST);
    memset(&state, 0, sizeof(state));
}

esp_err_t fast_spi_slave_init(uint8_t *rx_frame, fast_spi_slave_done_cb_t done, void *ctx)
{
    if (state.bus_allocated)
        return ESP_ERR_INVALID_STATE;
    if (!valid_buffer(rx_frame) || !done || !esp_ptr_in_iram(done) ||
        (ctx && !esp_ptr_internal(ctx)))
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = spicommon_bus_alloc(SPI2_HOST, "fast SPI slave");
    if (err != ESP_OK)
        return err;
    state.bus_allocated = true;
    state.rx = rx_frame;
    rx_desc.buffer = rx_frame;
    rx_desc.next = NULL;
    tx_desc.next = NULL;
    state.done = done;
    state.ctx = ctx;
    err = spicommon_dma_chan_alloc(SPI2_HOST, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        goto fail;
    state.dma_allocated = true;
    spi_dma_ctx_t *dma = spi_bus_get_dma_ctx(SPI2_HOST);
    err = gdma_get_channel_id(dma->rx_dma_chan, &state.rx_channel);
    if (err != ESP_OK)
        goto fail;
    err = gdma_get_channel_id(dma->tx_dma_chan, &state.tx_channel);
    if (err != ESP_OK)
        goto fail;
    spi_bus_config_t bus = {
        .mosi_io_num = BMC_PIN_SPI_MOSI,
        .miso_io_num = BMC_PIN_SPI_MISO,
        .sclk_io_num = BMC_PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FAST_SPI_SLAVE_FRAME_SIZE,
    };
    err = spicommon_bus_initialize_io(SPI2_HOST, &bus, SPICOMMON_BUSFLAG_SLAVE, NULL);
    if (err != ESP_OK)
        goto fail;
    state.io_initialized = true;
    spicommon_cs_initialize(SPI2_HOST, BMC_PIN_SPI_CS, 0, true, NULL);
    state.cs_initialized = true;
    spi_dev_t *hw = SPI_LL_GET_HW(SPI2_HOST);
    spi_ll_slave_init(hw);
    spi_ll_set_rx_lsbfirst(hw, false);
    spi_ll_set_tx_lsbfirst(hw, false);
    spi_ll_slave_set_mode(hw, 0, true);
    spi_ll_enable_mosi(hw, true);
    spi_ll_enable_miso(hw, true);
    spi_ll_slave_set_rx_bitlen(hw, FAST_SPI_SLAVE_FRAME_SIZE * 8);
    spi_ll_slave_set_tx_bitlen(hw, FAST_SPI_SLAVE_FRAME_SIZE * 8);
    spi_ll_disable_int(hw);
    spi_ll_clear_int_stat(hw);
    err = esp_intr_alloc(spicommon_irqsource_for_host(SPI2_HOST),
                         ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_INTRDISABLED,
                         transaction_done, NULL, &state.intr);
    if (err != ESP_OK)
        goto fail;
    state.initialized = true;
    return ESP_OK;
fail:
    fast_spi_slave_deinit();
    return err;
}

esp_err_t fast_spi_slave_arm(const uint8_t *tx_frame)
{
    if (!state.initialized)
        return ESP_ERR_INVALID_STATE;
    if (!valid_buffer(tx_frame))
        return ESP_ERR_INVALID_ARG;
    esp_intr_disable(state.intr);
    state.tx = tx_frame;
    esp_rom_gpio_connect_in_signal(BMC_PIN_SPI_CS, spi_periph_signal[SPI2_HOST].spics_in, false);
    prepare_frame();
    spi_ll_enable_int(SPI_LL_GET_HW(SPI2_HOST));
    return esp_intr_enable(state.intr);
}

esp_err_t fast_spi_slave_pause(void)
{
    if (!state.initialized) return ESP_ERR_INVALID_STATE;
    esp_intr_disable(state.intr);
    spi_ll_disable_int(SPI_LL_GET_HW(SPI2_HOST));
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, spi_periph_signal[SPI2_HOST].spics_in, false);
    spi_ll_slave_reset(SPI_LL_GET_HW(SPI2_HOST));
    gdma_ll_rx_stop(GDMA_LL_GET_HW(0), state.rx_channel);
    gdma_ll_tx_stop(GDMA_LL_GET_HW(0), state.tx_channel);
    return ESP_OK;
}
