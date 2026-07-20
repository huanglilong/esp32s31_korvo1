/*
 * SystemMonitor — Implementation.
 *
 * Samples CPU usage (via ulTaskGetIdleRunTimeCounter, no scheduler suspend)
 * and heap memory at a configurable interval, publishes results via uORB
 * system_stats topic.
 */

#include "system_monitor.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "topics.h"
#include <cstring>

/* Kconfig defaults — can be overridden in Kconfig.projbuild */
#ifndef CONFIG_APP_SYS_MONITOR_INTERVAL_MS
#define CONFIG_APP_SYS_MONITOR_INTERVAL_MS 5000
#endif

#ifndef CONFIG_APP_SYS_MONITOR_LOG_INTERVAL
#define CONFIG_APP_SYS_MONITOR_LOG_INTERVAL 12
#endif

#ifndef CONFIG_APP_SYS_MONITOR_TASK_STACK
#define CONFIG_APP_SYS_MONITOR_TASK_STACK 4096
#endif

/* Alert thresholds — default CPU > 90%, memory usage > 80% */
#ifndef CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT
#define CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT 90
#endif

#ifndef CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT
#define CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT 85
#endif

#ifndef CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S
#define CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S 10
#endif

/*============================================================================
 * Singleton
 *============================================================================*/
SystemMonitor& SystemMonitor::instance(void)
{
    static SystemMonitor inst;
    return inst;
}

SystemMonitor::SystemMonitor()
{
    memset(&_latest, 0, sizeof(_latest));
}

/*============================================================================
 * Init / Start / Stop
 *============================================================================*/
bool SystemMonitor::init(void)
{
    /* Use CAS to prevent double-init — two concurrent init() calls
     * must not both create mutexes and leak the duplicate. */
    bool expected = false;
    if (!_initialized.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        return true;  /* Another thread already initialized */
    }

    _latest_mutex = xSemaphoreCreateMutex();
    if (!_latest_mutex) {
        ESP_LOGE(TAG, "Failed to create latest_mutex");
        _initialized.store(false, std::memory_order_release);
        return false;
    }

    _alert_mutex = xSemaphoreCreateMutex();
    if (!_alert_mutex) {
        ESP_LOGE(TAG, "Failed to create alert_mutex");
        vSemaphoreDelete(_latest_mutex);
        _latest_mutex = nullptr;
        _initialized.store(false, std::memory_order_release);
        return false;
    }

    /* Track minimum free heap from boot */
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    _min_free_internal.store(free_int, std::memory_order_relaxed);
    _min_free_psram.store(free_psram, std::memory_order_relaxed);

    /* Cache total heap sizes (runtime constants — never change) */
    _total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    _total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    _initialized.store(true, std::memory_order_release);

    ESP_LOGI(TAG, "Initialized (interval=%dms, log_every=%d samples, "
             "cpu_alert=%d%%, mem_alert=%d%%, cooldown=%ds)",
             CONFIG_APP_SYS_MONITOR_INTERVAL_MS,
             CONFIG_APP_SYS_MONITOR_LOG_INTERVAL,
             CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT,
             CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT,
             CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S);
    return true;
}

bool SystemMonitor::start(void)
{
    if (!_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGE(TAG, "Not initialized, call init() first");
        return false;
    }

    /* Atomic compare-exchange to prevent double-start */
    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    _task_exited.store(false, std::memory_order_release);

    /* Allocate task stack from PSRAM to conserve internal SRAM.
     * sys_monitor is a low-priority sampling task — PSRAM latency is acceptable. */
    const uint32_t stack_words = CONFIG_APP_SYS_MONITOR_TASK_STACK;
    _task_stack = (StackType_t *)heap_caps_malloc(stack_words * sizeof(StackType_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM stack for monitor task");
        _running.store(false, std::memory_order_relaxed);
        return false;
    }

    _task_handle = xTaskCreateStaticPinnedToCore(
        _monitor_task_func,
        "sys_monitor",
        stack_words,
        this,
        1,  /* Low priority — must not interfere with real-time tasks */
        _task_stack,
        &_task_tcb,
        1   /* Pin to core 1 (core 0 reserved for camera/HTTP/NPU/SDIO) */
    );

    if (!_task_handle) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        heap_caps_free(_task_stack);
        _task_stack = nullptr;
        _running.store(false, std::memory_order_relaxed);
        return false;
    }

    ESP_LOGI(TAG, "Started on core 1");
    return true;
}

void SystemMonitor::stop(void)
{
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;  /* Not running */
    }

    /* Wait for task to exit. The monitor task checks _running each cycle,
     * sets _task_exited before vTaskDelete(NULL), and stop() polls the flag
     * instead of eTaskGetState() which races with idle task TCB recycling.
     * Total wait must cover one full task interval (up to 5s) plus margin. */
    if (_task_handle) {
        const int max_wait_ms = CONFIG_APP_SYS_MONITOR_INTERVAL_MS + 500;
        for (int i = 0; i < (max_wait_ms + 49) / 50 && !_task_exited.load(std::memory_order_acquire); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        /* If the task still hasn't signaled exit, do NOT force-delete it —
         * it may be holding _latest_mutex or _alert_mutex inside _sample(),
         * and force-deleting would orphan those mutexes permanently.
         * The task checks _running each loop iteration and will exit on its own. */
        if (!_task_exited.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, "Monitor task did not exit within %dms — will exit on next _running check",
                     max_wait_ms);
        }
        _task_handle = nullptr;
    }

    /* Free PSRAM-allocated stack (xTaskCreateStatic does not free it). */
    if (_task_stack) {
        heap_caps_free(_task_stack);
        _task_stack = nullptr;
    }

    ESP_LOGI(TAG, "Stopped");
}

/*============================================================================
 * Background Task
 *============================================================================*/
void SystemMonitor::_monitor_task_func(void *arg)
{
    SystemMonitor *self = static_cast<SystemMonitor *>(arg);

    ESP_LOGI(TAG, "Monitor task started");

    while (self->_running.load(std::memory_order_relaxed)) {
        self->_sample();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SYS_MONITOR_INTERVAL_MS));
    }

    /* Clean up and self-delete.
     * Signal stop() that we're exiting via _task_exited flag so it can
     * check without eTaskGetState() which races with idle task TCB recycling.
     * Do NOT write _task_handle — the owner (stop()) manages it exclusively
     * to avoid racing with a subsequent start() call. */
    self->_task_exited.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Monitor task exiting");
    vTaskDelete(NULL);
}

/*============================================================================
 * Sampling
 *============================================================================*/
void SystemMonitor::_sample(void)
{
    /* ── 1. Heap memory stats ── */
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    /* Update historical minimums */
    uint32_t prev_min = _min_free_internal.load(std::memory_order_relaxed);
    if (min_free_int < prev_min) {
        _min_free_internal.store(min_free_int, std::memory_order_relaxed);
    }
    prev_min = _min_free_psram.load(std::memory_order_relaxed);
    if (min_free_psram < prev_min) {
        _min_free_psram.store(min_free_psram, std::memory_order_relaxed);
    }

    /* ── 2. CPU usage via per-core idle runtime (non-blocking).
     * xTaskGetIdleTaskHandleForCore() + vTaskGetInfo() read each core's
     * idle task runtime counter without calling vTaskSuspendAll(), so
     * they're safe alongside LVGL rendering on core 1.
     *   idle_pct  = idle_delta / wall_delta × 10000
     *   busy_pct  = 10000 - idle_pct
     */
    UBaseType_t actual_count = uxTaskGetNumberOfTasks();
    int64_t now_us = esp_timer_get_time();

    /* ── 3. Build system_stats_s ── */
    system_stats_s stats = {};
    stats.timestamp = (uint64_t)esp_timer_get_time();
    stats.free_internal = free_internal;
    stats.free_psram = free_psram;
    stats.min_free_internal = min_free_int;
    stats.min_free_psram = min_free_psram;
    stats.task_count = (uint32_t)actual_count;

    /* Compute per-core and total busy CPU% from idle runtime deltas */
    if (_prev_timestamp_us > 0 && now_us > _prev_timestamp_us) {
        int64_t delta_us = now_us - _prev_timestamp_us;

        /* Read per-core idle task runtime via vTaskGetInfo (no scheduler suspend).
         * Pass eReady as eState to avoid vTaskSuspendAll() path inside vTaskGetInfo
         * (only triggered when eState==eInvalid for suspended tasks). */
        TaskStatus_t idle_status;
        uint32_t idle_runtime_core0 = 0;
        uint32_t idle_runtime_core1 = 0;

        TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
        if (idle0) {
            vTaskGetInfo(idle0, &idle_status, pdFALSE, eReady);
            idle_runtime_core0 = idle_status.ulRunTimeCounter;
        }
        TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
        if (idle1) {
            vTaskGetInfo(idle1, &idle_status, pdFALSE, eReady);
            idle_runtime_core1 = idle_status.ulRunTimeCounter;
        }

        /* Per-core busy CPU% */
        if (idle_runtime_core0 >= _prev_idle_runtime_core0 && delta_us > 0) {
            uint32_t idle_delta = idle_runtime_core0 - _prev_idle_runtime_core0;
            uint32_t idle_pct = (uint32_t)((uint64_t)idle_delta * 10000 / (uint64_t)delta_us);
            if (idle_pct > 10000) idle_pct = 10000;
            stats.core0_cpu_pct = 10000 - idle_pct;
        }
        if (idle_runtime_core1 >= _prev_idle_runtime_core1 && delta_us > 0) {
            uint32_t idle_delta = idle_runtime_core1 - _prev_idle_runtime_core1;
            uint32_t idle_pct = (uint32_t)((uint64_t)idle_delta * 10000 / (uint64_t)delta_us);
            if (idle_pct > 10000) idle_pct = 10000;
            stats.core1_cpu_pct = 10000 - idle_pct;
        }

        /* Total busy CPU% = average of per-core busy% */
        stats.total_cpu_pct = (stats.core0_cpu_pct + stats.core1_cpu_pct) / configNUMBER_OF_CORES;

        _prev_idle_runtime_core0 = idle_runtime_core0;
        _prev_idle_runtime_core1 = idle_runtime_core1;
    }
    _prev_timestamp_us = now_us;

    /* ── 4. Update latest snapshot (mutex-protected) ── */
    if (_latest_mutex && xSemaphoreTake(_latest_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _latest = stats;
        xSemaphoreGive(_latest_mutex);
    }

    /* ── 5. Publish via uORB ── */
    orb_advert_t pub = _pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(system_stats));
        orb_advert_t expected = ORB_ADVERT_INVALID;
        if (!_pub.compare_exchange_strong(expected, new_pub, std::memory_order_acq_rel)) {
            /* Another thread beat us — use the existing publisher */
            /* new_pub is leaked but this is a one-time event on a singleton */
        }
    }
    pub = _pub.load(std::memory_order_acquire);
    if (pub >= 0) {
        orb_publish(ORB_ID(system_stats), pub, &stats);
    }

    /* ── 6. Periodic ESP_LOG summary ── */
    _sample_count++;
    if (CONFIG_APP_SYS_MONITOR_LOG_INTERVAL > 0 &&
        (_sample_count % CONFIG_APP_SYS_MONITOR_LOG_INTERVAL) == 0) {
        _log_summary(stats);
    }

    /* ── 7. Check alert thresholds ── */
    _check_alerts(stats);
}

/*============================================================================
 * Log Summary
 *============================================================================*/
void SystemMonitor::_log_summary(const system_stats_s &stats)
{
    ESP_LOGI(TAG, "───── System Stats ─────");
    uint32_t total_int = _total_internal;
    uint32_t total_psram = _total_psram;
    uint32_t used_pct_int = total_int > 0 ? (uint32_t)((uint64_t)(total_int - stats.free_internal) * 100 / total_int) : 0;
    uint32_t used_pct_psram = total_psram > 0 ? (uint32_t)((uint64_t)(total_psram - stats.free_psram) * 100 / total_psram) : 0;
    ESP_LOGI(TAG, "  Internal SRAM: %u KB free / %u KB total (%u%% used, min %u KB)",
             stats.free_internal / 1024, total_int / 1024,
             used_pct_int, stats.min_free_internal / 1024);
    ESP_LOGI(TAG, "  PSRAM:         %u KB free / %u KB total (%u%% used, min %u KB)",
             stats.free_psram / 1024, total_psram / 1024,
             used_pct_psram, stats.min_free_psram / 1024);
    ESP_LOGI(TAG, "  Tasks: %u   CPU: %u.%02u%% (Core0: %u.%02u%%  Core1: %u.%02u%%)",
             stats.task_count,
             stats.total_cpu_pct / 100,
             stats.total_cpu_pct % 100,
             stats.core0_cpu_pct / 100,
             stats.core0_cpu_pct % 100,
             stats.core1_cpu_pct / 100,
             stats.core1_cpu_pct % 100);
}

/*============================================================================
 * Alert Checking
 *============================================================================*/
void SystemMonitor::_check_alerts(const system_stats_s &stats)
{
    int64_t now_us = esp_timer_get_time();
    int64_t cooldown_us = (int64_t)CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S * 1000000LL;

    /* ── CPU alert ── */
    uint32_t cpu_threshold = (uint32_t)CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT * 100;  /* 90% → 9000 */
    if (stats.total_cpu_pct >= cpu_threshold) {
        if ((now_us - _last_alert_cpu_us) >= cooldown_us) {
            /* Determine severity: >95% = CRITICAL, else WARNING */
            uint8_t severity = (stats.total_cpu_pct >= cpu_threshold + 500)  /* 95%+ */
                               ? SYS_ALERT_SEVERITY_CRITICAL
                               : SYS_ALERT_SEVERITY_WARNING;

            /* Per-task breakdown not available (uxTaskGetSystemState() would
             * cause display flickering), so report aggregate only. */
            _publish_alert(SYS_ALERT_CPU_HIGH, severity,
                           stats.total_cpu_pct, cpu_threshold,
                           "", 0,
                           stats.free_internal, stats.free_psram);

            _last_alert_cpu_us = now_us;

            ESP_LOGW(TAG, "⚠ CPU ALERT: %u.%02u%% exceeds %d%% threshold",
                     stats.total_cpu_pct / 100, stats.total_cpu_pct % 100,
                     CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT);
        }
    }

    /* ── Internal SRAM alert ── */
    uint32_t total_internal = _total_internal;
    if (total_internal > 0) {
        uint32_t used_pct = (uint32_t)((uint64_t)(total_internal - stats.free_internal) * 10000 / total_internal);
        uint32_t mem_threshold = (uint32_t)CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT * 100;  /* 80% → 8000 */
        if (used_pct >= mem_threshold) {
            if ((now_us - _last_alert_mem_int_us) >= cooldown_us) {
                uint8_t severity = (used_pct >= mem_threshold + 1000)  /* 90%+ */
                                   ? SYS_ALERT_SEVERITY_CRITICAL
                                   : SYS_ALERT_SEVERITY_WARNING;

                _publish_alert(SYS_ALERT_MEM_INTERNAL_HIGH, severity,
                               used_pct, mem_threshold,
                               "", 0,
                               stats.free_internal, stats.free_psram);

                _last_alert_mem_int_us = now_us;

                ESP_LOGW(TAG, "⚠ MEM(internal) ALERT: %u.%02u%% used exceeds %d%% threshold (%u KB free of %u KB)",
                         used_pct / 100, used_pct % 100,
                         CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT,
                         stats.free_internal / 1024, total_internal / 1024);
            }
        }
    }

    /* ── PSRAM alert ── */
    uint32_t total_psram = _total_psram;
    if (total_psram > 0) {
        uint32_t used_pct = (uint32_t)((uint64_t)(total_psram - stats.free_psram) * 10000 / total_psram);
        uint32_t mem_threshold = (uint32_t)CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT * 100;  /* 80% → 8000 */
        if (used_pct >= mem_threshold) {
            if ((now_us - _last_alert_mem_psram_us) >= cooldown_us) {
                uint8_t severity = (used_pct >= mem_threshold + 1000)  /* 90%+ */
                                   ? SYS_ALERT_SEVERITY_CRITICAL
                                   : SYS_ALERT_SEVERITY_WARNING;

                _publish_alert(SYS_ALERT_MEM_PSRAM_HIGH, severity,
                               used_pct, mem_threshold,
                               "", 0,
                               stats.free_internal, stats.free_psram);

                _last_alert_mem_psram_us = now_us;

                ESP_LOGW(TAG, "⚠ MEM(PSRAM) ALERT: %u.%02u%% used exceeds %d%% threshold (%u KB free of %u KB)",
                         used_pct / 100, used_pct % 100,
                         CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT,
                         stats.free_psram / 1024, total_psram / 1024);
            }
        }
    }
}

void SystemMonitor::_publish_alert(uint8_t alert_type, uint8_t severity,
                                    uint32_t current_value, uint32_t threshold,
                                    const char *task_name, uint32_t task_cpu_pct,
                                    uint32_t free_internal, uint32_t free_psram)
{
    system_alert_s alert = {};
    alert.timestamp = (uint64_t)esp_timer_get_time();
    alert.alert_type = alert_type;
    alert.severity = severity;
    alert.current_value = current_value;
    alert.threshold = threshold;
    if (task_name) {
        strncpy(alert.task_name, task_name, 15);
        alert.task_name[15] = '\0';
    }
    alert.task_cpu_pct = task_cpu_pct;
    alert.free_internal = free_internal;
    alert.free_psram = free_psram;

    /* Update latest alert snapshot (mutex-protected) */
    if (_alert_mutex && xSemaphoreTake(_alert_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        switch (alert_type) {
        case SYS_ALERT_CPU_HIGH:       _alert_cpu = alert; break;
        case SYS_ALERT_MEM_INTERNAL_HIGH: _alert_mem_int = alert; break;
        case SYS_ALERT_MEM_PSRAM_HIGH:   _alert_mem_psram = alert; break;
        default: break;
        }
        xSemaphoreGive(_alert_mutex);
    }

    /* Publish via uORB */
    orb_advert_t pub = _alert_pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(system_alert));
        orb_advert_t expected = ORB_ADVERT_INVALID;
        _alert_pub.compare_exchange_strong(expected, new_pub, std::memory_order_acq_rel);
    }
    pub = _alert_pub.load(std::memory_order_acquire);
    if (pub >= 0) {
        orb_publish(ORB_ID(system_alert), pub, &alert);
    }
}

/*============================================================================
 * Get latest snapshot (thread-safe)
 *============================================================================*/
system_stats_s SystemMonitor::get_latest(void) const
{
    system_stats_s result = {};
    if (_latest_mutex && xSemaphoreTake(_latest_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = _latest;
        xSemaphoreGive(_latest_mutex);
    }
    return result;
}

void SystemMonitor::get_alerts(system_alert_s *cpu_alert,
                                system_alert_s *mem_int_alert,
                                system_alert_s *mem_psram_alert) const
{
    if (!cpu_alert || !mem_int_alert || !mem_psram_alert) return;

    if (_alert_mutex && xSemaphoreTake(_alert_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *cpu_alert = _alert_cpu;
        *mem_int_alert = _alert_mem_int;
        *mem_psram_alert = _alert_mem_psram;
        xSemaphoreGive(_alert_mutex);
    } else {
        memset(cpu_alert, 0, sizeof(system_alert_s));
        memset(mem_int_alert, 0, sizeof(system_alert_s));
        memset(mem_psram_alert, 0, sizeof(system_alert_s));
    }
}
