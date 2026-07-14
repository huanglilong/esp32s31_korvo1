#pragma once

#include <atomic>

/*
 * SDCardDriver — manages SD card lifecycle via SDMMC native mode (SDIO 3.0).
 *
 * Follows esp_board_manager FS FAT device pattern:
 *   - dev_fs_fat_config_t: YAML-like configuration struct
 *   - dev_fs_fat_handle_t: handle with sdmmc_card_t, host, mount_point
 *   - Sub-type driven: sdmmc (SDIO 3.0 4-bit)
 *   - Internally delegates to BSP: bsp_sdcard_mount()
 *
 * Init-once, never deinit. SD card is mounted at boot and stays mounted.
 * Uses ESP-IDF SDMMC host driver for 4-bit SDIO 3.0 mode.
 *
 * Hardware: microSD slot connected via SDIO 3.0 (CLK, CMD, D0-D3).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

/* ── esp_board_manager-compatible config structures ────────────── */

/** SDMMC pin configuration (matches esp_board_manager dev_fs_fat_sdmmc_pins_t) */
typedef struct {
    int  clk;  /*!< Clock pin */
    int  cmd;  /*!< Command pin */
    int  d0;   /*!< Data line 0 pin */
    int  d1;   /*!< Data line 1 pin */
    int  d2;   /*!< Data line 2 pin */
    int  d3;   /*!< Data line 3 pin */
} dev_fs_fat_sdmmc_pins_t;

/** SDMMC sub configuration (matches esp_board_manager dev_fs_fat_sdmmc_sub_config_t) */
typedef struct {
    uint8_t                  slot;         /*!< SDMMC slot number */
    uint8_t                  bus_width;    /*!< Bus width (1, 4, or 8 bits) */
    uint16_t                 slot_flags;   /*!< Slot flags (SDMMC_SLOT_FLAG_INTERNAL_PULLUP, etc.) */
    dev_fs_fat_sdmmc_pins_t  pins;         /*!< GPIO pin configuration */
} dev_fs_fat_sdmmc_sub_config_t;

/** VFS mount configuration (matches esp_board_manager dev_fs_fat_vfs_config_t) */
typedef struct {
    bool      format_if_mount_failed;  /*!< Format if mount fails */
    uint32_t  max_files;              /*!< Max open files */
    uint32_t  allocation_unit_size;   /*!< Allocation unit size in bytes */
} dev_fs_fat_vfs_config_t;

/** FS FAT configuration (matches esp_board_manager dev_fs_fat_config_t) */
typedef struct {
    const char                   *name;          /*!< Device name */
    const char                   *mount_point;   /*!< Mount point path */
    uint32_t                      frequency;     /*!< Clock frequency in Hz */
    dev_fs_fat_vfs_config_t       vfs_config;    /*!< VFS configuration */
    const char                   *sub_type;      /*!< Sub type: "sdmmc" or "spi" */
    dev_fs_fat_sdmmc_sub_config_t sdmmc;         /*!< SDMMC sub config */
} dev_fs_fat_config_t;

/** FS FAT device handle (matches esp_board_manager dev_fs_fat_handle_t) */
typedef struct {
    sdmmc_card_t *card;         /*!< SDMMC card handle */
    sdmmc_host_t  host;         /*!< SDMMC host config */
    char         *mount_point;  /*!< Mount point path */
} dev_fs_fat_handle_t;

/* ── SDCardDriver class ────────────────────────────────────────── */

class SDCardDriver {
public:
    static SDCardDriver& instance();

    /**
     * @brief  Initialize SD card with given configuration.
     *         Follows esp_board_manager dev_fs_fat_init() signature.
     *         Idempotent. Thread-safe.
     *
     * @param[in]  cfg      Pointer to dev_fs_fat_config_t
     * @param[in]  cfg_size Size of config struct
     * @param[out] handle   Pointer to receive dev_fs_fat_handle_t*
     * @return 0 on success, -1 on failure
     */
    int init(dev_fs_fat_config_t *cfg, int cfg_size, void **handle);

    /** No-op — SD card is never unmounted after init. */
    void deinit();

    /** @return true if SD card was successfully initialized */
    bool available() const { return _initialized.load(std::memory_order_relaxed); }

    /** @return the device handles struct */
    const dev_fs_fat_handle_t* handles() const {
        return _handles.load(std::memory_order_acquire);
    }

    /** @return SDMMC card handle (or nullptr) */
    sdmmc_card_t *card() const;

    /* Delete copy/move */
    SDCardDriver(const SDCardDriver&) = delete;
    SDCardDriver& operator=(const SDCardDriver&) = delete;

private:
    SDCardDriver();
    ~SDCardDriver();

    SemaphoreHandle_t           _init_mutex;
    std::atomic<dev_fs_fat_handle_t*> _handles;
    std::atomic<bool>           _initialized;
};