/*
 * SystemMonitor — Periodic system performance sampling and reporting.
 *
 * Design:
 *   - Background task samples CPU usage (via ulTaskGetIdleRunTimeCounter)
 *     and heap memory (via heap_caps_get_free_size) at a configurable interval.
 *   - CPU% is computed per-core from idle task runtime delta vs wall-clock delta:
 *     busy_cpu_pct = 100% - idle_pct. This avoids uxTaskGetSystemState()
 *     which calls vTaskSuspendAll() and disrupts LVGL rendering.
 *     Per-core idle runtime is read via xTaskGetIdleTaskHandleForCore() +
 *     vTaskGetInfo() (no scheduler suspend).
 *   - Results are published as a uORB `system_stats` topic, enabling:
 *     * ULog persistence (SD card binary log)
 *     * ESP_LOG summary output (configurable level/interval)
 *     * HTTP API (/api/system_stats) for remote monitoring
 *   - Historical minimum free heap is tracked per sampling period.
 *   - Alerts are published as uORB `system_alert` topic when:
 *     * Total CPU usage exceeds threshold (default 90%)
 *     * Internal SRAM usage exceeds threshold (default 80%)
 *     * PSRAM usage exceeds threshold (default 80%)
 *     Alerts are throttled by a cooldown period to avoid flooding.
 *
 * Usage:
 *   SystemMonitor::instance().init();     // once, after orb_init()
 *   SystemMonitor::instance().start();    // begin periodic sampling
 *   SystemMonitor::instance().stop();     // stop sampling
 *
 * Configuration (Kconfig):
 *   CONFIG_APP_SYS_MONITOR_INTERVAL_MS        — sampling interval (default 5000)
 *   CONFIG_APP_SYS_MONITOR_LOG_INTERVAL       — log summary every N samples (default 12 = 60s)
 *   CONFIG_APP_SYS_MONITOR_TASK_STACK         — monitor task stack size (default 4096)
 *   CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT      — CPU alert threshold (default 90)
 *   CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT      — Memory alert threshold (default 85)
 *   CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S   — Alert cooldown in seconds (default 30)
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uorb.h"
#include "generated/system_stats.h"
#include "generated/system_alert.h"

/* Alert types — must match system_alert.msg alert_type field */
#define SYS_ALERT_CPU_HIGH          0
#define SYS_ALERT_MEM_INTERNAL_HIGH 1
#define SYS_ALERT_MEM_PSRAM_HIGH    2

/* Alert severity levels */
#define SYS_ALERT_SEVERITY_INFO     0
#define SYS_ALERT_SEVERITY_WARNING  1
#define SYS_ALERT_SEVERITY_CRITICAL 2

class SystemMonitor {
public:
    /** Singleton access */
    static SystemMonitor& instance(void);

    /** Initialize the monitor. Must be called after orb_init(). */
    bool init(void);

    /** Start periodic sampling in a background task. */
    bool start(void);

    /** Stop sampling and join the background task. */
    void stop(void);

    /** @return true if the monitor is actively sampling. */
    bool is_running(void) const { return _running.load(std::memory_order_relaxed); }

    /** Get the latest sample (thread-safe read). */
    system_stats_s get_latest(void) const;

    /** Get historical minimum free internal SRAM since boot. */
    uint32_t min_free_internal(void) const { return _min_free_internal.load(std::memory_order_relaxed); }

    /** Get historical minimum free PSRAM since boot. */
    uint32_t min_free_psram(void) const { return _min_free_psram.load(std::memory_order_relaxed); }

    /** Get the latest alerts snapshot (thread-safe read). */
    void get_alerts(system_alert_s *cpu_alert, system_alert_s *mem_int_alert, system_alert_s *mem_psram_alert) const;

    /* Delete copy/move */
    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

private:
    SystemMonitor();
    ~SystemMonitor() = default;

    /** Background sampling task entry point. */
    static void _monitor_task_func(void *arg);

    /** Perform one sampling cycle. */
    void _sample(void);

    /** Check alert thresholds and publish alerts. */
    void _check_alerts(const system_stats_s &stats);

    /** Publish a single alert via uORB. */
    void _publish_alert(uint8_t alert_type, uint8_t severity,
                        uint32_t current_value, uint32_t threshold,
                        const char *task_name, uint32_t task_cpu_pct,
                        uint32_t free_internal, uint32_t free_psram);

    /** Log a human-readable summary to ESP_LOG. */
    void _log_summary(const system_stats_s &stats);

    /** uORB publisher handles. */
    std::atomic<orb_advert_t> _pub{ORB_ADVERT_INVALID};
    std::atomic<orb_advert_t> _alert_pub{ORB_ADVERT_INVALID};

    /** Background task handle. */
    TaskHandle_t _task_handle{nullptr};

    /** Running flag (atomic for cross-core safety). */
    std::atomic<bool> _running{false};
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _task_exited{false};  /* Set by task before vTaskDelete, polled by stop() */

    /** Historical minimum free heap (tracked across all samples). */
    std::atomic<uint32_t> _min_free_internal{UINT32_MAX};
    std::atomic<uint32_t> _min_free_psram{UINT32_MAX};

    /** Latest sampled stats (for get_latest / web API). */
    system_stats_s _latest{};
    mutable SemaphoreHandle_t _latest_mutex{nullptr};

    /** Latest alert states (for web API). */
    system_alert_s _alert_cpu{};
    system_alert_s _alert_mem_int{};
    system_alert_s _alert_mem_psram{};
    mutable SemaphoreHandle_t _alert_mutex{nullptr};

    /** Alert cooldown timestamps (esp_timer_get_time when last alert fired). */
    int64_t _last_alert_cpu_us{0};
    int64_t _last_alert_mem_int_us{0};
    int64_t _last_alert_mem_psram_us{0};

    /** Sample counter for log throttling. */
    uint32_t _sample_count{0};

    /** Previous per-core idle runtime counters and timestamp for delta CPU% calculation.
     * Uses xTaskGetIdleTaskHandleForCore() + vTaskGetInfo() which do NOT call
     * vTaskSuspendAll(), so they're safe alongside LVGL rendering on core 1. */
    uint32_t _prev_idle_runtime_core0{0};
    uint32_t _prev_idle_runtime_core1{0};
    int64_t _prev_timestamp_us{0};

    /** Cached total heap sizes (runtime constants, set once in init()). */
    uint32_t _total_internal{0};
    uint32_t _total_psram{0};

    static constexpr const char *TAG = "SysMonitor";
};
