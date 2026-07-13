/*
 * SDCardDriver — SDIO 3.0 SD card driver for ESP32-S31-Korvo-1.
 *
 * Init-once, never deinit. Uses SDMMC host in 4-bit mode.
 */

#include "sdcard_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "example_config.h"
#include <sys/stat.h>

static const char *TAG = "SDCardDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
SDCardDriver& SDCardDriver::instance() {
    static SDCardDriver s;
    return s;
}

SDCardDriver::SDCardDriver() :
    _init_mutex(nullptr),
    _card(nullptr),
    _initialized(false)
{
    _init_mutex = xSemaphoreCreateMutex();
}

SDCardDriver::~SDCardDriver() {
    if (_init_mutex) {
        vSemaphoreDelete(_init_mutex);
        _init_mutex = nullptr;
    }
}

/*============================================================================
 * SD Card init — SDMMC 4-bit mode
 *============================================================================*/
bool SDCardDriver::init() {
    if (!_init_mutex) return false;
    xSemaphoreTake(_init_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "SD card already initialized");
        xSemaphoreGive(_init_mutex);
        return true;
    }

    /* Check if SD is already mounted (e.g., from previous boot or BSP init) */
    {
        struct stat st;
        if (stat(SDMMC_MOUNT_POINT, &st) == 0) {
            ESP_LOGI(TAG, "SD card already mounted at %s", SDMMC_MOUNT_POINT);
            _initialized.store(true, std::memory_order_relaxed);
            xSemaphoreGive(_init_mutex);
            return true;
        }
    }

    ESP_LOGI(TAG, "Initializing SD card via SDMMC 4-bit mode...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024
    };
    const char mount_point[] = SDMMC_MOUNT_POINT;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_4BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = (gpio_num_t)CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = (gpio_num_t)CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D0;
    slot_config.d1 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,
            &mount_config, &_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        xSemaphoreGive(_init_mutex);
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, _card);
    _initialized.store(true, std::memory_order_relaxed);
    xSemaphoreGive(_init_mutex);
    return true;
}

void SDCardDriver::deinit() {
    /* SD card is never unmounted — kept for API compatibility */
    ESP_LOGI(TAG, "SD card deinit skipped (SD stays mounted permanently)");
}