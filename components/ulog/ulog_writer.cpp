/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ULogWriter implementation — uORB topic logging to SD card in ULog format.
 *
 * Architecture:
 *   ULogWriter singleton
 *     ├─ Topic registry (subscribed uORB topics + intervals)
 *     ├─ Ring buffer (lock-free circular byte buffer)
 *     └─ Writer task (poll → format → ring → flush → file)
 */

#include "ulog_writer.h"
#include "ulog_messages.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cstdlib>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <inttypes.h>
#include <atomic>

static const char *TAG = "ULog";

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** Maximum number of topic subscriptions. */
#define MAX_TOPICS  ULOG_MAX_TOPICS

/** Maximum characters in a file path. */
#define MAX_PATH    ULOG_MAX_PATH

/** Log root directory under the SD card mount point. */
#define LOG_DIR_NAME "data"

/** PX4-compatible file extension. */
#define ULOG_FILE_EXT ".ulg"

/** Maximum single log file size before rotation (bytes).
 *  Default 100 MB. When exceeded, current file is closed and
 *  a new one is created in the same directory. */
#ifndef ULOG_MAX_FILE_SIZE
#ifdef CONFIG_ULOG_MAX_FILE_SIZE
#define ULOG_MAX_FILE_SIZE  CONFIG_ULOG_MAX_FILE_SIZE
#else
#define ULOG_MAX_FILE_SIZE  (100 * 1024 * 1024)
#endif
#endif

/** Default max ULog storage as percentage of SD card capacity. */
#ifndef ULOG_MAX_CAPACITY_PCT
#ifdef CONFIG_ULOG_MAX_CAPACITY_PCT
#define ULOG_MAX_CAPACITY_PCT  CONFIG_ULOG_MAX_CAPACITY_PCT
#else
#define ULOG_MAX_CAPACITY_PCT  70
#endif
#endif

/** Minimum retained session files. */
#ifndef ULOG_MIN_KEEP_FILES
#ifdef CONFIG_ULOG_MIN_KEEP_FILES
#define ULOG_MIN_KEEP_FILES  CONFIG_ULOG_MIN_KEEP_FILES
#else
#define ULOG_MIN_KEEP_FILES  200
#endif
#endif

#define MS_TO_US(ms)  ((ms) * 1000ULL)

/* ------------------------------------------------------------------ */
/*  Ring buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buffer;
    size_t   size;
    std::atomic<size_t> write_pos;
    std::atomic<size_t> read_pos;
    SemaphoreHandle_t mutex;
} ringbuf_t;

static bool ringbuf_init(ringbuf_t *rb, size_t size)
{
    rb->buffer = (uint8_t *)malloc(size);
    if (!rb->buffer) return false;
    rb->size = size;
    rb->write_pos.store(0, std::memory_order_relaxed);
    rb->read_pos.store(0, std::memory_order_relaxed);
    rb->mutex = xSemaphoreCreateMutex();
    return rb->mutex != NULL;
}

static void ringbuf_deinit(ringbuf_t *rb)
{
    if (rb->mutex) vSemaphoreDelete(rb->mutex);
    free(rb->buffer);
    rb->buffer = nullptr;
    rb->size = 0;
    rb->write_pos.store(0, std::memory_order_relaxed);
    rb->read_pos.store(0, std::memory_order_relaxed);
    rb->mutex = nullptr;
}

static size_t ringbuf_available(const ringbuf_t *rb)
{
    size_t w = rb->write_pos.load(std::memory_order_relaxed);
    size_t r = rb->read_pos.load(std::memory_order_relaxed);
    if (w >= r) return w - r;
    return rb->size - (r - w);
}

static size_t ringbuf_free_space(const ringbuf_t *rb)
{
    /* Leave one byte gap to distinguish full vs empty */
    return rb->size - ringbuf_available(rb) - 1;
}

static bool ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    if (len == 0) return true;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    /* Check free space inside the mutex to prevent TOCTOU race
     * between concurrent producers. */
    if (len > ringbuf_free_space(rb)) {
        xSemaphoreGive(rb->mutex);
        return false;
    }

    size_t w = rb->write_pos.load(std::memory_order_relaxed);
    size_t end = w + len;

    if (end <= rb->size) {
        memcpy(rb->buffer + w, data, len);
    } else {
        size_t first = rb->size - w;
        memcpy(rb->buffer + w, data, first);
        memcpy(rb->buffer, data + first, len - first);
    }

    /* Ensure write_pos is updated atomically */
    rb->write_pos.store(end % rb->size, std::memory_order_release);

    xSemaphoreGive(rb->mutex);
    return true;
}

static size_t ringbuf_read(ringbuf_t *rb, uint8_t *dst, size_t max_len)
{
    size_t avail = ringbuf_available(rb);
    if (avail == 0) return 0;

    size_t to_read = (avail < max_len) ? avail : max_len;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t r = rb->read_pos.load(std::memory_order_relaxed);
    size_t end = r + to_read;

    if (end <= rb->size) {
        memcpy(dst, rb->buffer + r, to_read);
    } else {
        size_t first = rb->size - r;
        memcpy(dst, rb->buffer + r, first);
        memcpy(dst + first, rb->buffer, to_read - first);
    }

    rb->read_pos.store(end % rb->size, std::memory_order_release);

    xSemaphoreGive(rb->mutex);
    return to_read;
}

/* ------------------------------------------------------------------ */
/*  Topic subscription entry                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    orb_id_t     meta;          /**< Topic metadata */
    orb_sub_t    sub_handle;    /**< uORB subscriber handle */
    uint32_t     interval_ms;   /**< Minimum sampling interval */
    uint64_t     last_poll_us;  /**< Last poll timestamp (esp_timer) */
    uint16_t     msg_id;        /**< ULog-internal message ID */
    bool         active;        /**< True if this slot is in use */
} topic_entry_t;

/* ------------------------------------------------------------------ */
/*  ULogWriter instance                                                */
/* ------------------------------------------------------------------ */

typedef struct ulog_writer {
    /* Configuration */
    char sd_mount_path[ULOG_MAX_PATH];

    /* State */
    ulog_state_t state;

    /* File I/O */
    int          fd;                /**< Current log file descriptor, -1 if none */
    char         filepath[ULOG_MAX_PATH + 64]; /**< Current log file path */
    char         log_dir[ULOG_MAX_PATH + 32];  /**< Current log directory (date or session) */
    size_t       bytes_written;     /**< Total bytes written to current file */

    /* Topics */
    topic_entry_t topics[MAX_TOPICS];
    int           num_topics;
    uint16_t      next_msg_id;      /**< Monotonic msg_id counter */

    /* Ring buffer */
    ringbuf_t     ringbuf;

    /* Writer task */
    TaskHandle_t  task_handle;
    StackType_t  *task_stack;          /**< PSRAM-allocated stack (freed on stop) */
    StaticTask_t *task_tcb;            /**< Internal SRAM TCB (freed on stop) */
    std::atomic<bool> task_should_run{false};
    std::atomic<bool> task_exited{false};  /* Set before vTaskDelete by writer task */

    /* Timing */
    uint64_t      last_flush_us;
    uint64_t      last_sync_us;
    uint64_t      last_fsync_us;
    uint64_t      start_time_us;    /**< When logging started (esp_timer) */

    /* Naming mode */
    bool          has_wall_clock;   /**< True if SNTP has synced */
    uint16_t      session_counter;  /**< Boot session number (from NVS) */
    uint16_t      file_counter;     /**< File counter within current session/date dir */

    /* Info fields */
    char          sys_name[32];     /**< System name */
    char          ver_sw[32];       /**< Software version string */
    char          ver_hw[32];       /**< Hardware board name */
    char          sys_uuid[24];     /**< MAC-based unique ID */
    char          sys_os_name[16];  /**< OS name */
    char          sys_os_ver[32];   /**< OS version */
    char          sys_mcu[32];      /**< MCU name */
    char          arch[16];         /**< Architecture */
    ulog_git_info_t git_info;       /**< Git version info (from app) */
} ulog_writer_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void writer_task_func(void *arg);
static esp_err_t write_file_header(ulog_writer_t *writer);
static esp_err_t write_flag_bits(ulog_writer_t *writer);
static esp_err_t write_info_messages(ulog_writer_t *writer);
static esp_err_t write_format_messages(ulog_writer_t *writer);
static esp_err_t write_subscription_messages(ulog_writer_t *writer);
static void ensure_log_dir(const char *dir);
static esp_err_t create_log_path(ulog_writer_t *writer);
static void cleanup_old_logs(const char *log_root, size_t size_limit);
static esp_err_t rotate_log_file(ulog_writer_t *writer);

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

ulog_writer_t *ulog_writer_get(void)
{
    static ulog_writer_t s_instance;
    return &s_instance;
}

esp_err_t ulog_writer_init(ulog_writer_t *writer, const char *sd_mount_path,
                           const ulog_init_config_t *config)
{
    if (!writer || !sd_mount_path || !config) return ESP_ERR_INVALID_ARG;

    /* Zero-initialize ringbuf (contains std::atomic members, cannot memset).
     * Use explicit field initialization instead. */
    writer->ringbuf.buffer = nullptr;
    writer->ringbuf.size = 0;
    writer->ringbuf.write_pos.store(0, std::memory_order_relaxed);
    writer->ringbuf.read_pos.store(0, std::memory_order_relaxed);
    writer->ringbuf.mutex = nullptr;
    memset(writer->topics, 0, sizeof(writer->topics));
    writer->num_topics = 0;
    writer->next_msg_id = 0;
    writer->bytes_written = 0;
    writer->last_flush_us = 0;
    writer->last_sync_us = 0;
    writer->last_fsync_us = 0;
    writer->start_time_us = 0;
    writer->file_counter = 0;
    writer->filepath[0] = '\0';
    writer->log_dir[0] = '\0';
    writer->fd = -1;
    writer->state = ULOG_STATE_IDLE;
    writer->task_handle = NULL;
    writer->task_stack = nullptr;
    strlcpy(writer->sd_mount_path, sd_mount_path, sizeof(writer->sd_mount_path));

    /* Pre-allocate TCB in internal SRAM at init time.
     * TCB is small (~340B) and reused across start/stop cycles.
     * This avoids freeing the TCB while FreeRTOS idle task may still
     * reference it from xTasksWaitingTermination after vTaskDelete(NULL).
     * The stack is allocated separately from PSRAM on each start()
     * and freed on each stop() — it's large (8KB) and safe to free
     * immediately since the idle task doesn't reference the stack. */
    writer->task_tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!writer->task_tcb) {
        ESP_LOGE(TAG, "Failed to allocate writer TCB");
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }

    /* Copy config from application */
    writer->session_counter = config->session_counter;
    writer->has_wall_clock = config->has_wall_clock;
    strlcpy(writer->sys_name, config->sys_name, sizeof(writer->sys_name));
    strlcpy(writer->ver_sw, config->ver_sw, sizeof(writer->ver_sw));
    strlcpy(writer->ver_hw, config->ver_hw, sizeof(writer->ver_hw));
    strlcpy(writer->sys_uuid, config->sys_uuid, sizeof(writer->sys_uuid));
    strlcpy(writer->sys_os_name, config->sys_os_name, sizeof(writer->sys_os_name));
    strlcpy(writer->sys_os_ver, config->sys_os_ver, sizeof(writer->sys_os_ver));
    strlcpy(writer->sys_mcu, config->sys_mcu, sizeof(writer->sys_mcu));
    strlcpy(writer->arch, config->arch, sizeof(writer->arch));

    if (!ringbuf_init(&writer->ringbuf, ULOG_RINGBUF_SIZE)) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        writer->state = ULOG_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized (SD: %s, ringbuf: %u bytes, session: %u, RTC: %s)",
             sd_mount_path, ULOG_RINGBUF_SIZE, writer->session_counter,
             writer->has_wall_clock ? "yes" : "no");
    return ESP_OK;
}

void ulog_writer_set_git_info(ulog_writer_t *writer, const ulog_git_info_t *git_info)
{
    if (!writer || !git_info) return;
    writer->git_info = *git_info;
}

void ulog_writer_set_wall_clock(ulog_writer_t *writer, bool has_wall_clock)
{
    if (!writer) return;
    writer->has_wall_clock = has_wall_clock;
}

esp_err_t ulog_writer_add_topic(ulog_writer_t *writer, orb_id_t meta,
                                uint32_t interval_ms)
{
    if (!writer || !meta) return ESP_ERR_INVALID_ARG;
    if (writer->state == ULOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Cannot add topics while logging is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (writer->num_topics >= MAX_TOPICS) {
        ESP_LOGE(TAG, "Max topics reached (%d)", MAX_TOPICS);
        return ESP_ERR_NO_MEM;
    }

    topic_entry_t *entry = &writer->topics[writer->num_topics];
    entry->meta = meta;
    entry->interval_ms = (interval_ms > 0) ? interval_ms : ULOG_DEFAULT_INTERVAL_MS;
    entry->last_poll_us = 0;
    entry->msg_id = writer->next_msg_id++;
    entry->active = true;

    writer->num_topics++;

    ESP_LOGI(TAG, "Added topic %s (msg_id=%u, interval=%ums)",
             meta->o_name, entry->msg_id, entry->interval_ms);
    return ESP_OK;
}

esp_err_t ulog_writer_start(ulog_writer_t *writer)
{
    if (!writer) return ESP_ERR_INVALID_ARG;
    if (writer->state == ULOG_STATE_RUNNING) {
        ESP_LOGW(TAG, "Already logging");
        return ESP_OK;
    }
    if (writer->num_topics == 0) {
        ESP_LOGW(TAG, "No topics registered, nothing to log");
        return ESP_ERR_INVALID_STATE;
    }

    /* Re-check wall-clock time (SNTP may have synced since init) */
    struct timespec ts_now;
    if (clock_gettime(CLOCK_REALTIME, &ts_now) == 0) {
        writer->has_wall_clock = (uint64_t)ts_now.tv_sec > 1577836800ULL;
    }

    /* Ensure log root directory exists */
    char log_root[MAX_PATH + 8];
    snprintf(log_root, sizeof(log_root), "%s/%s", writer->sd_mount_path, LOG_DIR_NAME);
    ensure_log_dir(log_root);

    /* Compute ULog storage limit from SD card total capacity */
    size_t size_limit = 512 * 1024;  /* fallback: 512 KB */
    {
        struct statvfs vfs;
        if (statvfs(writer->sd_mount_path, &vfs) == 0) {
            uint64_t total = (uint64_t)vfs.f_frsize * vfs.f_blocks;
            size_limit = (size_t)(total * ULOG_MAX_CAPACITY_PCT / 100);
        }
    }

    /* Cleanup old logs if over capacity */
    cleanup_old_logs(log_root, size_limit);

    /* Create log directory and file path (date-based or session-based) */
    esp_err_t err = create_log_path(writer);
    if (err != ESP_OK) {
        writer->state = ULOG_STATE_ERROR;
        return err;
    }

    /* Open the file */
    writer->fd = open(writer->filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (writer->fd < 0) {
        ESP_LOGE(TAG, "Failed to create %s", writer->filepath);
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }

    /* Subscribe to all topics */
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        entry->sub_handle = orb_subscribe(entry->meta);
        if (entry->sub_handle < 0) {
            ESP_LOGW(TAG, "Failed to subscribe to %s", entry->meta->o_name);
        }
    }

    writer->bytes_written = 0;
    writer->start_time_us = esp_timer_get_time();
    writer->last_flush_us = writer->start_time_us;
    writer->last_sync_us  = writer->start_time_us;
    writer->last_fsync_us = writer->start_time_us;

    /* Write file header + definition section */
    err = write_file_header(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_flag_bits(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_info_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_format_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    err = write_subscription_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; writer->state = ULOG_STATE_ERROR; return err; }

    /* Everything from now on goes through the ring buffer (non-reliable) */

    /* Create the writer task.
     * Use xTaskCreateStatic with PSRAM-allocated stack to conserve
     * internal SRAM — xTaskCreate would allocate the 8KB stack from
     * scarce internal memory, which fails when LVGL + LWIP consume
     * most of it. PSRAM on ESP32-P4 is DMA-capable and suitable for
     * task stacks.
     * TCB was pre-allocated in init() and reused across start/stop cycles. */
#ifndef CONFIG_ULOG_TASK_STACK_SIZE
#define CONFIG_ULOG_TASK_STACK_SIZE 8192
#endif
    writer->task_exited.store(false, std::memory_order_release);
    /* xTaskCreateStaticPinnedToCore depth (CONFIG_ULOG_TASK_STACK_SIZE) is in
     * StackType_t words; buffer must be depth * sizeof(StackType_t) bytes. */
    writer->task_stack = (StackType_t *)heap_caps_malloc(
        CONFIG_ULOG_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!writer->task_stack) {
        ESP_LOGE(TAG, "Failed to allocate writer task stack");
        close(writer->fd);
        writer->fd = -1;
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }
    writer->task_should_run.store(true, std::memory_order_release);
    writer->task_handle = xTaskCreateStaticPinnedToCore(
        writer_task_func,
        "ulog_writer",
        CONFIG_ULOG_TASK_STACK_SIZE,
        writer,      /* arg */
        5,           /* priority */
        writer->task_stack,
        writer->task_tcb,
        0            /* Core 0 (never preempt Music ASP on Core 1) */
    );
    if (writer->task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create writer task");
        heap_caps_free(writer->task_stack); writer->task_stack = nullptr;
        close(writer->fd);
        writer->fd = -1;
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }

    writer->state = ULOG_STATE_RUNNING;
    ESP_LOGI(TAG, "Logging started: %s (%d topics, %s mode)",
             writer->filepath, writer->num_topics,
             writer->has_wall_clock ? "date" : "session");
    return ESP_OK;
}

esp_err_t ulog_writer_stop(ulog_writer_t *writer)
{
    if (!writer) return ESP_ERR_INVALID_ARG;
    /* Allow stop() to clean up subscriptions even after a failed start().
     * Only skip if already idle (fully stopped). */
    if (writer->state == ULOG_STATE_IDLE) {
        return ESP_OK;
    }

    /* Signal writer task to stop gracefully.
     * The writer task polls task_should_run and self-deletes.
     * We wait for it to set task_exited before proceeding.
     * Force-killing a task while it holds the ring buffer mutex
     * would permanently deadlock subsequent ringbuf_read calls. */
    writer->task_should_run.store(false, std::memory_order_release);
    if (writer->task_handle) {
        /* Wait for graceful exit (up to 2s) */
        for (int i = 0; i < 200 && !writer->task_exited.load(std::memory_order_acquire); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!writer->task_exited.load(std::memory_order_acquire)) {
            /* Last resort: force-kill. The ring buffer mutex may be permanently
             * locked — we can no longer safely drain it. */
            ESP_LOGW(TAG, "Writer task did not exit gracefully, force-killing");
            vTaskDelete(writer->task_handle);
        }
        writer->task_handle = NULL;
    }

    /* Free PSRAM-allocated stack (xTaskCreateStatic does not free it).
     * The stack is safe to free immediately since FreeRTOS doesn't
     * reference it after vTaskDelete — the idle task only reclaims
     * the TCB, which we keep allocated across stop/start cycles.
     * Yield first to ensure the task has fully exited. */
    vTaskDelay(pdMS_TO_TICKS(10));
    if (writer->task_stack) { heap_caps_free(writer->task_stack); writer->task_stack = nullptr; }

    /* Drain remaining ring buffer data to file.
     * Skip if writer was force-killed (mutex may be locked). */
    if (writer->task_exited.load(std::memory_order_acquire)) {
        uint8_t *drain_buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        bool drain_is_caps = (drain_buf != nullptr);
        if (!drain_buf) drain_buf = (uint8_t *)malloc(4096);
        if (drain_buf) {
            size_t n;
            while ((n = ringbuf_read(&writer->ringbuf, drain_buf, 4096)) > 0) {
                write(writer->fd, drain_buf, n);
                writer->bytes_written += n;
            }
            if (drain_is_caps) {
                heap_caps_free(drain_buf);
            } else {
                free(drain_buf);
            }
        }
    }

    /* Close the file */
    if (writer->fd >= 0) {
        fsync(writer->fd);
        close(writer->fd);
        writer->fd = -1;
    }

    /* Unsubscribe */
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        if (entry->sub_handle >= 0) {
            orb_unsubscribe(entry->sub_handle);
            entry->sub_handle = -1;
        }
    }

    /* Report statistics */
    uint64_t elapsed_us = esp_timer_get_time() - writer->start_time_us;
    uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
    double rate = (elapsed_ms > 0)
        ? (double)writer->bytes_written * 1000.0 / (double)elapsed_ms
        : 0.0;

    ESP_LOGI(TAG, "Logging stopped: %s (%u bytes in %ums, %.1f B/s)",
             writer->filepath, (unsigned)writer->bytes_written,
             elapsed_ms, rate);

    writer->state = ULOG_STATE_IDLE;
    return ESP_OK;
}

ulog_state_t ulog_writer_get_state(const ulog_writer_t *writer)
{
    return writer ? writer->state : ULOG_STATE_UNINIT;
}

const char *ulog_writer_get_filepath(const ulog_writer_t *writer)
{
    return writer ? writer->filepath : "";
}

size_t ulog_writer_get_bytes_written(const ulog_writer_t *writer)
{
    return writer ? writer->bytes_written : 0;
}

esp_err_t ulog_writer_write_message(ulog_writer_t *writer, uint8_t level,
                                    const char *message)
{
    if (!writer || writer->state != ULOG_STATE_RUNNING) return ESP_ERR_INVALID_STATE;

    size_t msg_len = strlen(message);
    if (msg_len > 127) msg_len = 127;

    ulog_message_logging_s msg;
    size_t total_size = ULOG_MSG_HEADER_LEN + 9 + msg_len; /* no NUL terminator per ULog spec */
    msg.msg_size = (uint16_t)(total_size - ULOG_MSG_HEADER_LEN);
    msg.msg_type = ULOG_MSG_TYPE_LOGGING;
    msg.log_level = level;
    msg.timestamp = esp_timer_get_time();
    memcpy(msg.message, message, msg_len); /* no NUL terminator per ULog spec */

    if (!ringbuf_write(&writer->ringbuf, (const uint8_t *)&msg,
                       ULOG_MSG_HEADER_LEN + 9 + msg_len)) {
        ESP_LOGW(TAG, "Ring buffer full, dropping log message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ulog_writer_deinit(ulog_writer_t *writer)
{
    if (!writer) return;
    if (writer->state == ULOG_STATE_RUNNING) {
        ulog_writer_stop(writer);
    }
    ringbuf_deinit(&writer->ringbuf);
    /* TCB is pre-allocated at init and never freed — ~340B internal SRAM,
     * negligible cost for a permanent singleton in embedded firmware.
     * Avoids TCB use-after-free race with idle task entirely. */
    writer->state = ULOG_STATE_UNINIT;
    ESP_LOGI(TAG, "Deinitialized");
}

/* ------------------------------------------------------------------ */
/*  Writer task                                                        */
/* ------------------------------------------------------------------ */

static void writer_task_func(void *arg)
{
    ulog_writer_t *writer = (ulog_writer_t *)arg;

    /* Determine the maximum topic payload size across all registered topics.
     * This determines the scratch buffer size for formatting DATA messages.
     * Layout: 3 (header) + 2 (msg_id) + payload_size.
     * For camera_frame, payload is ~8203 bytes, so we allocate from PSRAM heap.
     * Note: buffer must be o_size (full struct) because orb_copy writes sizeof(struct). */
    size_t max_payload = 64;  /* minimum reasonable size */
    for (int i = 0; i < writer->num_topics; i++) {
        if (writer->topics[i].meta && writer->topics[i].meta->o_size > max_payload) {
            max_payload = writer->topics[i].meta->o_size;
        }
    }
    size_t data_buf_size = ULOG_MSG_HEADER_LEN + sizeof(uint16_t) + max_payload;
    uint8_t *data_buf = (uint8_t *)heap_caps_malloc(data_buf_size, MALLOC_CAP_SPIRAM);
    bool data_buf_is_caps = (data_buf != nullptr);
    if (!data_buf) {
        /* Fallback to internal RAM (may fail for very large topics) */
        data_buf = (uint8_t *)malloc(data_buf_size);
    }
    if (!data_buf) {
        ESP_LOGE(TAG, "Failed to allocate data_buf (%u bytes)", (unsigned)data_buf_size);
        writer->task_should_run.store(false, std::memory_order_release);
        writer->task_exited.store(true, std::memory_order_release);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "data_buf: %u bytes (%s)", (unsigned)data_buf_size,
             esp_ptr_external_ram(data_buf) ? "PSRAM" : "internal");

    uint8_t flush_buf[4096];

    while (writer->task_should_run.load(std::memory_order_acquire)) {
        uint64_t now_us = esp_timer_get_time();

        /* ── Poll each topic ── */
        for (int i = 0; i < writer->num_topics; i++) {
            topic_entry_t *entry = &writer->topics[i];
            if (!entry->active || entry->sub_handle < 0) continue;

            uint64_t interval_us = MS_TO_US(entry->interval_ms);
            if ((now_us - entry->last_poll_us) < interval_us) continue;
            entry->last_poll_us = now_us;

            bool updated = false;
            if (orb_check(entry->sub_handle, &updated) != 0 || !updated) continue;

            /* Copy the topic data */
            void *payload = data_buf + ULOG_MSG_HEADER_LEN + sizeof(uint16_t);
            if (orb_copy(entry->meta, entry->sub_handle, payload) != 0) continue;

            /* Build ULog DATA message header.
             * Use o_size_no_padding (PX4 convention): write only the data
             * fields without trailing _padding bytes. This ensures pyulog's
             * max_data_size (which strips trailing _padding) matches the
             * actual DATA message size, avoiding false data corruption. */
            uint16_t payload_size = (uint16_t)entry->meta->o_size_no_padding;
            uint16_t msg_total = (uint16_t)(sizeof(uint16_t) + payload_size); /* excl. 3-byte header */

            data_buf[0] = (uint8_t)(msg_total & 0xFF);
            data_buf[1] = (uint8_t)((msg_total >> 8) & 0xFF);
            data_buf[2] = ULOG_MSG_TYPE_DATA;
            data_buf[3] = (uint8_t)(entry->msg_id & 0xFF);
            data_buf[4] = (uint8_t)((entry->msg_id >> 8) & 0xFF);

            size_t total = ULOG_MSG_HEADER_LEN + msg_total;

            if (!ringbuf_write(&writer->ringbuf, data_buf, total)) {
                ESP_LOGW(TAG, "Ring buffer full, dropping %s data", entry->meta->o_name);
            }
        }

        /* ── Periodic flush to file ── */
        uint64_t flush_interval_us = MS_TO_US(ULOG_FLUSH_INTERVAL_MS);
        if ((now_us - writer->last_flush_us) >= flush_interval_us) {
            writer->last_flush_us = now_us;

            /* Check if it's time for a sync marker */
            uint64_t sync_interval_us = MS_TO_US(ULOG_SYNC_INTERVAL_MS);
            if ((now_us - writer->last_sync_us) >= sync_interval_us) {
                writer->last_sync_us = now_us;
                /* Insert sync marker into the ring buffer (so it's in data section) */
                uint8_t sync_buf[ULOG_MSG_HEADER_LEN + 8];
                sync_buf[0] = 8;  /* msg_size = 8 */
                sync_buf[1] = 0;
                sync_buf[2] = ULOG_MSG_TYPE_SYNC;
                const uint8_t sync_magic[8] = ULOG_SYNC_MAGIC;
                memcpy(sync_buf + 3, sync_magic, 8);
                ringbuf_write(&writer->ringbuf, sync_buf, sizeof(sync_buf));
            }

            /* Drain ring buffer to file */
            size_t total_flushed = 0;
            size_t n;
            while ((n = ringbuf_read(&writer->ringbuf, flush_buf, sizeof(flush_buf))) > 0) {
                ssize_t written = write(writer->fd, flush_buf, n);
                if (written > 0) {
                    writer->bytes_written += (size_t)written;
                    total_flushed += (size_t)written;
                } else {
                    ESP_LOGE(TAG, "File write error");
                    break;
                }
            }

            /* Periodic fsync */
            uint64_t fsync_interval_us = MS_TO_US(ULOG_FSYNC_INTERVAL_MS);
            if ((now_us - writer->last_fsync_us) >= fsync_interval_us) {
                writer->last_fsync_us = now_us;
                fsync(writer->fd);
            }

            /* File size rotation check */
            if (writer->bytes_written >= ULOG_MAX_FILE_SIZE) {
                esp_err_t rot_err = rotate_log_file(writer);
                if (rot_err != ESP_OK) {
                    ESP_LOGE(TAG, "File rotation failed, stopping logger");
                    break;
                }
            }

            if (total_flushed > 0) {
                ESP_LOGV(TAG, "Flushed %u bytes to %s",
                         (unsigned)total_flushed, writer->filepath);
            }
        }

        /* Sleep a short time */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Free heap-allocated data buffer before task exit */
    if (data_buf_is_caps) {
        heap_caps_free(data_buf);
    } else {
        free(data_buf);
    }

    /* Task self-delete — set exited flag first for clean teardown */
    writer->task_exited.store(true, std::memory_order_release);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  File I/O helpers — write individual ULog sections                  */
/* ------------------------------------------------------------------ */

static esp_err_t write_all(ulog_writer_t *writer, const void *data, size_t len)
{
    if (writer->fd < 0) return ESP_ERR_INVALID_STATE;
    const uint8_t *ptr = (const uint8_t *)data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(writer->fd, ptr, remaining);
        if (n <= 0) {
            ESP_LOGE(TAG, "Write error: errno=%d", errno);
            return ESP_FAIL;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    writer->bytes_written += len;
    return ESP_OK;
}

/** Write file header: magic + timestamp. */
static esp_err_t write_file_header(ulog_writer_t *writer)
{
    ulog_file_header_s hdr;
    memcpy(hdr.magic, ULOG_MAGIC, ULOG_MAGIC_LEN);

    /* File header timestamp must be in the same time domain as data message
     * timestamps — microseconds since boot (esp_timer).
     * Tools like ulog_info convert to UTC using boot_time_utc_us. */
    hdr.timestamp = esp_timer_get_time();
    return write_all(writer, &hdr, sizeof(hdr));
}

/** Write flag bits message. */
static esp_err_t write_flag_bits(ulog_writer_t *writer)
{
    ulog_message_flag_bits_s msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_size = sizeof(msg) - ULOG_MSG_HEADER_LEN;
    msg.msg_type = ULOG_MSG_TYPE_FLAG_BITS;
    /* No special flags set */
    return write_all(writer, &msg, sizeof(msg));
}

/** Write info messages: sys_name, version, hardware, etc. (PX4 convention) */
static esp_err_t write_info_messages(ulog_writer_t *writer)
{
    /* Per PX4 ULog spec, INFO ('I') message format:
     *   [msg_size(2)] [msg_type='I'(1)] [key_len(1)] [key(type+name, key_len bytes)] [binary_value]
     * key = "type key_name" (no NUL), e.g. "uint32_t ver_data_format"
     * value = binary LE matching the type, e.g. uint32_t → 4 bytes LE
     * For strings: key = "char[N] key_name", value = string bytes (NUL-terminated) */

    esp_err_t err;

    /* ── char[N] string values ── */
    auto write_info_str = [writer](const char *key, const char *value) -> esp_err_t {
        size_t val_len = strlen(value);
        char key_buf[256];
        int key_n = snprintf(key_buf, sizeof(key_buf), "char[%d] %s", (int)val_len, key);
        if (key_n < 0 || (size_t)key_n >= sizeof(key_buf)) return ESP_ERR_INVALID_SIZE;
        uint8_t key_len = (uint8_t)key_n;

        uint16_t payload = (uint16_t)(1 + key_len + (uint16_t)val_len); /* no NUL in value per ULog spec */
        uint8_t msg[512];
        size_t pos = 0;
        msg[pos++] = (uint8_t)(payload & 0xFF);
        msg[pos++] = (uint8_t)((payload >> 8) & 0xFF);
        msg[pos++] = ULOG_MSG_TYPE_INFO;
        msg[pos++] = key_len;
        memcpy(msg + pos, key_buf, (size_t)key_n);
        pos += (size_t)key_n;
        memcpy(msg + pos, value, val_len); /* no NUL terminator per ULog spec */
        pos += val_len;
        return write_all(writer, msg, pos);
    };

    /* ── uint32_t values (4 bytes LE) ── */
    auto write_info_u32 = [writer](const char *key, uint32_t value) -> esp_err_t {
        char key_buf[256];
        int key_n = snprintf(key_buf, sizeof(key_buf), "uint32_t %s", key);
        if (key_n < 0 || (size_t)key_n >= sizeof(key_buf)) return ESP_ERR_INVALID_SIZE;
        uint8_t key_len = (uint8_t)key_n;

        uint16_t payload = (uint16_t)(1 + key_len + 4);
        uint8_t msg[256];
        size_t pos = 0;
        msg[pos++] = (uint8_t)(payload & 0xFF);
        msg[pos++] = (uint8_t)((payload >> 8) & 0xFF);
        msg[pos++] = ULOG_MSG_TYPE_INFO;
        msg[pos++] = key_len;
        memcpy(msg + pos, key_buf, (size_t)key_n);
        pos += (size_t)key_n;
        msg[pos++] = (uint8_t)(value & 0xFF);
        msg[pos++] = (uint8_t)((value >> 8) & 0xFF);
        msg[pos++] = (uint8_t)((value >> 16) & 0xFF);
        msg[pos++] = (uint8_t)((value >> 24) & 0xFF);
        return write_all(writer, msg, pos);
    };

    /* ── int32_t values (4 bytes LE) ── */
    auto write_info_i32 = [writer](const char *key, int32_t value) -> esp_err_t {
        char key_buf[256];
        int key_n = snprintf(key_buf, sizeof(key_buf), "int32_t %s", key);
        if (key_n < 0 || (size_t)key_n >= sizeof(key_buf)) return ESP_ERR_INVALID_SIZE;
        uint8_t key_len = (uint8_t)key_n;

        uint16_t payload = (uint16_t)(1 + key_len + 4);
        uint8_t msg[256];
        size_t pos = 0;
        msg[pos++] = (uint8_t)(payload & 0xFF);
        msg[pos++] = (uint8_t)((payload >> 8) & 0xFF);
        msg[pos++] = ULOG_MSG_TYPE_INFO;
        msg[pos++] = key_len;
        memcpy(msg + pos, key_buf, (size_t)key_n);
        pos += (size_t)key_n;
        uint32_t uv = (uint32_t)value;
        msg[pos++] = (uint8_t)(uv & 0xFF);
        msg[pos++] = (uint8_t)((uv >> 8) & 0xFF);
        msg[pos++] = (uint8_t)((uv >> 16) & 0xFF);
        msg[pos++] = (uint8_t)((uv >> 24) & 0xFF);
        return write_all(writer, msg, pos);
    };

    /* PX4 standard Info keys */
    err = write_info_str("sys_name", writer->sys_name);
    if (err != ESP_OK) return err;
    err = write_info_str("ver_sw", writer->ver_sw);
    if (err != ESP_OK) return err;
    err = write_info_str("ver_hw", writer->ver_hw);
    if (err != ESP_OK) return err;
    err = write_info_str("sys_os_name", writer->sys_os_name);
    if (err != ESP_OK) return err;
    err = write_info_str("sys_os_ver", writer->sys_os_ver);
    if (err != ESP_OK) return err;
    err = write_info_str("sys_mcu", writer->sys_mcu);
    if (err != ESP_OK) return err;
    err = write_info_str("sys_uuid", writer->sys_uuid);
    if (err != ESP_OK) return err;
    err = write_info_str("arch", writer->arch);
    if (err != ESP_OK) return err;

    /* Git version info */
    if (writer->git_info.branch[0] != '\0') {
        err = write_info_str("ver_sw_branch", writer->git_info.branch);
        if (err != ESP_OK) return err;
    }
    if (writer->git_info.commit[0] != '\0') {
        err = write_info_str("ver_sw_commit", writer->git_info.commit);
        if (err != ESP_OK) return err;
    }
    if (writer->git_info.author[0] != '\0') {
        err = write_info_str("ver_sw_author", writer->git_info.author);
        if (err != ESP_OK) return err;
    }
    if (writer->git_info.date[0] != '\0') {
        err = write_info_str("ver_sw_date", writer->git_info.date);
        if (err != ESP_OK) return err;
    }
    if (writer->git_info.message[0] != '\0') {
        err = write_info_str("ver_sw_msg", writer->git_info.message);
        if (err != ESP_OK) return err;
    }

    /* ULog data format version (PX4 uses 2) */
    err = write_info_u32("ver_data_format", 2);
    if (err != ESP_OK) return err;

    /* Boot time in UTC µs + time_ref_utc (only if wall clock is synced) */
    if (writer->has_wall_clock) {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            uint64_t boot_utc_us = (uint64_t)ts.tv_sec * 1000000ULL +
                                   (uint64_t)ts.tv_nsec / 1000ULL;
            boot_utc_us -= esp_timer_get_time();

            char key_buf[64];
            int key_n = snprintf(key_buf, sizeof(key_buf), "uint64_t %s", "boot_time_utc_us");
            uint8_t key_len = (uint8_t)key_n;
            uint16_t payload = (uint16_t)(1 + key_len + 8);
            uint8_t msg[128];
            size_t pos = 0;
            msg[pos++] = (uint8_t)(payload & 0xFF);
            msg[pos++] = (uint8_t)((payload >> 8) & 0xFF);
            msg[pos++] = ULOG_MSG_TYPE_INFO;
            msg[pos++] = key_len;
            memcpy(msg + pos, key_buf, (size_t)key_n);
            pos += (size_t)key_n;
            /* uint64_t LE */
            for (int i = 0; i < 8; i++) {
                msg[pos++] = (uint8_t)((boot_utc_us >> (i * 8)) & 0xFF);
            }
            err = write_all(writer, msg, pos);
            if (err != ESP_OK) return err;
        }

        /* UTC offset (0 for now) */
        err = write_info_i32("time_ref_utc", 0);
        if (err != ESP_OK) return err;
    }

    return ESP_OK;
}

/** Write format messages for all subscribed topics. */
static esp_err_t write_format_messages(ulog_writer_t *writer)
{
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        const char *fmt = entry->meta->o_format;
        size_t fmt_len = strlen(fmt);
        if (fmt_len == 0) continue;

        uint16_t msg_total = (uint16_t)(fmt_len); /* no NUL terminator per ULog spec */
        uint8_t header[ULOG_MSG_HEADER_LEN];
        header[0] = (uint8_t)(msg_total & 0xFF);
        header[1] = (uint8_t)((msg_total >> 8) & 0xFF);
        header[2] = ULOG_MSG_TYPE_FORMAT;

        esp_err_t err = write_all(writer, header, sizeof(header));
        if (err != ESP_OK) return err;
        err = write_all(writer, fmt, fmt_len); /* no NUL terminator per ULog spec */
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/** Write subscription (ADD_LOGGED_MSG) messages. */
static esp_err_t write_subscription_messages(ulog_writer_t *writer)
{
    for (int i = 0; i < writer->num_topics; i++) {
        topic_entry_t *entry = &writer->topics[i];
        const char *name = entry->meta->o_name;
        size_t name_len = strlen(name);

        uint16_t payload_size = (uint16_t)(1 + 2 + name_len); /* multi_id + msg_id + name (no NUL per ULog spec) */
        uint8_t header[ULOG_MSG_HEADER_LEN];
        header[0] = (uint8_t)(payload_size & 0xFF);
        header[1] = (uint8_t)((payload_size >> 8) & 0xFF);
        header[2] = ULOG_MSG_TYPE_ADD_LOGGED_MSG;

        esp_err_t err = write_all(writer, header, sizeof(header));
        if (err != ESP_OK) return err;

        uint8_t body[256];
        body[0] = 0; /* multi_id = 0 */
        body[1] = (uint8_t)(entry->msg_id & 0xFF);
        body[2] = (uint8_t)((entry->msg_id >> 8) & 0xFF);
        memcpy(body + 3, name, name_len); /* no NUL terminator per ULog spec */

        err = write_all(writer, body, payload_size);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Time source detection                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  File system helpers                                                 */
/* ------------------------------------------------------------------ */

static void ensure_log_dir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0) {
        mkdir(dir, 0755);
        ESP_LOGI(TAG, "Created log directory: %s", dir);
    }
}

/** Recursively get total size of a directory. */
static size_t dir_total_size(const char *path)
{
    size_t total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[MAX_PATH + 64];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            total += dir_total_size(full);
        } else {
            total += (size_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

/** Count .ulg files recursively in a directory. */
static int count_ulg_files(const char *path)
{
    int count = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[MAX_PATH + 64];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count += count_ulg_files(full);
        } else {
            size_t nl = strlen(ent->d_name);
            if (nl >= 4 && strcmp(ent->d_name + nl - 4, ".ulg") == 0) {
                count++;
            }
        }
    }
    closedir(d);
    return count;
}

/** Recursively remove a directory and all its contents. */
static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[MAX_PATH + 64];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rmtree(full);
        } else {
            unlink(full);
        }
    }
    closedir(d);
    rmdir(path);
}

/** Directory entry for cleanup sorting. */
struct dir_entry {
    char     path[ULOG_MAX_PATH + ULOG_MAX_PATH + 16]; /**< log_root + "/" + d_name, max ~520 */
    time_t   mtime;   /**< Directory modification time */
    size_t   size;    /**< Total size of .ulg files inside */
};

/** Comparator for sorting directories oldest-first. */
static int cmp_dir_mtime(const void *a, const void *b)
{
    const dir_entry *da = (const dir_entry *)a;
    const dir_entry *db = (const dir_entry *)b;
    if (da->mtime < db->mtime) return -1;
    if (da->mtime > db->mtime) return  1;
    return 0;
}

/**
 * Cleanup old log directories if total size exceeds limit.
 * Scans date (YYYY-MM-DD) and session (sessNNN) subdirectories,
 * sorts oldest-first, and deletes directories until under capacity.
 * Respects ULOG_MIN_KEEP_FILES as a floor.
 */
static void cleanup_old_logs(const char *log_root, size_t size_limit)
{
    /* Allocate on heap — this function is called from httpd task which has a small stack.
     * dir_entry has a 320-byte path, and even 32 entries = ~10KB, too large for 4KB stack. */
    const int max_dirs = 32;
    dir_entry *dirs = (dir_entry *)calloc((size_t)max_dirs, sizeof(dir_entry));
    if (!dirs) return;

    int dir_count = 0;
    size_t total_size = 0;

    DIR *d = opendir(log_root);
    if (!d) { free(dirs); return; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && dir_count < max_dirs) {
        if (ent->d_name[0] == '.') continue;

        snprintf(dirs[dir_count].path, sizeof(dirs[dir_count].path),
                 "%s/%s", log_root, ent->d_name);

        struct stat st;
        if (stat(dirs[dir_count].path, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;

        dirs[dir_count].mtime = st.st_mtime;
        dirs[dir_count].size = dir_total_size(dirs[dir_count].path);
        total_size += dirs[dir_count].size;
        dir_count++;
    }
    closedir(d);

    if (dir_count == 0) { free(dirs); return; }

    /* Sort oldest first */
    qsort(dirs, (size_t)dir_count, sizeof(dirs[0]), cmp_dir_mtime);

    /* Count total ULog files once (not per-iteration O(n²)) */
    int file_count = count_ulg_files(log_root);

    /* Delete oldest directories until under capacity, respecting file floor */
    for (int i = 0; i < dir_count && total_size > size_limit; i++) {
        if (file_count <= (int)ULOG_MIN_KEEP_FILES) break;

        ESP_LOGI(TAG, "Cleanup: removing %s (%u KB)",
                 dirs[i].path, (unsigned)(dirs[i].size / 1024));
        rmtree(dirs[i].path);
        total_size -= dirs[i].size;
        /* Re-count after deletion (rmtree is O(N) anyway) */
        file_count = count_ulg_files(log_root);
    }

    free(dirs);
}

/* ------------------------------------------------------------------ */
/*  Log path creation (dual-mode naming)                                */
/* ------------------------------------------------------------------ */

/**
 * Find the next available file number in a directory.
 * PX4 starts at 100. Scans existing logNNN.ulg files.
 */
static uint16_t find_next_file_number(const char *dir)
{
    uint16_t max_num = 99;  /* PX4 starts at 100 */
    DIR *d = opendir(dir);
    if (!d) return 100;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        unsigned int num;
        if (sscanf(ent->d_name, "log%u.ulg", &num) == 1) {
            if (num > max_num) max_num = num;
        }
    }
    closedir(d);
    return max_num + 1;
}

/**
 * Find the next available session directory number.
 * Scans existing sessNNN directories.
 */
static uint16_t find_next_session_number(const char *log_root)
{
    uint16_t max_sess = 0;
    DIR *d = opendir(log_root);
    if (!d) return 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        unsigned int num;
        if (sscanf(ent->d_name, "sess%u", &num) == 1) {
            if (num > max_sess) max_sess = num;
        }
    }
    closedir(d);
    return max_sess + 1;
}

/**
 * Create the log directory and file path.
 *
 * Date mode (SNTP synced):  /sdcard/data/YYYY-MM-DD/<unix_timestamp>.ulg
 * Session mode (no RTC):    /sdcard/data/sessNNN/logNNN.ulg
 *
 * @return ESP_OK on success
 */
static esp_err_t create_log_path(ulog_writer_t *writer)
{
    char log_root[ULOG_MAX_PATH + 8];
    snprintf(log_root, sizeof(log_root), "%s/%s",
             writer->sd_mount_path, LOG_DIR_NAME);
    ensure_log_dir(log_root);

    /* Use a large local buffer for path construction to avoid truncation warnings.
     * Max path: log_dir(256) + "/" + base(32) + "_1000.ulg"(9) = 298 */
    char full_path[ULOG_MAX_PATH + 128];

    if (writer->has_wall_clock) {
        /* ── Date-based naming ── */
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            ESP_LOGE(TAG, "clock_gettime failed despite RTC available");
            return ESP_FAIL;
        }
        struct tm tm_buf;
        localtime_r(&ts.tv_sec, &tm_buf);

        /* Create date directory: YYYY-MM-DD */
        char date_dir[16];
        strftime(date_dir, sizeof(date_dir), "%Y-%m-%d", &tm_buf);

        snprintf(writer->log_dir, sizeof(writer->log_dir), "%s/%s",
                 log_root, date_dir);
        ensure_log_dir(writer->log_dir);

        /* Create filename: <unix_timestamp>.ulg (unique per second) */
        char filename[24];
        snprintf(filename, sizeof(filename), "%lld" ULOG_FILE_EXT,
                 (long long)ts.tv_sec);

        snprintf(full_path, sizeof(full_path), "%s/%s",
                 writer->log_dir, filename);

        /* If file already exists (same second), append _2, _3, etc. */
        struct stat st;
        if (stat(full_path, &st) == 0) {
            for (int suffix = 2; suffix < 100; suffix++) {
                snprintf(full_path, sizeof(full_path), "%s/%lld_%d" ULOG_FILE_EXT,
                         writer->log_dir, (long long)ts.tv_sec, suffix);
                if (stat(full_path, &st) != 0) break;
            }
        }
    } else {
        /* ── Session-based naming ── */
        uint16_t sess_num = find_next_session_number(log_root);
        /* Prefer the NVS session counter if higher */
        if (writer->session_counter > sess_num) {
            sess_num = writer->session_counter;
        }

        snprintf(writer->log_dir, sizeof(writer->log_dir), "%s/sess%03u",
                 log_root, (unsigned)sess_num);
        ensure_log_dir(writer->log_dir);

        /* PX4 convention: files start at log100.ulg */
        writer->file_counter = find_next_file_number(writer->log_dir);

        snprintf(full_path, sizeof(full_path), "%s/log%03u" ULOG_FILE_EXT,
                 writer->log_dir, (unsigned)writer->file_counter);
    }

    strlcpy(writer->filepath, full_path, sizeof(writer->filepath));
    return ESP_OK;
}

/**
 * Rotate to the next file when current file exceeds size limit.
 * Creates a new file in the same directory, writes headers, and
 * continues logging without stopping the task.
 */
static esp_err_t rotate_log_file(ulog_writer_t *writer)
{
    /* Drain remaining ring buffer data to current file */
    uint8_t buf[512];
    size_t n;
    while ((n = ringbuf_read(&writer->ringbuf, buf, sizeof(buf))) > 0) {
        write(writer->fd, buf, n);
        writer->bytes_written += n;
    }

    /* Close current file */
    if (writer->fd >= 0) {
        fsync(writer->fd);
        close(writer->fd);
        writer->fd = -1;
    }

    ESP_LOGI(TAG, "Rotating log file (size: %u KB)",
             (unsigned)(writer->bytes_written / 1024));

    /* Determine next filename.
     * Max path: log_dir(256) + "/" + base(32) + "_1000.ulg"(9) = 298 */
    char full_path[ULOG_MAX_PATH + 128];
    if (writer->has_wall_clock) {
        /* Date mode: use current unix timestamp with suffix for uniqueness */
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            /* Find a non-existing filename with _N suffix */
            writer->file_counter++;
            snprintf(full_path, sizeof(full_path), "%s/%lld_%u" ULOG_FILE_EXT,
                     writer->log_dir, (long long)ts.tv_sec, (unsigned)writer->file_counter);
            struct stat st;
            if (stat(full_path, &st) == 0) {
                /* Fallback: scan for next available suffix */
                for (unsigned i = writer->file_counter + 1; i < 1000; i++) {
                    snprintf(full_path, sizeof(full_path), "%s/%lld_%u" ULOG_FILE_EXT,
                             writer->log_dir, (long long)ts.tv_sec, i);
                    if (stat(full_path, &st) != 0) {
                        writer->file_counter = (uint16_t)i;
                        break;
                    }
                }
            }
        } else {
            /* Fallback: use uptime-based name */
            snprintf(full_path, sizeof(full_path), "%s/%llu" ULOG_FILE_EXT,
                     writer->log_dir, (unsigned long long)(esp_timer_get_time() / 1000ULL));
        }
    } else {
        /* Session mode: increment log file number (PX4 convention) */
        writer->file_counter++;
        snprintf(full_path, sizeof(full_path), "%s/log%03u" ULOG_FILE_EXT,
                 writer->log_dir, (unsigned)writer->file_counter);
    }
    strlcpy(writer->filepath, full_path, sizeof(writer->filepath));

    /* Open new file */
    writer->fd = open(writer->filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (writer->fd < 0) {
        ESP_LOGE(TAG, "Failed to create rotated file %s", writer->filepath);
        writer->state = ULOG_STATE_ERROR;
        return ESP_FAIL;
    }

    writer->bytes_written = 0;

    /* Re-write headers for the new file */
    esp_err_t err;
    err = write_file_header(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; return err; }

    err = write_flag_bits(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; return err; }

    err = write_info_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; return err; }

    err = write_format_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; return err; }

    err = write_subscription_messages(writer);
    if (err != ESP_OK) { close(writer->fd); writer->fd = -1; return err; }

    ESP_LOGI(TAG, "Rotated to new file: %s", writer->filepath);
    return ESP_OK;
}
