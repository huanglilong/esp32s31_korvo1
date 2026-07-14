/*
 * SDCardDriver — SDIO 3.0 SD card driver for ESP32-S31-Korvo-1.
 *
 * Follows esp_board_manager FS FAT device pattern:
 *   - init() takes dev_fs_fat_config_t + returns dev_fs_fat_handle_t*
 *   - Internally delegates to BSP: bsp_sdcard_mount() / bsp_sdcard_unmount()
 *   - Config-driven: SDMMC pins and settings from config struct
 *   - Init-once, never deinit (SD stays mounted permanently)
 */

#include "sdcard_driver.hpp"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "bsp/esp32_s31_korvo_1.h"
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
 * SD Card init — delegates to BSP bsp_sdcard_mount()
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

    /* Check if SD is already mounted */
    {
        struct stat st;
        if (stat(cfg->mount_point, &st) == 0) {
            ESP_LOGI(TAG, "SD card already mounted at %s", cfg->mount_point);
            _initialized.store(true, std::memory_order_relaxed);
            xSemaphoreGive(_init_mutex);
            return 0;
        }
    }

    ESP_LOGI(TAG, "Initializing SD card via BSP (SDMMC 4-bit mode)...");

    /* Allocate handle */
    dev_fs_fat_handle_t *fs_handle = new(std::nothrow) dev_fs_fat_handle_t();
    if (!fs_handle) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        xSemaphoreGive(_init_mutex);
        return -1;
    }
    memset(fs_handle, 0, sizeof(*fs_handle));

    /* Mount SD card via BSP */
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        delete fs_handle;
        xSemaphoreGive(_init_mutex);
        return -1;
    }

    /* Get SD card handle from BSP */
    fs_handle->card = bsp_sdcard_get_handle();
    fs_handle->mount_point = (char *)cfg->mount_point;

    _handles.store(fs_handle, std::memory_order_release);
    _initialized.store(true, std::memory_order_relaxed);

    ESP_LOGI(TAG, "SD card mounted at %s", cfg->mount_point);
    if (fs_handle->card) {
        sdmmc_card_print_info(stdout, fs_handle->card);
    }

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