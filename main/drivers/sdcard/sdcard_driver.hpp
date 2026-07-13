#pragma once

#include <atomic>

/*
 * SDCardDriver — manages SD card lifecycle via SDMMC native mode (SDIO 3.0).
 *
 * Init-once, never deinit. SD card is mounted at boot and stays mounted.
 * Uses ESP-IDF SDMMC host driver for 4-bit SDIO 3.0 mode.
 *
 * Hardware: microSD slot connected via SDIO 3.0 (CLK, CMD, D0-D3).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

class SDCardDriver {
public:
    static SDCardDriver& instance();

    /** Initialize SD card (idempotent). Thread-safe.
     *  Mounts at SDMMC_MOUNT_POINT with FAT filesystem.
     *  @return true on success (or already initialized) */
    bool init();

    /** No-op — SD card is never unmounted after init. */
    void deinit();

    /** @return true if SD card was successfully initialized */
    bool available() const { return _initialized.load(std::memory_order_relaxed); }

    /** @return SDMMC card handle (or nullptr) */
    sdmmc_card_t *card() const { return _card; }

    /* Delete copy/move */
    SDCardDriver(const SDCardDriver&) = delete;
    SDCardDriver& operator=(const SDCardDriver&) = delete;

private:
    SDCardDriver();
    ~SDCardDriver();

    SemaphoreHandle_t           _init_mutex;
    sdmmc_card_t               *_card;
    std::atomic<bool>           _initialized;
};