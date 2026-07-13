/*
 * Logger — Text log output to SD card with level filtering.
 *
 * Design:
 *   - Uses esp_log_set_vprintf() to intercept ALL ESP_LOGx calls
 *     across the entire firmware.  Zero code changes needed in existing
 *     source files — every ESP_LOGI / ESP_LOGW / ESP_LOGE automatically
 *     appears on both UART and the SD card.
 *   - A background writer task drains a ring buffer to
 *     /sdcard/logs/app_NNNNNN.log, rotating files when they
 *     exceed the configured size limit.
 *   - SD-card level is independent of ESP-IDF CONFIG_LOG_DEFAULT_LEVEL:
 *     you can log INFO to UART but only WARN+ to the SD card, etc.
 *   - Existing ESP_LOGx calls work exactly as before.  No new macros
 *     are needed.
 *
 * Usage:
 *   #include "logger/logger.hpp"
 *   logger_init("/sdcard");                     // once after SD mount
 *   logger_set_sd_level(LOG_LEVEL_WARN);        // runtime filter (optional)
 *   logger_deinit();                            // on shutdown
 *
 *   // All existing ESP_LOGI/W/E calls now also write to SD card.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Log-level enum (matches ESP-IDF esp_log_level_t values) ───── */
typedef enum {
    LOG_LEVEL_NONE  = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO  = 3,
    LOG_LEVEL_DEBUG = 4,
} logger_level_t;

/* ── Public API ────────────────────────────────────────────────── */

/** Initialise the logger.  Must be called *after* the SD card is
 *  mounted.  Starts the writer task and intercepts all ESP_LOGx calls
 *  via esp_log_set_vprintf().
 *
 *  @param sd_base  SD mount point, e.g. "/sdcard".
 *  @return true on success. */
bool logger_init(const char *sd_base);

/** Stop the writer task and close the current log file. */
void logger_deinit(void);

/** Runtime override of the minimum level written to the SD card.
 *  UART output is unaffected.  Default: CONFIG_APP_LOG_SD_LEVEL. */
void logger_set_sd_level(logger_level_t level);

/** @return current SD-card log level. */
logger_level_t logger_get_sd_level(void);

/** @return true if the logger is actively writing to an SD file. */
bool logger_is_running(void);

/** Get the path of the current log file (empty string if not running). */
const char *logger_get_filepath(void);

#ifdef __cplusplus
}
#endif
