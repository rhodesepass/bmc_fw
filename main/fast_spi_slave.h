#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define FAST_SPI_SLAVE_FRAME_SIZE 2052u

/* Runs in IRAM ISR context; NULL retains the current TX frame. */
typedef const uint8_t *(*fast_spi_slave_done_cb_t)(size_t bitlen, void *ctx);

/* Buffers must be 4-byte aligned DMA RAM; callback and its callees must reside in IRAM. */
esp_err_t fast_spi_slave_init(uint8_t *rx_frame, fast_spi_slave_done_cb_t done, void *ctx);
/* Call with the master held in reset; returns with the fixed-size DMA frame ready. */
esp_err_t fast_spi_slave_arm(const uint8_t *tx_frame);
/* Keep the master in reset while releasing the peripheral and its pins. */
void fast_spi_slave_deinit(void);

esp_err_t fast_spi_slave_pause(void);
