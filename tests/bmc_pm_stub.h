#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef int portMUX_TYPE;
typedef struct { int kind, depth; } fake_pm_lock;
typedef fake_pm_lock *esp_pm_lock_handle_t;
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_PM_APB_FREQ_MAX 2
#define ESP_PM_CPU_FREQ_MAX 3
#define IRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL_SAFE(mux) (++*(mux))
#define portEXIT_CRITICAL_SAFE(mux) (--*(mux))

static fake_pm_lock fake_locks[2];
static unsigned lock_count;
static esp_pm_config_t configured;
static bool fake_bt_sleep;
static unsigned radio_calls;
static int acquire_error, release_error, radio_error;
static esp_err_t esp_pm_lock_create(int kind, int unused, const char *name, esp_pm_lock_handle_t *out)
{
    (void)unused;
    assert(name && lock_count < 2);
    *out = &fake_locks[lock_count++];
    (*out)->kind = kind;
    return ESP_OK;
}
static esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle)
{
    if (acquire_error) return acquire_error;
    assert(handle && ++handle->depth == 1);
    return ESP_OK;
}
static esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle)
{
    if (release_error) return release_error;
    assert(handle && --handle->depth == 0);
    return ESP_OK;
}
static esp_err_t esp_pm_configure(const esp_pm_config_t *config)
{
    assert(config->max_freq_mhz == 160 && !config->light_sleep_enable);
    assert(config->min_freq_mhz == 80 || config->min_freq_mhz == 160);
    configured = *config;
    return ESP_OK;
}
static esp_err_t esp_pm_get_configuration(esp_pm_config_t *config) { *config = configured; return ESP_OK; }
static esp_err_t esp_bt_sleep_enable(void)
{
    radio_calls++;
    if (radio_error) return radio_error;
    fake_bt_sleep = true;
    return ESP_OK;
}
static esp_err_t esp_bt_sleep_disable(void)
{
    radio_calls++;
    if (radio_error) return radio_error;
    fake_bt_sleep = false;
    return ESP_OK;
}
static int esp_clk_cpu_freq(void)
{
    return (fake_locks[1].depth ? configured.max_freq_mhz : configured.min_freq_mhz) * 1000000;
}
static int esp_clk_apb_freq(void) { assert(fake_locks[0].depth == 1); return 80000000; }
static const char *esp_err_to_name(int result) { return result ? "ERROR" : "OK"; }
