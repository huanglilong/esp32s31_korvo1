/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ULogWriter — Write uORB topics to SD card in ULog format.
 *
 * Design:
 *   - Singleton, initialized once at boot.
 *   - Topics are registered via add_topic() with per-topic sampling intervals.
 *   - start() creates a new .ulog file and writes header/format/subscription sections.
 *   - A background task polls topics and writes DATA messages to a ring buffer.
 *   - A consumer periodically flushes the ring buffer to the SD card file.
 *   - stop() closes the file cleanly.
 *
 * Usage:
 *   ULogWriter::instance().init("/sdcard");
 *   ULogWriter::instance().add_topic(ORB_ID(fps_stats), 100);   // 100ms interval
 *   ULogWriter::instance().add_topic(ORB_ID(wifi_state), 200);  // 200ms interval
 *   ULogWriter::instance().start();
 *   ...
 *   ULogWriter::instance().stop();
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "uorb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default sampling interval for topics that specify 0 (ms). */
#ifdef CONFIG_ULOG_DEFAULT_INTERVAL_MS
#define ULOG_DEFAULT_INTERVAL_MS  CONFIG_ULOG_DEFAULT_INTERVAL_MS
#else
#define ULOG_DEFAULT_INTERVAL_MS  100
#endif

/** Maximum number of logged topics. */
#define ULOG_MAX_TOPICS 16

/** Maximum log file path length. */
#define ULOG_MAX_PATH 256

/** Ring buffer size (bytes). Default 64KB to accommodate camera frame topics. */
#ifdef CONFIG_ULOG_RINGBUF_SIZE
#define ULOG_RINGBUF_SIZE  CONFIG_ULOG_RINGBUF_SIZE
#else
#define ULOG_RINGBUF_SIZE  (64 * 1024)
#endif

/** How often to flush ring buffer to file (ms). */
#define ULOG_FLUSH_INTERVAL_MS 100

/** How often to write sync markers (ms). */
#define ULOG_SYNC_INTERVAL_MS 500

/** How often to fsync the file (ms). */
#define ULOG_FSYNC_INTERVAL_MS 1000

/** Maximum single log file size before rotation (bytes).
 *  Default 100 MB. */
#ifndef ULOG_MAX_FILE_SIZE
#ifdef CONFIG_ULOG_MAX_FILE_SIZE
#define ULOG_MAX_FILE_SIZE  CONFIG_ULOG_MAX_FILE_SIZE
#else
#define ULOG_MAX_FILE_SIZE  (100 * 1024 * 1024)
#endif
#endif

/** Git version info — passed from the application at init time. */
typedef struct {
    char branch[64];     /**< Git branch name, e.g. "main" */
    char commit[16];     /**< Git short commit hash, e.g. "a1b2c3d" */
    char author[64];     /**< Git commit author, e.g. "User <user@example.com>" */
    char date[64];       /**< Git commit date, e.g. "2026-07-05 18:00:00 +0800" */
    char message[128];   /**< Git commit message (first line) */
} ulog_git_info_t;

/** Initialization config — all hardware/platform-dependent params
 *  supplied by the application, keeping the ULog component portable. */
typedef struct {
    uint16_t session_counter; /**< Boot session number (managed by app via NVS) */
    bool     has_wall_clock;  /**< True if SNTP/RTC has synced wall-clock time */
    char     sys_name[32];    /**< System name, e.g. "esp32p4_monitor" */
    char     ver_sw[32];      /**< Software version, e.g. "IDF v6.0.2" */
    char     ver_hw[32];      /**< Hardware board name, e.g. "ESP32-P4-WIFI6" */
    char     sys_uuid[24];    /**< Unique ID (e.g. MAC-based), e.g. "E8F60AE6E08A" */
    char     sys_os_name[16]; /**< OS name, e.g. "FreeRTOS" */
    char     sys_os_ver[32];  /**< OS version, e.g. "v6.0.2" */
    char     sys_mcu[32];     /**< MCU name, e.g. "ESP32-P4NRW32" */
    char     arch[16];        /**< Architecture, e.g. "esp32p4" */
} ulog_init_config_t;

/** ULog writer states. */
typedef enum {
    ULOG_STATE_UNINIT = 0,
    ULOG_STATE_IDLE,
    ULOG_STATE_RUNNING,
    ULOG_STATE_ERROR,
} ulog_state_t;

/**
 * ULogWriter singleton.
 */
typedef struct ulog_writer ulog_writer_t;

/**
 * Create / get the ULogWriter singleton instance.
 */
ulog_writer_t *ulog_writer_get(void);

/**
 * Initialize the ULogWriter.
 *
 * @param writer         ULogWriter instance
 * @param sd_mount_path  SD card mount path, e.g. "/sdcard"
 * @param config         Initialization config (session, hardware info, etc.)
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_init(ulog_writer_t *writer, const char *sd_mount_path,
                           const ulog_init_config_t *config);

/**
 * Set git version info for ULog Info messages.
 * Must be called after init(), before start().
 *
 * @param writer   ULogWriter instance
 * @param git_info Pointer to git info struct (contents are copied)
 */
void ulog_writer_set_git_info(ulog_writer_t *writer, const ulog_git_info_t *git_info);

/**
 * Update wall-clock availability status.
 * Call when SNTP syncs or when starting logging to re-check.
 *
 * @param writer          ULogWriter instance
 * @param has_wall_clock  True if wall-clock time is available
 */
void ulog_writer_set_wall_clock(ulog_writer_t *writer, bool has_wall_clock);

/**
 * Register a topic for logging.
 *
 * @param meta          Topic metadata (use ORB_ID(name))
 * @param interval_ms   Minimum interval between samples (ms). 0 = default.
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_add_topic(ulog_writer_t *writer, orb_id_t meta,
                                uint32_t interval_ms);

/**
 * Start logging — creates a new .ulog file and begins data collection.
 *
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_start(ulog_writer_t *writer);

/**
 * Stop logging — closes the current file.
 *
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_stop(ulog_writer_t *writer);

/**
 * Get current writer state.
 */
ulog_state_t ulog_writer_get_state(const ulog_writer_t *writer);

/**
 * Get the current log file path (empty string if not logging).
 */
const char *ulog_writer_get_filepath(const ulog_writer_t *writer);

/**
 * Get the number of bytes written to the current log file.
 */
size_t ulog_writer_get_bytes_written(const ulog_writer_t *writer);

/**
 * Write a logging message to the ULog file (for debug text).
 *
 * @param level    Log level (0=emerg through 7=debug)
 * @param message  NUL-terminated string
 * @return ESP_OK on success
 */
esp_err_t ulog_writer_write_message(ulog_writer_t *writer, uint8_t level,
                                    const char *message);

/**
 * Deinitialize and free resources.
 */
void ulog_writer_deinit(ulog_writer_t *writer);

#ifdef __cplusplus
}
#endif
