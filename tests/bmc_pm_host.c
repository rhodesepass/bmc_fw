#include "bmc_pm_stub.h"
#include "../main/bmc_pm.c"

int main(void)
{
    char response[256];
    bmc_pm_boot(true);
    assert(bmc_pm_init() == ESP_OK);
    assert(lock_count == 2 && fake_locks[0].kind == ESP_PM_APB_FREQ_MAX);
    assert(esp_clk_cpu_freq() == 160000000 && radio_calls == 0);
    assert(bmc_pm_radio_ready() == ESP_OK && fake_bt_sleep);
    bmc_pm_boot(false);
    bmc_pm_boot(false);
    assert(esp_clk_cpu_freq() == 160000000);
    assert(bmc_pm_command("pm-status", response, sizeof(response)) == ESP_OK);
    assert(strstr(response, "mode=modem") && strstr(response, "boot_lock=0"));
    assert(bmc_pm_command("pm performance", response, sizeof(response)) == ESP_OK);
    assert(!fake_bt_sleep && esp_clk_cpu_freq() == 160000000);
    assert(bmc_pm_command("pm modem", response, sizeof(response)) == ESP_OK);
    assert(fake_bt_sleep && esp_clk_cpu_freq() == 160000000);
    assert(bmc_pm_command("pm balanced", response, sizeof(response)) == ESP_OK);
    assert(fake_bt_sleep && esp_clk_cpu_freq() == 80000000);
    for (unsigned i = 0; i < 20; ++i) bmc_pm_boot(true);
    assert(esp_clk_cpu_freq() == 160000000 && fake_locks[1].depth == 1);
    assert(bmc_pm_command("pm balanced", response, sizeof(response)) == ESP_OK);
    assert(esp_clk_cpu_freq() == 160000000);
    bmc_pm_boot(false);
    assert(esp_clk_cpu_freq() == 80000000 && fake_locks[0].depth == 1);
    assert(bmc_pm_command("pm balanced extra", response, sizeof(response)) == ESP_ERR_INVALID_ARG);
    assert(bmc_pm_command("pm sleep", response, sizeof(response)) == ESP_ERR_INVALID_ARG);
    assert(fake_bt_sleep && configured.min_freq_mhz == 80 && !configured.light_sleep_enable);
    radio_error = ESP_ERR_INVALID_STATE;
    assert(bmc_pm_command("pm performance", response, sizeof(response)) == ESP_ERR_INVALID_STATE);
    assert(fake_bt_sleep && configured.min_freq_mhz == 80);
    assert(strstr(response, "mode=balanced"));
    radio_error = 0;
    acquire_error = ESP_ERR_INVALID_ARG;
    assert(bmc_pm_boot(true) == ESP_ERR_INVALID_ARG);
    assert(fake_locks[1].depth == 0 && !boot_active);
    acquire_error = 0;
    assert(bmc_pm_boot(true) == ESP_OK && boot_active);
    release_error = ESP_ERR_INVALID_STATE;
    assert(bmc_pm_boot(false) == ESP_ERR_INVALID_STATE);
    assert(fake_locks[1].depth == 1 && boot_active);
    release_error = 0;
    assert(bmc_pm_boot(false) == ESP_OK && !boot_active);
    assert(boot_mux == 0);
    puts("BMC PM: APB protection, boot lock balance, radio and three modes passed");
    return 0;
}
