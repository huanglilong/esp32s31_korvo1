/*
 * SDCardDriver — SDIO 3.0 SD card driver for ESP32-S31-Korvo-1.
 *
 * Follows esp_board_manager FS FAT device pattern:
 *   - init() takes dev_fs_fat_config_t + returns dev_fs_fat_handle_t*
 *   - Config-driven: SDMMC pins and settings from config struct
 *   - Init-once, never deinit (SD stays mounted permanently)
 */

#include "sdcard_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "app_config.h"
#include <sys/stat.h>
#include <cstring>

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
    _handles(nullptr),
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
 * SD Card init — SDMMC 4-bit mode (config-driven)
 *============================================================================*/
int SDCardDriver::init(dev_fs_fat_config_t *cfg, int cfg_size, void **handle) {
    if (!cfg || !handle) {
        ESP_LOGE(TAG, "Invalid parameters: cfg=%p, handle=%p", (void*)cfg, (void*)handle);
        return -1;
    }
    if (cfg_size != (int)sizeof(dev_fs_fat_config_t)) {
        ESP_LOGE(TAG, "Invalid cfg_size: %d (expected %d)", cfg_size, (int)sizeof(dev_fs_fat_config_t));
        return -1;
    }

    if (!_init_mutex) return -1;
    xSemaphoreTake(_init_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "SD card already initialized");
        if (handle) *handle = _handles.load(std::memory_order_acquire);
        xSemaphoreGive(_init_mutex);
        return 0;
    }

    /* Check if SD is already mounted (e.g., from previous boot) */
    {
        struct stat st;
        if (stat(cfg->mount_point, &st) == 0) {
            ESP_LOGI(TAG, "SD card already mounted at %s", cfg->mount_point);
            _initialized.store(true, std::memory_order_relaxed);
            xSemaphoreGive(_init_mutex);
            return 0;
        }
    }

    ESP_LOGI(TAG, "Initializing SD card via SDMMC 4-bit mode...");

    /* Allocate handle */
    dev_fs_fat_handle_t *fs_handle = new(std::nothrow) dev_fs_fat_handle_t();
    if (!fs_handle) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        xSemaphoreGive(_init_mutex);
        return -1;
    }
    memset(fs_handle, 0, sizeof(*fs_handle));

    /* Configure mount */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = cfg->vfs_config.format_if_mount_failed,
        .max_files = (int)cfg->vfs_config.max_files,
        .allocation_unit_size = cfg->vfs_config.allocation_unit_size
    };

    /* Configure SDMMC host */
    fs_handle->host = SDMMC_HOST_DEFAULT();
    fs_handle->host.flags = SDMMC_HOST_FLAG_4BIT;
    fs_handle->host.max_freq_khz = (int)(cfg->frequency / 1000);

    /* Configure SDMMC slot */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = cfg->sdmmc.bus_width;
    slot_config.clk = (gpio_num_t)cfg->sdmmc.pins.clk;
    slot_config.cmd = (gpio_num_t)cfg->sdmmc.pins.cmd;
    slot_config.d0 = (gpio_num_t)cfg->sdmmc.pins.d0;
    slot_config.d1 = (gpio_num_t)cfg->sdmmc.pins.d1;
    slot_config.d2 = (gpio_num_t)cfg->sdmmc.pins.d2;
    slot_config.d3 = (gpio_num_t)cfg->sdmmc.pins.d3;
    slot_config.flags |= cfg->sdmmc.slot_flags;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(cfg->mount_point, &fs_handle->host,
            &slot_config, &mount_config, &fs_handle->card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        delete fs_handle;
        xSemaphoreGive(_init_mutex);
        return -1;
    }

    /* Store mount point (borrowed from config, caller must keep it alive) */
    fs_handle->mount_point = (char *)cfg->mount_point;

    _handles.store(fs_handle, std::memory_order_release);
    _initialized.store(true, std::memory_order_relaxed);

    ESP_LOGI(TAG, "SD card mounted at %s", cfg->mount_point);
    sdmmc_card_print_info(stdout, fs_handle->card);

    if (handle) *handle = fs_handle;
    xSemaphoreGive(_init_mutex);
    return 0;
}

sdmmc_card_t *SDCardDriver::card() const {
    dev_fs_fat_handle_t *h = _handles.load(std::memory_order_acquire);
    return h ? h->card : nullptr;
}

void SDCardDriver::deinit() {
    /* SD card is never unmounted — kept for API compatibility */
    ESP_LOGI(TAG, "SD card deinit skipped (SD stays mounted permanently)");
}
