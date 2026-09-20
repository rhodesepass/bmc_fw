#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t page_reads;
    uint32_t cache_reads;
    uint32_t cache_misses;
    uint32_t unexpected;
    esp_err_t queue_error;
} spl_nand_stats_t;

esp_err_t spl_nand_start(void);

void spl_nand_get_stats(spl_nand_stats_t *stats);

esp_err_t spl_nand_prepare(void);
esp_err_t spl_nand_boot(bool fel);
esp_err_t spl_nand_hold(void);
bool spl_nand_is_fel(void);

#define SPL_NAND_TRACE_COUNT 256u
typedef struct {
    uint32_t cycles, bits, rx, tx;
} spl_nand_trace_t;
bool spl_nand_get_trace(unsigned index, spl_nand_trace_t *entry);

esp_err_t spl_nand_enter_fel(void);

void spl_nand_trace_enable(bool enabled);

esp_err_t spl_nand_boot_slot(unsigned slot);
