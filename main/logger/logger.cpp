/*
 * Logger — Text log output to SD card with level filtering.
 *
 * Architecture:
 *   ESP_LOGx macros
 *   ──────────────
 *   esp_log() ──▶ esp_log_set_vprintf() hook
 *                        │
 *              ┌─────────┴──────────┐
 *              ▼                    ▼
 *     orig_vprintf (UART)    _logger_push()
 *                             (ring buffer → SD card)
 */

#include "logger.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "git_info.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ── Configurable defaults ──────────────────────────────────────── */
#ifndef CONFIG_APP_LOG_SD_LEVEL
#define CONFIG_APP_LOG_SD_LEVEL  LOG_LEVEL_INFO
#endif
#ifndef CONFIG_APP_LOG_MAX_FILE_SIZE_KB
#define CONFIG_APP_LOG_MAX_FILE_SIZE_KB  1024
#endif
#ifndef CONFIG_APP_LOG_MAX_FILES
#define CONFIG_APP_LOG_MAX_FILES  5
#endif
#ifndef CONFIG_APP_LOG_RINGBUF_SIZE
#define CONFIG_APP_LOG_RINGBUF_SIZE  8192
#endif

/* ── Constants ──────────────────────────────────────────────────── */
#define LOG_DIR_NAME      "logs"
#define LOG_FILE_PREFIX   "app_"
#define LOG_FILE_SUFFIX   ".log"
#define WRITER_TASK_STACK  4096
#define WRITER_TASK_PRIO   3
#define WRITER_TASK_NAME   "logger_writer"
#define MAX_LINE_LEN       512

/* ── Static state ───────────────────────────────────────────────── */
static struct {
    std::atomic<bool> running{false};  /* Atomic: cross-core set by logger_deinit, read by _logger_vprintf */
    std::atomic<int>   sd_level{0};

    /* Ring buffer */
    uint8_t         *buf;
    size_t           buf_size;
    size_t           head;    /* write index */
    size_t           tail;    /* read index */
    SemaphoreHandle_t buf_mutex;
    SemaphoreHandle_t data_sem;  /* signals writer when data available */

    /* In-flight tracking: deinit waits for producers to exit before
     * deleting buf_mutex, preventing use-after-free on the mutex. */
    std::atomic<int>  push_in_flight{0};

    /* File I/O */
    char             dir_path[64];
    char             file_path[128];
    FILE            *fd;
    size_t           bytes_written;
    std::atomic<bool> writer_running{false};
    TaskHandle_t     writer_task;
    std::atomic<bool> writer_exited{false};  /* set by writer task before vTaskDelete(NULL) */
    /* vprintf hook */
    vprintf_like_t   orig_vprintf;  /* saved original for UART */
} s_log;

/* ── Forward declarations ───────────────────────────────────────── */
static void _logger_writer_task(void *arg);
static void _logger_push(const char *data, int len);
static int  _logger_vprintf(const char *format, va_list args);
static void _rotate_file(void);
static void _cleanup_old_logs(void);
static int  _strip_ansi(char *str, int len);

/* ── Public API ─────────────────────────────────────────────────── */

bool logger_init(const char *sd_base)
{
    if (s_log.running.load(std::memory_order_acquire)) return true;

    /* Sanity: SD path must be a valid mount */
    struct stat st;
    snprintf(s_log.dir_path, sizeof(s_log.dir_path),
             "%s/" LOG_DIR_NAME, sd_base);
    if (stat(sd_base, &st) != 0) {
        return false;
    }
    mkdir(s_log.dir_path, 0755);

    /* Allocate ring buffer */
    s_log.buf_size = CONFIG_APP_LOG_RINGBUF_SIZE;
    s_log.buf = (uint8_t *)malloc(s_log.buf_size);
    if (!s_log.buf) return false;
    s_log.head = 0;
    s_log.tail = 0;

    s_log.buf_mutex = xSemaphoreCreateMutex();
    s_log.data_sem  = xSemaphoreCreateBinary();
    if (!s_log.buf_mutex || !s_log.data_sem) {
        free(s_log.buf);
        s_log.buf = nullptr;
        return false;
    }

    /* SD log level (Kconfig → runtime default) */
    s_log.sd_level.store((int)CONFIG_APP_LOG_SD_LEVEL, std::memory_order_relaxed);

    /* Open first log file */
    s_log.fd = nullptr;
    s_log.bytes_written = 0;
    _rotate_file();

    /* Start writer task */
    s_log.writer_running = true;
    s_log.writer_exited = false;
    if (xTaskCreatePinnedToCore(_logger_writer_task, WRITER_TASK_NAME,
                    WRITER_TASK_STACK, NULL,
                    WRITER_TASK_PRIO, &s_log.writer_task, 0) != pdPASS) {  /* Core 0 */
        if (s_log.fd) { fclose(s_log.fd); s_log.fd = nullptr; }
        vSemaphoreDelete(s_log.buf_mutex); s_log.buf_mutex = nullptr;
        vSemaphoreDelete(s_log.data_sem);  s_log.data_sem = nullptr;
        free(s_log.buf);
        s_log.buf = nullptr;
        return false;
    }

    s_log.running.store(true, std::memory_order_release);

    /* Install vprintf hook — captures ALL ESP_LOGx output for SD card */
    s_log.orig_vprintf = esp_log_set_vprintf(_logger_vprintf);

    return true;
}

void logger_deinit(void)
{
    if (!s_log.running.load(std::memory_order_acquire)) return;

    /* Atomic store — synchronizes with _logger_vprintf's acquire load.
     * Any _logger_vprintf that reads false will skip _logger_push. */
    s_log.running.store(false, std::memory_order_release);

    /* Restore original vprintf FIRST — no more SD log writes from this point.
     * However, _logger_vprintf calls already in flight (past the running check
     * but not yet finished) may still be executing.  Wait for them to exit
     * by polling the in-flight counter. */
    if (s_log.orig_vprintf) {
        esp_log_set_vprintf(s_log.orig_vprintf);
        s_log.orig_vprintf = NULL;
    }

    /* Wait for in-flight _logger_push calls to finish.
     * They increment push_in_flight on entry and decrement on exit,
     * so once it reaches 0, no one holds buf_mutex. */
    for (int i = 0; i < 200 && s_log.push_in_flight.load(std::memory_order_acquire) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_log.push_in_flight.load(std::memory_order_acquire) > 0) {
        ESP_LOGW("logger", "deinit: %d push ops still in-flight after 1s, proceeding anyway",
                 s_log.push_in_flight.load());
    }

    /* Signal writer to stop and wake it */
    s_log.writer_running = false;
    if (s_log.data_sem) xSemaphoreGive(s_log.data_sem);

    /* Wait for writer task to finish draining and exit cleanly.
     * The writer sets writer_exited = true before vTaskDelete(NULL). */
    for (int i = 0; i < 100 && !s_log.writer_exited; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_log.writer_exited && s_log.writer_task) {
        /* Safety: force-kill only if writer didn't exit on its own.
         * Re-check after a short yield: the writer may have just set
         * writer_exited and called vTaskDelete(NULL) — the idle task
         * needs time to reclaim the TCB before we can safely reference
         * or skip the handle. */
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!s_log.writer_exited) {
            vTaskDelete(s_log.writer_task);
        }
    }
    s_log.writer_task = nullptr;

    /* Writer may have already closed fd in its clean exit path */
    if (s_log.fd) { fclose(s_log.fd); s_log.fd = nullptr; }

    if (s_log.buf_mutex) { vSemaphoreDelete(s_log.buf_mutex); s_log.buf_mutex = nullptr; }
    if (s_log.data_sem)  { vSemaphoreDelete(s_log.data_sem);  s_log.data_sem = nullptr; }
    free(s_log.buf);
    s_log.buf = nullptr;
}

void logger_set_sd_level(logger_level_t level)
{
    s_log.sd_level.store((int)level, std::memory_order_relaxed);
}

logger_level_t logger_get_sd_level(void)
{
    return (logger_level_t)s_log.sd_level.load(std::memory_order_relaxed);
}

bool logger_is_running(void)
{
    return s_log.running.load(std::memory_order_acquire);
}

const char *logger_get_filepath(void)
{
    return s_log.file_path;
}

/* ── esp_log_set_vprintf hook ──────────────────────────────────── */

/*
 * ESP-IDF v6.x inlines esp_log_write / esp_log_writev, so linker
 * --wrap cannot intercept them.  Instead we use esp_log_set_vprintf()
 * which is the official API for redirecting log output.
 *
 * The vprintf hook receives the fully-formatted log line (with ANSI
 * codes, level char, timestamp, and tag already embedded).  We call
 * the original vprintf for UART, then parse + re-format for the SD
 * card ring buffer.
 */
static int _logger_vprintf(const char *format, va_list args)
{
    /* 1. UART output via the original vprintf */
    int ret = 0;
    /* Read orig_vprintf into a local before the running check, so even if
     * logger_deinit() restores it between our check and the call, we use a
     * consistent pointer. */
    vprintf_like_t orig_vprintf = s_log.orig_vprintf;
    if (orig_vprintf) {
        va_list uart_args;
        va_copy(uart_args, args);
        ret = orig_vprintf(format, uart_args);
        va_end(uart_args);
    }

    /* SD card: skip if not running (atomic load — synchronizes with deinit store) */
    if (!s_log.running.load(std::memory_order_acquire)) return ret;

    /* Re-entrancy guard: if we are called from the writer task
     * context (e.g. FAT/SDMMC driver logs an ESP_LOGE during fputc
     * or _rotate_file), skip _logger_push to avoid self-deadlock.
     * The UART output above still works — only SD capture is skipped. */
    if (s_log.writer_task &&
        xTaskGetCurrentTaskHandle() == s_log.writer_task) {
        return ret;
    }

    /* ESP-IDF LOG_VERSION=1 passes the vprintf hook the full format string
     * with header ("I (%u) %s: format\n") and ANSI color codes, plus the
     * actual args (timestamp, tag, user args).  We must call vsnprintf to
     * substitute the format specifiers with their real values — strlcpy
     * would copy the format string literally (e.g. "%u" and "%s" would
     * appear verbatim in the log file). */
    char raw[MAX_LINE_LEN];
    va_list sd_args;
    va_copy(sd_args, args);
    int raw_len = vsnprintf(raw, sizeof(raw), format, sd_args);
    va_end(sd_args);
    if (raw_len < 0) return ret;
    if (raw_len >= (int)sizeof(raw)) raw_len = (int)sizeof(raw) - 1;

    int clean_len = _strip_ansi(raw, raw_len);
    while (clean_len > 0 && (raw[clean_len - 1] == '\n' ||
                              raw[clean_len - 1] == '\r'))
        raw[--clean_len] = '\0';

    /* Parse level from the cleaned line.  ESP_LOG v1 format:
     *   " E (12345) Camera: message"
     *   " W (12345) Camera: message"
     *   " I (12345) Camera: message"
     *   " D (12345) Camera: message"
     * Level char is at raw[1] (raw[0] is a space).
     *
     * NOTE: This parsing relies on CONFIG_LOG_VERSION=1 format.
     * If LOG_VERSION=2 is used, the format differs and level
     * detection will default to INFO (level=3). */
    int level = 3;  /* default INFO */
    if (clean_len >= 2 && raw[0] == ' ') {
        switch (raw[1]) {
            case 'E': level = 1; break;
            case 'W': level = 2; break;
            case 'I': level = 3; break;
            case 'D': level = 4; break;
            case 'V': level = 5; break;
        }
    }

    /* Level filter */
    if (level > s_log.sd_level.load(std::memory_order_relaxed)) return ret;

    /* Timestamp prefix */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char time_str[64];
    if (tv.tv_sec > 100000000) {
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        snprintf(time_str, sizeof(time_str),
                 "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 (int)(tv.tv_usec / 1000));
    } else {
        snprintf(time_str, sizeof(time_str), "boot+%lu.%03us",
                 (unsigned long)tv.tv_sec,
                 (unsigned int)(tv.tv_usec / 1000));
    }

    /* Build final line: [timestamp] log_line */
    char line[MAX_LINE_LEN];
    int len = snprintf(line, sizeof(line), "[%s] %s\n",
                       time_str, clean_len > 0 ? raw : "");
    if (len > 0) {
        if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
        _logger_push(line, len);
    }

    return ret;
}

/* ── Ring buffer producer ──────────────────────────────────────── */

static void _logger_push(const char *data, int len)
{
    if (len <= 0) return;
    SemaphoreHandle_t mtx = s_log.buf_mutex;
    if (!mtx || !s_log.buf) return;

    /* Mark in-flight before taking mutex — deinit() waits for this counter
     * to reach 0 before deleting buf_mutex, ensuring our mtx is valid. */
    s_log.push_in_flight.fetch_add(1, std::memory_order_acq_rel);
    /* Re-read mutex after incrementing counter */
    mtx = s_log.buf_mutex;
    if (!mtx) {
        s_log.push_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    /* Take mutex — check return in case mutex was deleted while we were blocked.
     * (logger_deinit restores vprintf first, so new _logger_vprintf calls are
     *  prevented, but an in-flight call already past the running check could
     *  still reach here.) */
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_log.push_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    /* Re-validate: buf may have been freed by deinit while we waited */
    if (!s_log.buf || !s_log.buf_mutex) {
        xSemaphoreGive(mtx);
        s_log.push_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    int written = 0;
    while (written < len) {
        /* Available space = buf_size - 1 (keep one slot empty to distinguish full/empty) */
        size_t avail = (s_log.buf_size - 1) - 
                       ((s_log.head + s_log.buf_size - s_log.tail) % s_log.buf_size);

        size_t to_write = (avail >= (size_t)(len - written)) ? (size_t)(len - written) : avail;
        if (to_write == 0) {
            /* Buffer completely full — drop entire remaining data */
            break;
        }

        /* Wrap-aware memcpy: up to two chunks if data wraps around buffer end */
        size_t first_chunk = s_log.buf_size - s_log.head;
        if (first_chunk > to_write) first_chunk = to_write;
        memcpy(&s_log.buf[s_log.head], &data[written], first_chunk);
        written += first_chunk;
        s_log.head = (s_log.head + first_chunk) % s_log.buf_size;

        if (first_chunk < to_write) {
            /* Second chunk wraps to beginning of buffer */
            size_t second_chunk = to_write - first_chunk;
            memcpy(&s_log.buf[0], &data[written], second_chunk);
            written += second_chunk;
            s_log.head = second_chunk;
        }

        if (to_write < (size_t)(len - written)) {
            /* Partial write: filled available space, drop remainder.
             * Better to lose a log line than freeze a time-critical task. */
            break;
        }
    }

    xSemaphoreGive(mtx);
    xSemaphoreGive(s_log.data_sem);
    s_log.push_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

/* ── Writer task ────────────────────────────────────────────────── */

static void _logger_writer_task(void *arg)
{
    (void)arg;

    while (s_log.writer_running) {
        if (xSemaphoreTake(s_log.data_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (s_log.fd) { fflush(s_log.fd); fsync(fileno(s_log.fd)); }
            if (!s_log.writer_running) break;
            continue;
        }

        /* Drain the ring buffer in batches */
        while (true) {
            uint8_t local[512];
            int n = 0;

            xSemaphoreTake(s_log.buf_mutex, portMAX_DELAY);

            /* Wrap-aware read from ring buffer */
            while (s_log.tail != s_log.head && n < (int)sizeof(local)) {
                size_t contiguous = (s_log.head >= s_log.tail)
                    ? (s_log.head - s_log.tail)
                    : (s_log.buf_size - s_log.tail);
                size_t room = sizeof(local) - n;
                size_t chunk = (contiguous > room) ? room : contiguous;
                memcpy(&local[n], &s_log.buf[s_log.tail], chunk);
                n += chunk;
                s_log.tail = (s_log.tail + chunk) % s_log.buf_size;
            }

            xSemaphoreGive(s_log.buf_mutex);

            if (n == 0) break;

            if (s_log.fd) {
                fwrite(local, 1, n, s_log.fd);
                s_log.bytes_written += n;
            }

            if (s_log.bytes_written >=
                (size_t)CONFIG_APP_LOG_MAX_FILE_SIZE_KB * 1024) {
                _rotate_file();
            }
        }
        if (s_log.fd) { fflush(s_log.fd); fsync(fileno(s_log.fd)); }
    }

    /* Clean exit: drain remaining data.
     * Copy under mutex, write outside mutex to avoid orphaning it
     * if deinit force-kills the task during blocking I/O. */
    uint8_t *drain = (uint8_t *)malloc(s_log.buf_size);
    size_t drain_n = 0;
    if (drain) {
        xSemaphoreTake(s_log.buf_mutex, portMAX_DELAY);
        if (s_log.tail != s_log.head) {
            size_t avail = (s_log.head >= s_log.tail)
                ? (s_log.head - s_log.tail)
                : (s_log.buf_size - s_log.tail);
            memcpy(drain, &s_log.buf[s_log.tail], avail);
            drain_n = avail;
            if (s_log.head < s_log.tail) {
                memcpy(drain + drain_n, s_log.buf, s_log.head);
                drain_n += s_log.head;
            }
        }
        xSemaphoreGive(s_log.buf_mutex);
        if (s_log.fd && drain_n > 0) {
            fwrite(drain, 1, drain_n, s_log.fd);
        }
        free(drain);
    }

    if (s_log.fd) {
        fflush(s_log.fd);
        fsync(fileno(s_log.fd));
        fclose(s_log.fd);
        s_log.fd = nullptr;
    }
    s_log.writer_exited = true;
    s_log.writer_task = nullptr;
    vTaskDelete(NULL);
}

/* ── File management ────────────────────────────────────────────── */

/** Scan /sdcard/logs/ for app_NNNNNN.log files, return max index. */
static int _find_max_index(void)
{
    int max_idx = 0;
    DIR *d = opendir(s_log.dir_path);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int idx = 0;
        if (sscanf(ent->d_name, LOG_FILE_PREFIX "%6d" LOG_FILE_SUFFIX, &idx) == 1) {
            if (idx > max_idx) max_idx = idx;
        }
    }
    closedir(d);
    return max_idx;
}

static void _rotate_file(void)
{
    if (s_log.fd) {
        fflush(s_log.fd);
        fsync(fileno(s_log.fd));
        fclose(s_log.fd);
        s_log.fd = nullptr;
    }

    /* Index-based naming: app_000001.log, app_000002.log ...
     * Independent of system clock — survives reboots without RTC. */
    int idx = _find_max_index() + 1;
    snprintf(s_log.file_path, sizeof(s_log.file_path),
             "%s/" LOG_FILE_PREFIX "%06d" LOG_FILE_SUFFIX,
             s_log.dir_path, idx);

    s_log.fd = fopen(s_log.file_path, "w");
    s_log.bytes_written = 0;

    if (!s_log.fd) return;

    /* Write header with real timestamp if available, otherwise
     * just note it's a new session. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 100000000) {  /* reasonable date: > 1973 */
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        fprintf(s_log.fd,
            "=== Log session #%d started at %04d-%02d-%02d %02d:%02d:%02d ===\n",
            idx, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        fprintf(s_log.fd, "=== Log session #%d started (clock not set) ===\n", idx);
    }
    /* Write git version info to log header */
    fprintf(s_log.fd, "Git branch:  %s\n", GIT_BRANCH);
    fprintf(s_log.fd, "Git commit:  %s\n", GIT_COMMIT);
    fprintf(s_log.fd, "Git author:  %s\n", GIT_AUTHOR);
    fprintf(s_log.fd, "Git date:    %s\n", GIT_DATE);
    fprintf(s_log.fd, "Git message: %s\n", GIT_MSG);
    fflush(s_log.fd);
    fsync(fileno(s_log.fd));

    _cleanup_old_logs();
}

static void _cleanup_old_logs(void)
{
    int max_files = CONFIG_APP_LOG_MAX_FILES;
    if (max_files <= 0) return;

    /* Collect all indices, sort, delete oldest.
     * Dynamic allocation since max_files can be up to 100 and there
     * may be stale files from prior runs beyond that. */
    int capacity = 64;
    int *indices = (int *)malloc(capacity * sizeof(int));
    if (!indices) return;
    int count = 0;

    DIR *d = opendir(s_log.dir_path);
    if (!d) { free(indices); return; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int idx = 0;
        if (sscanf(ent->d_name, LOG_FILE_PREFIX "%6d" LOG_FILE_SUFFIX, &idx) == 1) {
            if (count >= capacity) {
                capacity *= 2;
                int *tmp = (int *)realloc(indices, capacity * sizeof(int));
                if (!tmp) break;
                indices = tmp;
            }
            indices[count++] = idx;
        }
    }
    closedir(d);

    if (count <= max_files) { free(indices); return; }

    /* Simple insertion sort (count is small) */
    for (int i = 1; i < count; i++) {
        int key = indices[i];
        int j = i - 1;
        while (j >= 0 && indices[j] > key) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }

    /* Delete oldest files beyond the limit */
    int to_delete = count - max_files;
    for (int i = 0; i < to_delete; i++) {
        char path[192];
        snprintf(path, sizeof(path), "%s/" LOG_FILE_PREFIX "%06d" LOG_FILE_SUFFIX,
                 s_log.dir_path, indices[i]);
        unlink(path);
    }
    free(indices);
}

/** Strip ANSI escape sequences in-place.  Returns the new length. */
static int _strip_ansi(char *str, int len)
{
    int w = 0;
    for (int r = 0; r < len; r++) {
        if (str[r] == '\033') {
            while (r < len && str[r] != 'm') r++;
            continue;
        }
        str[w++] = str[r];
    }
    if (w < len) str[w] = '\0';
    return w;
}
