/**
 * @file web_config_server.cpp
 * @brief Web configuration server for ESP32-S31-Korvo-1.
 *
 * HTTP server on port 8080 with REST APIs and Web UI:
 *   WiFi:  GET  /api/wifi/scan, POST /api/wifi/connect, GET /api/wifi/status
 *   Audio: POST /api/audio/volume, GET /api/audio/volume
 *          GET  /api/audio/record_start, /api/audio/record_stop, /api/audio/record_status
 *          GET  /api/audio/list, /api/audio/play, /api/audio/play_status, /api/audio/stop
 *   Files: GET  /api/files/list, GET /api/files/download,
 *          POST /api/files/delete, POST /api/files/delete_batch
 *   ULog:  GET  /api/ulog/status, POST /api/ulog/start, POST /api/ulog/stop
 *   System:GET  /api/system/info, /api/system/stats, /api/system/timezone
 *          POST /api/system/timezone
 *   SDCard:GET  /api/sdcard/info
 *
 * Web UI: GET / — 4-tab page with auto-refresh:
 *   WiFi:    Auto-scan (10s), select SSID → password modal, no manual Scan/Connect
 *   Audio:   Record (Start/Stop toggle + status), Music Player (auto-refresh 5s, play_status poll)
 *   Files:   File manager with download/delete status bar updates
 *   System:  System Info (auto-refresh 5s), Volume (real-time slider), ULog Record (Start/Stop toggle + status, auto-refresh 3s)
 *
 * mDNS: advertises _http._tcp on esp-web-XXXXXX.local:8080
 *
 * Reference: esp32p4_monitor/main/web_config_server.cpp
 */

#include "web_config_server.hpp"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/stat.h"
#include "dirent.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <ctime>

#include "app_config.h"
#include "wifi_service.hpp"
#include "drivers/audio/audio_driver.hpp"
#include "drivers/audio/audio_ulog_recorder.hpp"
#include "drivers/bt_audio/bt_audio_driver.hpp"
#include "drivers/camera/camera_app.hpp"
#include "drivers/sdcard/sdcard_driver.hpp"
#include "drivers/system_monitor/system_monitor.hpp"
#include "ulog_writer.h"

/* Audio recording / playback */
#include "esp_audio_simple_player.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_aac_enc.h"

static const char *TAG = "web_config";

static httpd_handle_t s_httpd = nullptr;

/* ── Audio recording / playback state ──────────────────────────── */

#define REC_BUF_SAMPLES      480
#define REC_BUF_BYTES        (REC_BUF_SAMPLES * 2 * sizeof(int16_t))

/* AAC encoder: codec ADC → AAC (ADTS) → SD card (.aac file).
 * Uses esp_audio_enc official API with adts_used=true.
 * esp_audio_simple_player supports AAC playback natively. */

static std::atomic<TaskHandle_t> s_audio_task{nullptr};
static StackType_t   *s_audio_stack = NULL;
static StaticTask_t  *s_audio_tcb = NULL;
static std::atomic<bool>  s_audio_running{false};
static std::atomic<bool>  s_is_recording{false};
static esp_audio_enc_handle_t s_encoder = nullptr;
static uint8_t       *s_enc_in_buf = NULL;    /* PCM accumulation buffer (encoder input) */
static int            s_enc_in_size = 0;       /* Required input frame size (bytes) */
static std::atomic<int> s_enc_in_count{0};    /* Accumulated PCM bytes (cross-task: written by audio_task, reset by API handler before task start) */
static uint8_t       *s_enc_out_buf = NULL;   /* Encoder output buffer */
static int            s_enc_out_size = 0;      /* Output buffer size */
static FILE          *s_rec_file = NULL;
static std::atomic<uint32_t> s_rec_bytes{0};
static std::atomic<uint32_t> s_rec_start_ms{0};
static char           s_rec_path[128];

/* Playback */
static esp_asp_handle_t s_asp = NULL;
static std::atomic<bool>    s_playing{false};
static char           s_playing_file[128] = {};

/* Mutual exclusion — file manager blocks audio ops during download/delete */
static std::atomic<bool>    s_fm_busy{false};

/* Audio mutex — serializes audio operations across concurrent HTTP handlers */
static SemaphoreHandle_t s_audio_mutex = NULL;

static void audio_lock(void)
{
    if (s_audio_mutex) xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
}

static void audio_unlock(void)
{
    if (s_audio_mutex) xSemaphoreGive(s_audio_mutex);
}

/* ── Audio state accessors (for AudioUlogRecorder mutual exclusion) ── */
bool web_config_is_aac_recording(void) { return s_is_recording.load(std::memory_order_acquire); }
bool web_config_is_playing(void) { return s_playing.load(std::memory_order_acquire); }

static void _stop_audio_task_if_running(void)
{
    if (!s_audio_task.load(std::memory_order_acquire) && !s_audio_running) return;
    s_audio_running = false;
    for (int i = 0; i < 10 && s_audio_task.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_audio_task.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Audio task did not exit in time, force deleting");
        vTaskDelete(s_audio_task.exchange(nullptr, std::memory_order_acq_rel));
    }
    if (s_audio_stack) { heap_caps_free(s_audio_stack); s_audio_stack = NULL; }
}

/* Audio recording task: codec ADC → AAC (ADTS) → SD card */
static void audio_task(void *arg)
{
    (void)arg;
    int16_t *buf = (int16_t *)heap_caps_calloc(1, REC_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { s_audio_running = false; s_audio_task.store(nullptr, std::memory_order_release); vTaskDelete(NULL); return; }
    while (s_audio_running) {
        int n = AudioDriver::instance().codec_read((uint8_t*)buf, REC_BUF_BYTES);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        if (s_is_recording && s_encoder && s_enc_in_buf && s_enc_out_buf) {
            const uint8_t *src = (const uint8_t*)buf;
            int remaining = n;
            while (remaining > 0) {
                int count = s_enc_in_count.load(std::memory_order_relaxed);
                int space = s_enc_in_size - count;
                int copy = (remaining < space) ? remaining : space;
                memcpy(s_enc_in_buf + count, src, copy);
                s_enc_in_count.store(count + copy, std::memory_order_relaxed);
                src += copy;
                remaining -= copy;

                if (s_enc_in_count.load(std::memory_order_relaxed) >= s_enc_in_size) {
                    esp_audio_enc_in_frame_t in_frame = {
                        .buffer = s_enc_in_buf, .len = (uint32_t)s_enc_in_size
                    };
                    esp_audio_enc_out_frame_t out_frame = {
                        .buffer = s_enc_out_buf, .len = (uint32_t)s_enc_out_size
                    };
                    if (esp_audio_enc_process(s_encoder, &in_frame, &out_frame) == ESP_AUDIO_ERR_OK
                        && out_frame.encoded_bytes > 0 && s_rec_file) {
                        size_t wr = fwrite(out_frame.buffer, 1, out_frame.encoded_bytes, s_rec_file);
                        s_rec_bytes.fetch_add((uint32_t)wr, std::memory_order_relaxed);
                    }
                    s_enc_in_count.store(0, std::memory_order_relaxed);
                }
            }
        }
    }
    heap_caps_free(buf);
    s_audio_task.store(nullptr, std::memory_order_release);
    vTaskDelete(NULL);
}

/* ── Path sanitizer ─────────────────────────────────────────────── */

static bool _path_sanitize(const char *user_path, char *out, size_t out_len) {
    if (!user_path || !out || out_len == 0) return false;
    out[0] = '\0';
    if (user_path[0] == '\0') return false;

    char full[256];
    if (strncmp(user_path, "/sdcard", 7) == 0 && (user_path[7] == '\0' || user_path[7] == '/')) {
        snprintf(full, sizeof(full), "%s", user_path);
    } else if (user_path[0] == '/') {
        snprintf(full, sizeof(full), "/sdcard%s", user_path);
    } else {
        snprintf(full, sizeof(full), "/sdcard/%s", user_path);
    }

    size_t len = strlen(full);
    while (len > 1 && full[len - 1] == '/') full[--len] = '\0';
    if (len == 0 || (len == 1 && full[0] == '/')) {
        snprintf(full, sizeof(full), "/sdcard");
        len = strlen(full);
    }

    {   /* Reject ".." as a path segment */
        const char *p = full;
        while (*p) {
            if (*p == '/') p++;
            if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return false;
            while (*p && *p != '/') p++;
        }
    }

    if (strncmp(full, "/sdcard", 7) != 0) return false;
    if (full[7] != '\0' && full[7] != '/') return false;

    strlcpy(out, full, out_len);
    return true;
}

/* URL-decode %XX sequences in-place */
static int _hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = c & 0xDF;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void _url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = _hex_digit(r[1]), lo = _hex_digit(r[2]);
            if (hi >= 0 && lo >= 0) { *w++ = (char)((hi << 4) | lo); r += 3; }
            else *w++ = *r++;
        } else *w++ = *r++;
    }
    *w = '\0';
}

/* Playback output callback — writes to codec DAC via esp_codec_dev.
 * Same pattern as esp32p4_monitor._asp_output_cb: call esp_codec_dev_write()
 * which internally writes to the I2S TX channel managed by BSP/esp_codec_dev. */
static int _asp_out(uint8_t *d, int sz, void *_) {
    (void)_;
    return AudioDriver::instance().codec_write(d, sz);
}

/* Playback event callback — tracks playing state.
 * Must hold s_audio_mutex when writing to s_playing_file/s_playing
 * to avoid data race with API handlers (try_lock, non-blocking). */
static int _asp_evt(esp_asp_event_pkt_t *pkt, void *_) {
    (void)_;
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
        int s = *(esp_asp_state_t*)pkt->payload;
        if (s == ESP_ASP_STATE_FINISHED || s == ESP_ASP_STATE_STOPPED || s == ESP_ASP_STATE_ERROR) {
            if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, 0) == pdTRUE) {
                s_playing_file[0] = '\0';
                s_playing = false;
                xSemaphoreGive(s_audio_mutex);
            } else {
                /* Mutex busy — another handler is stopping/starting playback.
                 * The handler will clear state itself. Skip update to avoid
                 * overwriting newer state. */
            }
        }
    }
    return 0;
}

/* ── SNTP state ──────────────────────────────────────────────────── */
static std::atomic<bool> s_sntp_initialized{false};
static std::atomic<bool> s_sntp_synced{false};

/* Saved timezone string (e.g. "CST-8") — persisted in NVS */
static char s_timezone[32] = {};
static std::atomic<bool> s_timezone_loaded{false};
static SemaphoreHandle_t s_timezone_mutex = nullptr;  /* Protects s_timezone r/w */

/* ── Timezone helpers ──────────────────────────────────────────────── */

/* Atomic mutex to prevent race on first creation */
static std::atomic<SemaphoreHandle_t> s_tz_mutex_atomic{nullptr};

static void _load_timezone_from_nvs(void)
{
    /* Create mutex on first call — CAS prevents double-creation race */
    if (!s_tz_mutex_atomic.load(std::memory_order_acquire)) {
        SemaphoreHandle_t new_mtx = xSemaphoreCreateMutex();
        SemaphoreHandle_t expected = nullptr;
        if (!s_tz_mutex_atomic.compare_exchange_strong(expected, new_mtx,
                std::memory_order_release, std::memory_order_acquire)) {
            /* Another thread beat us — free our duplicate */
            vSemaphoreDelete(new_mtx);
        }
    }
    s_timezone_mutex = s_tz_mutex_atomic.load(std::memory_order_acquire);

    if (s_timezone_loaded.load(std::memory_order_acquire)) return;

    if (s_timezone_mutex) xSemaphoreTake(s_timezone_mutex, portMAX_DELAY);

    /* Double-check after acquiring mutex */
    if (s_timezone_loaded.load(std::memory_order_acquire)) {
        if (s_timezone_mutex) xSemaphoreGive(s_timezone_mutex);
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_timezone);
        if (nvs_get_str(h, NVS_KEY_TIMEZONE, s_timezone, &len) == ESP_OK) {
            ESP_LOGI(TAG, "Timezone loaded from NVS: %s", s_timezone);
        }
        nvs_close(h);
    }

    /* Default to Kconfig timezone if NVS has no timezone */
    if (strlen(s_timezone) == 0) {
        strlcpy(s_timezone, CONFIG_SNTP_DEFAULT_TIMEZONE, sizeof(s_timezone));
    }

    setenv("TZ", s_timezone, 1);
    tzset();
    s_timezone_loaded.store(true, std::memory_order_release);

    if (s_timezone_mutex) xSemaphoreGive(s_timezone_mutex);
}

/* ── SNTP initialization ──────────────────────────────────────────── */

/**
 * @brief Start SNTP time sync and enable ULog wall-clock.
 *
 * Called once after WiFi STA gets an IP. Handles three cases:
 *   1. Already synced (defensive) — just mark synced, enable ULog wall-clock
 *   2. Already in progress — register callback for when sync completes
 *   3. Fresh init — set NTP server, register callback, call esp_sntp_init()
 *
 * On sync: logs current time to console, sets s_sntp_synced=true,
 *           enables ULog wall-clock timestamps.
 */
static void sntp_start_and_ulog_autostart(void)
{
    if (s_sntp_initialized.load(std::memory_order_acquire)) return;

    /* Ensure timezone is loaded before SNTP starts */
    _load_timezone_from_nvs();

    /* 1. Already synced? (defensive) */
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ulog_writer_t *ulog = ulog_writer_get();
        ulog_writer_set_wall_clock(ulog, true);
        WifiService::instance().set_sntp_synced();
        s_sntp_synced.store(true, std::memory_order_release);
        s_sntp_initialized.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "SNTP already synced (defensive check)");
        return;
    }

    /* 2. Already in progress but not yet synced — just register callback */
    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET) {
        s_sntp_initialized.store(true, std::memory_order_release);
        esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
            struct tm tm;
            localtime_r(&tv->tv_sec, &tm);
            /* Snapshot s_timezone under mutex to avoid data race with HTTP POST handler */
            char tz_snap[32] = {};
            if (s_timezone_mutex && xSemaphoreTake(s_timezone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
                xSemaphoreGive(s_timezone_mutex);
            } else {
                strlcpy(tz_snap, s_timezone, sizeof(tz_snap));  /* Best-effort fallback */
            }
            ESP_LOGI(TAG, "SNTP synchronized: %04d-%02d-%02d %02d:%02d:%02d (TZ=%s)",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, tz_snap);
            ulog_writer_t *ulog = ulog_writer_get();
            ulog_writer_set_wall_clock(ulog, true);
            WifiService::instance().set_sntp_synced();
            s_sntp_synced.store(true, std::memory_order_release);
        });
        return;
    }

    /* 3. Fresh init */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_SNTP_SERVER_0);

    esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
        struct tm tm;
        localtime_r(&tv->tv_sec, &tm);
        /* Snapshot s_timezone under mutex to avoid data race with HTTP POST handler */
        char tz_snap[32] = {};
        if (s_timezone_mutex && xSemaphoreTake(s_timezone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
            xSemaphoreGive(s_timezone_mutex);
        } else {
            strlcpy(tz_snap, s_timezone, sizeof(tz_snap));  /* Best-effort fallback */
        }
        ESP_LOGI(TAG, "SNTP synchronized: %04d-%02d-%02d %02d:%02d:%02d (TZ=%s)",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, tz_snap);
        ulog_writer_t *ulog = ulog_writer_get();
        ulog_writer_set_wall_clock(ulog, true);
        WifiService::instance().set_sntp_synced();
        s_sntp_synced.store(true, std::memory_order_release);
    });
    esp_sntp_init();
    s_sntp_initialized.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "SNTP started with server %s — waiting for sync...",
             CONFIG_SNTP_SERVER_0);
}
std::atomic<bool> s_server_running{false};

/* ── HTML helpers ────────────────────────────────────────────────── */

static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32-S31 Korvo-1</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #e0e0e0; padding: 10px; max-width: 700px; margin: 0 auto; }
  h1 { text-align: center; color: #e94560; margin-bottom: 10px; font-size: 20px; }
  .tabs { display: flex; gap: 4px; margin-bottom: 12px; }
  .tab { flex: 1; padding: 8px; text-align: center; background: #16213e; color: #888; border-radius: 8px 8px 0 0; cursor: pointer; font-size: 13px; }
  .tab.active { background: #0f3460; color: #e94560; font-weight: bold; }
  .card { background: #16213e; border-radius: 12px; padding: 16px; margin-bottom: 12px; }
  .card h2 { font-size: 16px; color: #0f3460; background: #e94560; display: inline-block; padding: 3px 10px; border-radius: 6px; margin-bottom: 10px; }
  label { display: block; font-size: 13px; color: #aaa; margin-bottom: 3px; }
  input, select { width: 100%; padding: 8px; border: 1px solid #333; border-radius: 8px; background: #0f3460; color: #fff; font-size: 13px; margin-bottom: 10px; }
  button { background: #0f3460; color: #fff; border: none; padding: 8px 16px; border-radius: 8px; font-size: 13px; cursor: pointer; }
  button:hover { background: #1a5276; }
  button.sm { padding: 4px 10px; font-size: 12px; }
  button.green { background: #2ecc71; }
  button.red { background: #e74c3c; }
  button.gray { background: #555; }
  .status { font-size: 12px; color: #3498db; margin-top: 6px; }
  .error { color: #f44; }
  table { width: 100%; font-size: 12px; border-collapse: collapse; margin-top: 6px; }
  td { padding: 3px 6px; border-bottom: 1px solid #333; }
  td:first-child { color: #888; }
  .flex { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
  .hidden { display: none; }
  .modal-overlay { display:none; position:fixed; top:0; left:0; width:100%; height:100%; background:rgba(0,0,0,0.6); z-index:100; justify-content:center; align-items:center; }
  .modal-overlay.show { display:flex; }
  .modal { background:#16213e; border-radius:12px; padding:20px; max-width:320px; width:90%; }
  .modal h3 { color:#e94560; margin-bottom:12px; font-size:15px; }
  .modal label { font-size:13px; color:#aaa; margin-bottom:3px; }
  .modal input { width:100%; padding:8px; border:1px solid #333; border-radius:8px; background:#0f3460; color:#fff; font-size:13px; margin-bottom:10px; }
  .modal .flex { display:flex; gap:8px; justify-content:flex-end; }
</style>
</head>
<body>
<h1>ESP32-S31 Korvo-1</h1>

<div class="tabs">
  <div class="tab active" onclick="switchTab('wifi')">WiFi</div>
  <div class="tab" onclick="switchTab('audio')">Audio</div>
  <div class="tab" onclick="switchTab('files')">Files</div>
  <div class="tab" onclick="switchTab('system')">System</div>
</div>

<!-- WiFi Password Modal -->
<div class="modal-overlay" id="wifi-modal">
<div class="modal">
  <h3 id="modal-ssid-label">Connect to WiFi</h3>
  <label>Password</label>
  <input type="password" id="modal-password" placeholder="WiFi Password" onkeydown="if(event.key==='Enter')connectFromModal();else if(['Shift','Control','Alt','Meta','CapsLock','Tab','Escape'].indexOf(event.key)>=0)return;else event.stopPropagation()">
  <div class="flex">
    <button class="gray sm" onclick="closeWifiModal()">Cancel</button>
    <button onclick="connectFromModal()">Connect</button>
  </div>
</div>
</div>

<!-- WiFi Tab -->
<div id="tab-wifi">
<div class="card">
  <h2>WiFi</h2>
  <div id="wifi-status" class="status"></div>
  <table id="scan-results"></table>
</div>
</div>

<!-- Audio Tab -->
<div id="tab-audio" class="hidden">
<div class="card">
  <h2>Record</h2>
  <div class="flex">
    <button class="green" id="btn-rec" onclick="recToggle()">▶</button>
    <span id="rec-status" style="font-size:12px;color:#3498db">Stopped</span>
  </div>
</div>
<div class="card">
  <h2>Music Player</h2>
  <div id="music-list" style="max-height:300px;overflow-y:auto;font-size:13px"></div>
  <div id="play-status" style="font-size:12px;color:#3498db;margin-top:6px">Stopped</div>
</div>
</div>

<!-- Files Tab -->
<div id="tab-files" class="hidden">
<div class="card">
  <h2>File Manager</h2>
  <div id="fm_breadcrumb" style="font-size:13px;color:#e94560;margin-bottom:4px;">/</div>
  <div id="fm_capacity" style="font-size:11px;color:#666;margin-bottom:4px;"></div>
  <div id="fm_list" style="max-height:400px;overflow-y:auto;font-size:13px"></div>
  <div id="fm_status" class="status"></div>
</div>
</div>

<!-- System Tab -->
<div id="tab-system" class="hidden">
<div class="card">
  <h2>System Info</h2>
  <table id="sys-info"></table>
</div>
<div class="card">
  <h2>Volume</h2>
  <div class="flex">
    <input type="range" id="volume" min="0" max="100" value="60" oninput="onVolumeSlide(this.value)" style="flex:1">
    <span id="volume-val" style="font-size:20px;">60</span>
  </div>
  <div id="vol-status" class="status"></div>
</div>
<div class="card">
  <h2>ULog Record</h2>
  <div class="flex">
    <button class="sm green" id="btn-ulog" onclick="ulogToggle()">▶</button>
    <span id="ulog-status" style="font-size:12px;color:#3498db">Stopped</span>
  </div>
</div>
</div>

<script>
var fmCurrentDir='/';
var _scanTimer=null, _musicTimer=null, _sysInfoTimer=null, _ulogTimer=null, _recTimer=null;

async function apiGet(p) { try { let r=await fetch(p); return await r.json(); } catch(e) { return {error:e.message}; } }
async function apiPost(p, b) { try { let r=await fetch(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}); return await r.json(); } catch(e) { return {error:e.message}; } }

function switchTab(t) {
  ['wifi','audio','files','system'].forEach(x=>{
    document.getElementById('tab-'+x).classList.toggle('hidden', x!==t);
  });
  document.querySelectorAll('.tab').forEach(el=>el.classList.toggle('active', el.textContent.trim().toLowerCase().indexOf(t)>=0 || (t==='system' && el.textContent.trim()==='System')));
  if(_scanTimer){clearInterval(_scanTimer);_scanTimer=null}
  if(_musicTimer){clearInterval(_musicTimer);_musicTimer=null}
  if(_sysInfoTimer){clearInterval(_sysInfoTimer);_sysInfoTimer=null}
  if(_ulogTimer){clearInterval(_ulogTimer);_ulogTimer=null}
  if(_recTimer){clearInterval(_recTimer);_recTimer=null}
  if(t==='wifi'){scanWiFi();_scanTimer=setInterval(scanWiFi,5000)}
  if(t==='audio'){refreshRecStatus();loadMusicList();_recTimer=setInterval(refreshRecStatus,2000);_musicTimer=setInterval(loadMusicList,2000)}
  if(t==='files') loadFileManager('/');
  if(t==='system'){refreshSysInfo();_sysInfoTimer=setInterval(refreshSysInfo,2000);refreshUlogStatus();_ulogTimer=setInterval(refreshUlogStatus,2000)}
}

/* ── WiFi: auto-scan + Connect button + password modal ── */
var _modalSsid='';
async function scanWiFi() {
  let d=await apiGet('/api/wifi/scan');
  if(d.error) return;
  let st=await apiGet('/api/wifi/status');
  let statusEl=document.getElementById('wifi-status');
  if(st.connected) statusEl.innerHTML='<span style="color:#2ecc71">Connected: '+st.ssid+' ('+st.ip+')</span>';
  else statusEl.innerHTML='<span style="color:#e74c3c">Disconnected</span>';
  let h='<tr><td>SSID</td><td>Signal</td><td></td></tr>';
  if(d.networks) d.networks.forEach(n=>{
    let ej=JSON.stringify(n.ssid);
    let dn=n.ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
    let lockIcon=n.authmode>0?'🔒':'';
    h+='<tr><td>'+lockIcon+' '+dn+'</td><td>'+n.rssi+' dBm</td><td><button class="sm green" onclick=\'selectWifi('+ej+')\'>Connect</button></td></tr>';
  });
  document.getElementById('scan-results').innerHTML=h;
}
function selectWifi(ssid) {
  _modalSsid=ssid;
  document.getElementById('modal-ssid-label').textContent='Connect to: '+ssid;
  document.getElementById('modal-password').value='';
  document.getElementById('wifi-modal').classList.add('show');
}
function closeWifiModal() {
  document.getElementById('wifi-modal').classList.remove('show');
  _modalSsid='';
}
async function connectFromModal() {
  let pass=document.getElementById('modal-password').value;
  let connectSsid=_modalSsid;  /* Save SSID before closeWifiModal() clears it */
  closeWifiModal();
  document.getElementById('wifi-status').textContent='Connecting to '+connectSsid+'...';
  let d=await apiPost('/api/wifi/connect',{ssid:connectSsid,password:pass});
  if(d.error) {
    document.getElementById('wifi-status').innerHTML='<span class="error">'+d.error+'</span>';
    return;
  }
  /* Non-blocking: server returns {status:"connecting"} immediately.
   * Poll /api/wifi/status until connected or timeout (20s). */
  let attempts=0;
  let pollId=setInterval(async function(){
    let st=await apiGet('/api/wifi/status');
    if(st.error) return; /* Skip failed fetches, don't count as attempt */
    attempts++;
    if(st.connected) {
      clearInterval(pollId);
      document.getElementById('wifi-status').innerHTML='<span style="color:#2ecc71">Connected: '+st.ssid+' ('+st.ip+')</span>';
      scanWiFi();
    } else if(attempts>=40) {
      clearInterval(pollId);
      document.getElementById('wifi-status').innerHTML='<span class="error">Connection timed out</span>';
      scanWiFi();
    }
  },500);
}

/* ── Volume (real-time slider with NVS debounce) ── */
var _volDebounceTimer=null;
function onVolumeSlide(v) {
  document.getElementById('volume-val').textContent=v;
  apiPost('/api/audio/volume?save=false',{volume:parseInt(v)});
  if(_volDebounceTimer) clearTimeout(_volDebounceTimer);
  _volDebounceTimer=setTimeout(function(){
    apiPost('/api/audio/volume',{volume:parseInt(v)});
    document.getElementById('vol-status').textContent='Volume saved: '+v;
    setTimeout(function(){document.getElementById('vol-status').textContent=''},2000);
  },1000);
}

/* ── Audio Recording ── */
var s_recording=false;
var _recTimer=null;
function applyRecStatus(d) {
  let wasRecording=s_recording;
  s_recording=!!d.recording;
  if(s_recording) {
    document.getElementById('btn-rec').textContent='■';
    document.getElementById('btn-rec').className='red';
    let info='Recording: '+(d.seconds||0)+'s';
    if(d.file) info+=', file: '+d.file+' ('+Math.round((d.bytes||0)/1024)+' KB)';
    document.getElementById('rec-status').textContent=info;
    document.getElementById('rec-status').style.color='#e74c3c';
  } else {
    document.getElementById('btn-rec').textContent='▶';
    document.getElementById('btn-rec').className='green';
    if(wasRecording && d.file) {
      document.getElementById('rec-status').textContent='Stopped, file: '+d.file;
      document.getElementById('rec-status').style.color='#3498db';
    } else {
      document.getElementById('rec-status').textContent='Stopped';
      document.getElementById('rec-status').style.color='#3498db';
    }
  }
}
async function refreshRecStatus() {
  let d=await apiGet('/api/audio/record_status');
  applyRecStatus(d);
}
async function recToggle() {
  if(s_recording) {
    /* Optimistic: show Stopped immediately */
    applyRecStatus({recording:0});
    let d=await apiGet('/api/audio/record_stop');
    if(d.ok) applyRecStatus(d); /* Sync from command response (has file/bytes) */
    else refreshRecStatus(); /* Revert on error */
  } else {
    /* Optimistic: show Recording immediately */
    applyRecStatus({recording:1,seconds:0});
    let d=await apiGet('/api/audio/record_start');
    if(d.ok) applyRecStatus(d); /* Sync from command response (has file) */
    else { applyRecStatus({recording:0}); document.getElementById('rec-status').innerHTML='<span class="error">'+(d.error||'Failed')+'</span>'; }
  }
}

/* ── Music Playback (single Start/Stop toggle, auto-refresh list + play_status) ── */
var s_currentPlaying='', s_currentIdx=-1;
function applyPlayStatus(ps) {
  if(ps.playing) {
    if(ps.file && ps.file.length>0 && s_currentPlaying!==ps.file) {
      s_currentPlaying=ps.file;
      s_currentIdx=-1;
    }
    document.getElementById('play-status').textContent=s_currentPlaying?('Playing: '+s_currentPlaying):'Playing';
    document.getElementById('play-status').style.color='#2ecc71';
  } else if(s_currentPlaying) {
    s_currentPlaying=''; s_currentIdx=-1;
    document.getElementById('play-status').textContent='Stopped';
    document.getElementById('play-status').style.color='#3498db';
  }
}
async function refreshPlayStatus() {
  let ps=await apiGet('/api/audio/play_status');
  applyPlayStatus(ps);
}
async function loadMusicList() {
  /* Poll play_status to sync UI with actual device state */
  await refreshPlayStatus();
  let d=await apiGet('/api/audio/list');
  let h='';
  if(d.files && d.files.length>0) {
    d.files.sort(function(a,b){return a.localeCompare(b)});
    d.files.forEach(function(f,i){
      let ej=JSON.stringify(f);
      let dn=f.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
      let isCurrentPlaying=(s_currentPlaying===f);
      h+='<div style="display:flex;justify-content:space-between;align-items:center;padding:4px 8px;border-bottom:1px solid #0f3460">';
      h+='<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">'+dn+'</span>';
      h+='<button class="sm '+(isCurrentPlaying?'red':'green')+'" id="btn_play_'+i+'" onclick=\'playToggle('+ej+','+i+')\'>'+(isCurrentPlaying?'■':'▶')+'</button>';
      h+='</div>';
      if(isCurrentPlaying) s_currentIdx=i;
    });
  } else h='<div style="color:#666;padding:8px">No audio files</div>';
  document.getElementById('music-list').innerHTML=h;
}
async function playToggle(fn,idx) {
  if(s_currentPlaying===fn) {
    /* Optimistic: show Stopped immediately */
    applyPlayStatus({playing:false});
    let d=await apiGet('/api/audio/stop');
    if(d.ok) applyPlayStatus(d);
    else refreshPlayStatus();
    loadMusicList();
  } else {
    if(s_currentPlaying) {
      /* Optimistic: stop current first */
      applyPlayStatus({playing:false});
      await apiGet('/api/audio/stop');
    }
    /* Optimistic: show Loading immediately */
    s_currentPlaying=fn; s_currentIdx=idx;
    document.getElementById('play-status').textContent='Loading: '+fn;
    document.getElementById('play-status').style.color='#aaa';
    let d=await apiGet('/api/audio/play?file='+encodeURIComponent(fn));
    if(d.ok) {
      applyPlayStatus(d);
      loadMusicList();
    } else {
      s_currentPlaying=''; s_currentIdx=-1;
      document.getElementById('play-status').textContent='Play failed';
      document.getElementById('play-status').style.color='#e74c3c';
    }
  }
}

/* ── File Manager ── */
var fmCurrentDir='/';
async function loadFileManager(d) {
  if(typeof d!=='undefined') fmCurrentDir=d;
  try{var r=await fetch('/api/files/list?dir='+encodeURIComponent(fmCurrentDir));
  var j=await r.json();
  if(!j.ok){document.getElementById('fm_breadcrumb').textContent='Error';return}
  document.getElementById('fm_breadcrumb').textContent=j.current||'/';
  var cap='';
  if(j.total_kb&&j.total_kb>0){var free=j.free_kb, total=j.total_kb, used=total-free;
  var u=used>1048576?(used/1048576).toFixed(1)+'GB':used>1024?(used/1024).toFixed(1)+'MB':used+'KB';
  var t=total>1048576?(total/1048576).toFixed(1)+'GB':total>1024?(total/1024).toFixed(1)+'MB':total+'KB';
  cap=u+' used / '+t}
  else if(j.total_kb!==undefined)cap='Capacity unknown';
  document.getElementById('fm_capacity').textContent=cap;
  var h='';
  if(fmCurrentDir!=='/') h+='<div style="display:flex;align-items:center;padding:3px 0;border-bottom:1px solid #0f3460;cursor:pointer;color:#00d4ff" onclick="navigateUp()">📁 ..</div>';
  if(j.files) j.files.sort(function(a,b){if(a.is_dir!=b.is_dir)return a.is_dir?-1:1;return a.name.localeCompare(b.name)});
  if(j.files&&j.files.length)
  for(var i=0;i<j.files.length;i++){
  var f=j.files[i];var icon=f.is_dir?'📁':'📄';
  var sz=f.size>1048576?(f.size/1048576).toFixed(1)+'MB':f.size>1024?Math.round(f.size/1024)+'KB':f.size+'B';
  var ej=JSON.stringify(f.name);var dn=f.name.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
  if(f.is_dir) {
    h+='<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 0;border-bottom:1px solid #0f3460">';
    h+='<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:pointer" onclick=\'navigateTo('+ej+')\'>'+icon+' '+dn+'</span>';
    h+='<span style="display:flex;gap:4px;margin-left:8px">';
    h+='<button class="sm" onclick=\'navigateTo('+ej+')\'>Open</button>';
    h+='<button class="sm red" onclick=\'deleteFile('+ej+')\'>🗑</button>';
    h+='</span></div>';
  } else {
    h+='<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 0;border-bottom:1px solid #0f3460">';
    h+='<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">'+icon+' '+dn+' <span style="color:#666;font-size:10px">'+sz+'</span></span>';
    h+='<span style="display:flex;gap:4px;margin-left:8px">';
    h+='<button class="sm" onclick=\'downloadFile('+ej+')\'>⬇</button>';
    h+='<button class="sm red" onclick=\'deleteFile('+ej+')\'>🗑</button>';
    h+='</span></div>';
  }
  }
  document.getElementById('fm_list').innerHTML=h}
  catch(e){document.getElementById('fm_breadcrumb').textContent='Load failed'}}

function navigateTo(name) {
  var p=fmCurrentDir;if(p[p.length-1]!=='/')p+='/';
  loadFileManager(p+name)}
function navigateUp() {
  if(fmCurrentDir=='/'||fmCurrentDir=='')return;
  var p=fmCurrentDir;if(p[p.length-1]=='/')p=p.slice(0,-1);
  var i=p.lastIndexOf('/');
  loadFileManager(i<0?'/':p.substring(0,i)||'/')}
function downloadFile(name) {
  var p=fmCurrentDir;if(p[p.length-1]!=='/')p+='/';
  document.getElementById('fm_status').textContent='Downloading: '+name+'...';
  var a=document.createElement('a');a.href='/api/files/download?path='+encodeURIComponent(p+name);a.download=name;a.click();
  setTimeout(function(){document.getElementById('fm_status').textContent='Downloaded: '+name},2000);
}
async function deleteFile(name) {
  var p=fmCurrentDir;if(p[p.length-1]!=='/')p+='/';
  if(!confirm('Delete '+name+'?'))return;
  document.getElementById('fm_status').textContent='Deleting: '+name+'...';
  try{var r=await fetch('/api/files/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:p+name})});
  var j=await r.json();
  if(j.ok){document.getElementById('fm_status').textContent='Deleted: '+name;loadFileManager()}
  else document.getElementById('fm_status').innerHTML='<span class="error">Error: '+j.error+'</span>'}
  catch(e){document.getElementById('fm_status').innerHTML='<span class="error">Error</span>'}}

/* ── ULog ── */
var s_ulogRunning=false;
function applyUlogStatus(d) {
  s_ulogRunning=!!d.running;
  document.getElementById('btn-ulog').textContent=s_ulogRunning?'■':'▶';
  document.getElementById('btn-ulog').className='sm '+(s_ulogRunning?'red':'green');
  let info=s_ulogRunning?'Recording':'Stopped';
  if(d.filepath && d.filepath.length>0) {
    let kb=Math.round((d.bytes_written||0)/1024);
    info+=', file: '+d.filepath+' ('+kb+' KB)';
  }
  document.getElementById('ulog-status').textContent=info;
  document.getElementById('ulog-status').style.color=s_ulogRunning?'#e74c3c':'#3498db';
}
async function ulogToggle() {
  if(s_ulogRunning) {
    /* Optimistic: show Stopped immediately */
    applyUlogStatus({running:false});
    let d=await apiPost('/api/ulog/stop',{});
    if(d.ok) applyUlogStatus(d); /* Sync from command response (has filepath/bytes) */
    else refreshUlogStatus(); /* Revert on error */
  } else {
    /* Optimistic: show Recording immediately */
    applyUlogStatus({running:true});
    let d=await apiPost('/api/ulog/start',{});
    if(d.ok) applyUlogStatus(d); /* Sync from command response (has filepath) */
    else { applyUlogStatus({running:false}); document.getElementById('ulog-status').innerHTML='<span class="error">'+(d.error||'Failed')+'</span>'; }
  }
}
async function refreshUlogStatus() {
  let d=await apiGet('/api/ulog/status');
  applyUlogStatus(d);
}

/* ── System Info (auto-refresh) ── */
async function refreshSysInfo() {
  let d=await apiGet('/api/system/info');
  let h='';
  if(d.error) h='<tr><td colspan="2" class="error">'+d.error+'</td></tr>';
  else {
    h+='<tr><td>Chip</td><td>'+(d.chip||'N/A')+'</td></tr>';
    h+='<tr><td>CPU</td><td>'+(d.cpu_freq||'N/A')+' MHz</td></tr>';
    h+='<tr><td>Flash</td><td>'+(d.flash_size||'N/A')+' MB</td></tr>';
    h+='<tr><td>PSRAM</td><td>'+(d.psram_size||'N/A')+' MB</td></tr>';
    h+='<tr><td>SDK</td><td>'+(d.sdk_version||'N/A')+'</td></tr>';
    h+='<tr><td>WiFi</td><td>'+(d.wifi_connected?'Connected '+d.wifi_ssid:'Disconnected')+'</td></tr>';
    h+='<tr><td>SD Card</td><td>'+(d.sdcard_mounted?'Mounted':'Not detected')+'</td></tr>';
    h+='<tr><td>Free Heap</td><td>'+(d.free_heap||'N/A')+' KB</td></tr>';
    h+='<tr><td>Uptime</td><td>'+(d.uptime||'N/A')+' s</td></tr>';
    h+='<tr><td>NTP</td><td>'+(d.sntp_synced?'Synced':'Not synced')+'</td></tr>';
    h+='<tr><td>Timezone</td><td>'+(d.timezone||'N/A')+'</td></tr>';
    if(d.current_time) h+='<tr><td>Time</td><td>'+d.current_time+'</td></tr>';
  }
  document.getElementById('sys-info').innerHTML=h;
}

/* ── Init ── */
(async function(){
  let d=await apiGet('/api/audio/volume');
  if(!d.error && d.volume!==undefined){
    document.getElementById('volume').value=d.volume;
    document.getElementById('volume-val').textContent=d.volume;
  }
  /* Sync recording and playback state from device on page load */
  refreshRecStatus();
  refreshPlayStatus();
  scanWiFi();
  _scanTimer=setInterval(scanWiFi,5000);
})();
</script>
</body>
</html>
)rawliteral";

/* ── REST API handlers ──────────────────────────────────────────── */

/* GET /api/wifi/scan */
static esp_err_t _api_wifi_scan(httpd_req_t *req) {
    wifi_scan_result_t results[WIFI_SERVICE_SCAN_MAX];
    uint16_t count = 0;
    esp_err_t err = WifiService::instance().scan(results, &count);

    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", "Scan failed");
    } else {
        cJSON *networks = cJSON_AddArrayToObject(root, "networks");
        for (uint16_t i = 0; i < count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", results[i].ssid);
            cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
            cJSON_AddNumberToObject(item, "authmode", results[i].authmode);
            cJSON_AddItemToArray(networks, item);
        }
        cJSON_AddNumberToObject(root, "count", count);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/wifi/connect */
static esp_err_t _api_wifi_connect(httpd_req_t *req) {
    /* Read full body with Content-Length awareness.
     * httpd_req_recv may return partial data, so loop until done. */
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
        return ESP_FAIL;
    }

    char *buf = (char *)calloc(1, content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int total_received = 0;
    while (total_received < content_len) {
        int ret = httpd_req_recv(req, buf + total_received, content_len - total_received);
        if (ret <= 0) {
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Recv timeout");
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv error");
            }
            return ESP_FAIL;
        }
        total_received += ret;
    }
    buf[total_received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    const char *ssid = ssid_item ? ssid_item->valuestring : nullptr;
    const char *pass = pass_item ? pass_item->valuestring : nullptr;

    ESP_LOGI(TAG, "WiFi connect request: ssid='%s'", ssid ? ssid : "(null)");

    cJSON *resp = cJSON_CreateObject();
    if (!ssid) {
        cJSON_AddStringToObject(resp, "error", "Missing SSID");
    } else if (strlen(ssid) >= 32) {
        cJSON_AddStringToObject(resp, "error", "SSID too long (max 31 chars)");
    } else if (pass && strlen(pass) > 0 && strlen(pass) < 8) {
        cJSON_AddStringToObject(resp, "error", "Password must be at least 8 characters");
    } else {
        /* Non-blocking: apply STA config and return immediately.
         * Client should poll GET /api/wifi/status to check connection.
         * This avoids blocking the httpd task and prevents AP disruption
         * from killing the HTTP connection mid-response. */
        esp_err_t err = WifiService::instance().connect(ssid, pass);
        if (err == ESP_OK) {
            cJSON_AddStringToObject(resp, "status", "connecting");
            cJSON_AddStringToObject(resp, "ssid", ssid);
        } else {
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "Failed to apply config: %s", esp_err_to_name(err));
            cJSON_AddStringToObject(resp, "error", err_msg);
        }
    }

    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/wifi/status */
static esp_err_t _api_wifi_status(httpd_req_t *req) {
    wifi_service_status_t st;
    WifiService::instance().get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", st.sta_connected);
    cJSON_AddBoolToObject(root, "configured", st.sta_configured);
    cJSON_AddStringToObject(root, "ssid", st.sta_ssid);
    cJSON_AddStringToObject(root, "ip", st.sta_ip);
    cJSON_AddNumberToObject(root, "rssi", st.sta_rssi);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/system/info */
static esp_err_t _api_system_info(httpd_req_t *req) {
    /* Ensure timezone is loaded before reporting */
    _load_timezone_from_nvs();

    cJSON *root = cJSON_CreateObject();

    /* Chip info */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "chip", "ESP32-S31");
    cJSON_AddNumberToObject(root, "cpu_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "cpu_freq", (chip_info.cores > 0) ? 320 : 0);

    /* Flash size */
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        cJSON_AddNumberToObject(root, "flash_size", flash_size / (1024 * 1024));
    }

    /* PSRAM */
    uint32_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    cJSON_AddNumberToObject(root, "psram_size", psram_size / (1024 * 1024));

    /* SDK version */
    cJSON_AddStringToObject(root, "sdk_version", esp_get_idf_version());

    /* WiFi status */
    wifi_service_status_t st;
    WifiService::instance().get_status(&st);
    cJSON_AddBoolToObject(root, "wifi_connected", st.sta_connected);
    cJSON_AddStringToObject(root, "wifi_ssid", st.sta_ssid);
    cJSON_AddStringToObject(root, "wifi_ip", st.sta_ip);

    /* SD card */
    bool sd_ok = SDCardDriver::instance().available();
    cJSON_AddBoolToObject(root, "sdcard_mounted", sd_ok);

    /* Bluetooth audio */
    bool bt_ok = BtAudioDriver::instance().available();
    cJSON_AddBoolToObject(root, "bt_audio", bt_ok);
    if (bt_ok) {
        cJSON_AddBoolToObject(root, "bt_connected", BtAudioDriver::instance().connected());
        cJSON_AddBoolToObject(root, "bt_streaming", BtAudioDriver::instance().streaming());
        cJSON_AddStringToObject(root, "bt_device", BtAudioDriver::instance().device_name()[0]
                                ? BtAudioDriver::instance().device_name() : "");
        cJSON_AddStringToObject(root, "bt_device_addr", BtAudioDriver::instance().device_address()[0]
                                ? BtAudioDriver::instance().device_address() : "");
        cJSON_AddStringToObject(root, "bt_playback", BtAudioDriver::instance().playback_status_str());
        cJSON_AddStringToObject(root, "bt_title", BtAudioDriver::instance().metadata_title()[0]
                                ? BtAudioDriver::instance().metadata_title() : "");
        cJSON_AddStringToObject(root, "bt_artist", BtAudioDriver::instance().metadata_artist()[0]
                                ? BtAudioDriver::instance().metadata_artist() : "");
    }

    /* Free heap */
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size() / 1024);

    /* Uptime */
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);

    /* SNTP status */
    cJSON_AddBoolToObject(root, "sntp_synced", WifiService::instance().sntp_synced());

    /* Timezone (snapshot under mutex to avoid race with concurrent POST) */
    {
        char tz_snap[32] = {};
        if (s_timezone_mutex && xSemaphoreTake(s_timezone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
            xSemaphoreGive(s_timezone_mutex);
        } else {
            strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
        }
        cJSON_AddStringToObject(root, "timezone", tz_snap);
    }

    /* Current wall-clock time (if synced) */
    if (WifiService::instance().sntp_synced()) {
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddStringToObject(root, "current_time", time_str);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/system/timezone */
static esp_err_t _api_timezone_get(httpd_req_t *req) {
    _load_timezone_from_nvs();

    cJSON *root = cJSON_CreateObject();

    /* Timezone (snapshot under mutex) */
    {
        char tz_snap[32] = {};
        if (s_timezone_mutex && xSemaphoreTake(s_timezone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
            xSemaphoreGive(s_timezone_mutex);
        } else {
            strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
        }
        cJSON_AddStringToObject(root, "timezone", tz_snap);
    }
    cJSON_AddBoolToObject(root, "sntp_synced", WifiService::instance().sntp_synced());

    if (WifiService::instance().sntp_synced()) {
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddStringToObject(root, "current_time", time_str);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/system/timezone — body: {"timezone": "CST-8"} */
static esp_err_t _api_timezone_set(httpd_req_t *req) {
    /* Validate Content-Length to prevent buffer overflow */
    if (req->content_len > 127) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    char buf[128] = {};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *tz_item = cJSON_GetObjectItem(json, "timezone");
    if (!tz_item || !cJSON_IsString(tz_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'timezone' field");
        return ESP_FAIL;
    }

    const char *new_tz = tz_item->valuestring;
    if (strlen(new_tz) == 0 || strlen(new_tz) >= sizeof(s_timezone)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timezone value");
        return ESP_FAIL;
    }

    /* Apply timezone (protected by mutex against concurrent reads) */
    char tz_snap[32] = {};
    if (s_timezone_mutex) xSemaphoreTake(s_timezone_mutex, portMAX_DELAY);
    strlcpy(s_timezone, new_tz, sizeof(s_timezone));
    strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
    setenv("TZ", s_timezone, 1);
    tzset();
    if (s_timezone_mutex) xSemaphoreGive(s_timezone_mutex);

    /* Persist to NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_TIMEZONE, tz_snap);
        nvs_commit(h);
        nvs_close(h);
    }

    cJSON_Delete(json);
    ESP_LOGI(TAG, "Timezone set to: %s", tz_snap);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "timezone", tz_snap);
    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* GET /api/system/stats */
static esp_err_t _api_system_stats(httpd_req_t *req) {
    system_stats_s stats = SystemMonitor::instance().get_latest();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cpu0_pct", stats.core0_cpu_pct);
    cJSON_AddNumberToObject(root, "cpu1_pct", stats.core1_cpu_pct);
    cJSON_AddNumberToObject(root, "total_cpu_pct", stats.total_cpu_pct);
    cJSON_AddNumberToObject(root, "free_internal_kb", stats.free_internal / 1024);
    cJSON_AddNumberToObject(root, "free_psram_kb", stats.free_psram / 1024);
    cJSON_AddNumberToObject(root, "min_free_internal_kb", stats.min_free_internal / 1024);
    cJSON_AddNumberToObject(root, "min_free_psram_kb", stats.min_free_psram / 1024);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/audio/volume — ?save=false skips NVS write for real-time slider */
static esp_err_t _api_audio_volume_set(httpd_req_t *req) {
    /* Validate Content-Length to prevent buffer overflow */
    if (req->content_len > 127) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    /* Check ?save=false query param */
    bool save_nvs = true;
    char q[32] = {};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strlen(q) > 0) {
        char val[8] = {};
        if (httpd_query_key_value(q, "save", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "false") == 0) save_nvs = false;
        }
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *vol_item = cJSON_GetObjectItem(root, "volume");

    cJSON *resp = cJSON_CreateObject();
    if (!vol_item) {
        cJSON_AddStringToObject(resp, "error", "Missing volume parameter");
    } else {
        int vol = vol_item->valueint;
        if (vol < VOLUME_MIN) vol = VOLUME_MIN;
        if (vol > VOLUME_MAX) vol = VOLUME_MAX;

        AudioDriver::instance().set_volume(vol);

        /* Save to NVS only when save_nvs is true (debounced call) */
        if (save_nvs) {
            nvs_handle_t h;
            if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_i32(h, NVS_KEY_VOLUME, vol);
                nvs_commit(h);
                nvs_close(h);
            }
        }

        cJSON_AddNumberToObject(resp, "volume", vol);
    }

    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/audio/volume */
static esp_err_t _api_audio_volume_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "volume", AudioDriver::instance().volume());

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/bt/status */
static esp_err_t _api_bt_status(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "available", BtAudioDriver::instance().available());
    if (BtAudioDriver::instance().available()) {
        cJSON_AddBoolToObject(root, "connected", BtAudioDriver::instance().connected());
        cJSON_AddBoolToObject(root, "streaming", BtAudioDriver::instance().streaming());
        cJSON_AddStringToObject(root, "device_name", BtAudioDriver::instance().device_name());
        cJSON_AddStringToObject(root, "device_addr", BtAudioDriver::instance().device_address());
        cJSON_AddStringToObject(root, "playback_status", BtAudioDriver::instance().playback_status_str());
        cJSON_AddStringToObject(root, "title", BtAudioDriver::instance().metadata_title());
        cJSON_AddStringToObject(root, "artist", BtAudioDriver::instance().metadata_artist());
    }
    char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/sdcard/info */
static esp_err_t _api_sdcard_info(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mounted", SDCardDriver::instance().available());
    cJSON_AddStringToObject(root, "mountpoint", SDMMC_MOUNT_POINT);

    if (SDCardDriver::instance().available()) {
        /* Get filesystem capacity via fatfs (same as _api_files_list) */
        FATFS *fs; DWORD free_clust;
        if (f_getfree("", &free_clust, &fs) == FR_OK && fs) {
            uint64_t total_kb = (uint64_t)(fs->n_fatent - 2) * fs->csize * fs->ssize / 1024;
            uint64_t free_kb = (uint64_t)free_clust * fs->csize * fs->ssize / 1024;
            cJSON_AddNumberToObject(root, "total_kb", (double)total_kb);
            cJSON_AddNumberToObject(root, "free_kb", (double)free_kb);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Audio Recording / Playback / File Manager / ULog handlers ── */

/* GET /api/audio/record_start */
static esp_err_t _api_rec_start(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    audio_lock();
    if (s_is_recording) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":1}"); return ESP_OK; }
    if (AudioUlogRecorder::instance().running()) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"ULog audio recording is active\"}"); return ESP_OK; }
    if (s_fm_busy) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"File manager busy\"}"); return ESP_OK; }
    if (!AudioDriver::instance().available()) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Audio not available\"}"); return ESP_OK; }
    if (!s_audio_task.load(std::memory_order_acquire)) {
        s_audio_running = true;
        s_audio_stack = (StackType_t *)heap_caps_malloc(12 * 1024 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_audio_stack) { s_audio_running = false; audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
        if (!s_audio_tcb) {
            ESP_LOGE(TAG, "Audio TCB not allocated — audio task cannot start");
            heap_caps_free(s_audio_stack); s_audio_stack = NULL;
            s_audio_running = false;
            audio_unlock();
            httpd_resp_sendstr(req, "{\"ok\":0}");
            return ESP_OK;
        }
        TaskHandle_t h = xTaskCreateStaticPinnedToCore(audio_task, "w_audio", 12 * 1024, NULL, 1, s_audio_stack, s_audio_tcb, 1);
        s_audio_task.store(h, std::memory_order_release);
        if (!h) { heap_caps_free(s_audio_stack); s_audio_stack = NULL; s_audio_running = false; audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    }

    /* Open AAC encoder */
    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate = 16000;
    aac_cfg.channel = 2;
    aac_cfg.bits_per_sample = 16;
    aac_cfg.bitrate = 64000;   /* 64kbps — suitable for 16kHz stereo AAC */
    aac_cfg.adts_used = true;

    esp_audio_enc_config_t enc_cfg = {};
    enc_cfg.type = ESP_AUDIO_TYPE_AAC;
    enc_cfg.cfg = &aac_cfg;
    enc_cfg.cfg_sz = sizeof(aac_cfg);

    if (esp_audio_enc_open(&enc_cfg, &s_encoder) != ESP_AUDIO_ERR_OK || !s_encoder) {
        ESP_LOGE(TAG, "AAC encoder open failed");
        audio_unlock(); _stop_audio_task_if_running(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
    }
    if (esp_audio_enc_get_frame_size(s_encoder, &s_enc_in_size, &s_enc_out_size) != ESP_AUDIO_ERR_OK || s_enc_in_size <= 0) {
        ESP_LOGE(TAG, "AAC get_frame_size failed");
        esp_audio_enc_close(s_encoder); s_encoder = nullptr;
        audio_unlock(); _stop_audio_task_if_running(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
    }
    ESP_LOGI(TAG, "AAC encoder: in=%d out=%d", s_enc_in_size, s_enc_out_size);

    s_enc_in_buf = (uint8_t*)heap_caps_calloc(1, s_enc_in_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_enc_out_buf = (uint8_t*)heap_caps_calloc(1, s_enc_out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_enc_in_buf || !s_enc_out_buf) {
        ESP_LOGE(TAG, "Encoder buffer alloc failed");
        esp_audio_enc_close(s_encoder); s_encoder = nullptr;
        audio_unlock(); _stop_audio_task_if_running(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
    }
    s_enc_in_count = 0;

    /* Generate .aac filename */
    struct timeval tv; gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec; struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    if (tm_buf.tm_year + 1900 > 2020) {
        snprintf(s_rec_path, sizeof(s_rec_path), SDMMC_MOUNT_POINT "/rec_%04d%02d%02d_%02d%02d%02d.aac",
                 tm_buf.tm_year+1900, tm_buf.tm_mon+1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        uint32_t mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(s_rec_path, sizeof(s_rec_path), SDMMC_MOUNT_POINT "/rec_%lu.aac", (unsigned long)mono_ms);
    }

    s_rec_file = fopen(s_rec_path, "wb");
    if (!s_rec_file) {
        ESP_LOGE(TAG, "Cannot create file");
        esp_audio_enc_close(s_encoder); s_encoder = nullptr;
        heap_caps_free(s_enc_in_buf); s_enc_in_buf = NULL;
        heap_caps_free(s_enc_out_buf); s_enc_out_buf = NULL;
        audio_unlock(); _stop_audio_task_if_running(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
    }

    s_rec_bytes = 0;
    s_rec_start_ms.store((uint32_t)(esp_timer_get_time() / 1000), std::memory_order_relaxed);
    s_is_recording = true;

    /* Stop any active playback — recording and playback share I2S.
     * Capture the old handle and clear globals under the lock, then
     * stop/destroy outside the lock (may block). No re-check needed:
     * any new playback started while unlocked uses a different handle. */
    if (s_asp && s_playing) {
        esp_asp_handle_t old = s_asp; s_asp = NULL; s_playing = false;
        s_playing_file[0] = '\0';
        audio_unlock();
        esp_audio_simple_player_stop(old);
        esp_audio_simple_player_destroy(old);
        audio_lock();
    }
    /* Snapshot s_rec_path before unlock — concurrent rec_stop could overwrite it */
    char rec_path_snap[128]; strlcpy(rec_path_snap, s_rec_path, sizeof(rec_path_snap));
    audio_unlock();
    ESP_LOGI(TAG, "Recording: %s", rec_path_snap);
    /* Return status so client can update UI immediately without a second round-trip */
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "recording", true);
        cJSON_AddStringToObject(root, "file", rec_path_snap);
        char *j = cJSON_PrintUnformatted(root);
        if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
        else httpd_resp_sendstr(req, "{\"ok\":1,\"recording\":true}");
        cJSON_Delete(root);
    } else httpd_resp_sendstr(req, "{\"ok\":1,\"recording\":true}");
    return ESP_OK;
}

/* GET /api/audio/record_stop */
static esp_err_t _api_rec_stop(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    audio_lock();
    if (!s_is_recording) {
        _stop_audio_task_if_running();
        audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":1}"); return ESP_OK;
    }
    s_is_recording = false;
    _stop_audio_task_if_running();

    /* Close encoder */
    FILE *f = s_rec_file; s_rec_file = NULL;
    if (s_encoder) {
        /* Flush remaining encoder output */
        if (s_enc_out_buf) {
            esp_audio_enc_out_frame_t flush_out = {
                .buffer = s_enc_out_buf, .len = (uint32_t)s_enc_out_size
            };
            /* Send empty frame to flush */
            esp_audio_enc_in_frame_t empty_in = { .buffer = NULL, .len = 0 };
            esp_audio_enc_process(s_encoder, &empty_in, &flush_out);
            if (flush_out.encoded_bytes > 0 && f) {
                fwrite(flush_out.buffer, 1, flush_out.encoded_bytes, f);
                s_rec_bytes.fetch_add(flush_out.encoded_bytes, std::memory_order_relaxed);
            }
        }
        esp_audio_enc_close(s_encoder); s_encoder = nullptr;
    }
    if (s_enc_in_buf) { heap_caps_free(s_enc_in_buf); s_enc_in_buf = NULL; }
    if (s_enc_out_buf) { heap_caps_free(s_enc_out_buf); s_enc_out_buf = NULL; }

    /* Close file */
    if (f) fclose(f);

    char saved[128]; strlcpy(saved, s_rec_path, sizeof(saved));
    uint32_t total_bytes = s_rec_bytes.load(std::memory_order_relaxed);
    audio_unlock();
    /* Return status so client can update UI immediately */
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "recording", false);
        cJSON_AddStringToObject(root, "file", saved);
        cJSON_AddNumberToObject(root, "bytes", (double)total_bytes);
        char *j = cJSON_PrintUnformatted(root);
        if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
        else httpd_resp_sendstr(req, "{\"ok\":1,\"recording\":false}");
        cJSON_Delete(root);
    } else httpd_resp_sendstr(req, "{\"ok\":1,\"recording\":false}");
    ESP_LOGI(TAG, "Saved AAC: %s (%lu)", saved, (unsigned long)total_bytes);
    return ESP_OK;
}

/* GET /api/audio/record_status */
static esp_err_t _api_rec_status(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (s_is_recording) {
        audio_lock();
        uint32_t bytes = s_rec_bytes.load(std::memory_order_relaxed);
        uint32_t start_ms = s_rec_start_ms.load(std::memory_order_relaxed);
        char path_snap[128]; strlcpy(path_snap, s_rec_path, sizeof(path_snap));
        audio_unlock();
        uint32_t e = (uint32_t)((esp_timer_get_time() / 1000 - start_ms) / 1000);

        cJSON *root = cJSON_CreateObject();
        if (!root) { httpd_resp_sendstr(req, "{}"); return ESP_OK; }
        cJSON_AddBoolToObject(root, "recording", true);
        cJSON_AddNumberToObject(root, "seconds", (double)e);
        cJSON_AddNumberToObject(root, "bytes", (double)bytes);
        cJSON_AddStringToObject(root, "file", path_snap);
        char *j = cJSON_PrintUnformatted(root);
        if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
        else httpd_resp_sendstr(req, "{}");
        cJSON_Delete(root);
    } else httpd_resp_sendstr(req, "{\"recording\":0}");
    return ESP_OK;
}

/* GET /api/audio/list */
static esp_err_t _api_audio_list(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (!SDCardDriver::instance().available()) {
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}"); return ESP_OK;
    }
    DIR *d = opendir(SDMMC_MOUNT_POINT);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) { if (d) closedir(d); if (arr) cJSON_Delete(arr); if (root) cJSON_Delete(root); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    cJSON_AddItemToObject(root, "files", arr);
    if (d) { struct dirent *e; while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; char *x = strrchr(e->d_name, '.'); if (x && (strcasecmp(x, ".aac") == 0 || strcasecmp(x, ".wav") == 0 || strcasecmp(x, ".mp3") == 0)) cJSON_AddItemToArray(arr, cJSON_CreateString(e->d_name)); } closedir(d); }
    char *j = cJSON_PrintUnformatted(root);
    if (!j) { cJSON_Delete(root); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    httpd_resp_send(req, j, strlen(j));
    cJSON_free(j); cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/audio/play?file=xxx.wav */
static esp_err_t _api_play(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    audio_lock();
    if (!AudioDriver::instance().available()) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Audio not available\"}"); return ESP_OK; }
    char q[256] = {}, fn[128] = {};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || !strlen(q)) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    httpd_query_key_value(q, "file", fn, sizeof(fn));
    if (!strlen(fn)) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    _url_decode(fn);
    char safe[256];
    if (!_path_sanitize(fn, safe, sizeof(safe))) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid path\"}"); return ESP_OK; }
    char uri[300]; snprintf(uri, sizeof(uri), "file://%s", safe);
    if (s_fm_busy) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"File manager busy\"}"); return ESP_OK; }
    if (s_is_recording) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Recording in progress\"}"); return ESP_OK; }
    if (s_asp) {
        esp_asp_handle_t old = s_asp; s_asp = NULL; s_playing = false;
        s_playing_file[0] = '\0';
        audio_unlock();
        esp_audio_simple_player_stop(old);
        esp_audio_simple_player_destroy(old);
        audio_lock();
        if (s_is_recording) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Recording in progress\"}"); return ESP_OK; }
    }
    esp_asp_cfg_t c = {.out = {.cb = _asp_out}, .task_prio = 3, .task_stack = 8192, .task_core = 1, .task_stack_in_ext = true};
    if (esp_audio_simple_player_new(&c, &s_asp) != ESP_GMF_ERR_OK || !s_asp) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    esp_audio_simple_player_set_event(s_asp, _asp_evt, NULL);
    esp_gmf_err_t ret = esp_audio_simple_player_run(s_asp, uri, NULL);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Play failed: %d, uri=%s", ret, uri);
        esp_audio_simple_player_destroy(s_asp); s_asp = NULL;
        audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
    }
    s_playing = true;
    /* Store filename (basename only) for play_status */
    const char *base = strrchr(safe, '/');
    base = base ? base + 1 : safe;
    strlcpy(s_playing_file, base, sizeof(s_playing_file));
    audio_unlock();
    ESP_LOGI(TAG, "Play: %s", uri);
    /* Return status so client can update UI immediately */
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "playing", true);
        cJSON_AddStringToObject(root, "file", base);
        char *j = cJSON_PrintUnformatted(root);
        if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
        else httpd_resp_sendstr(req, "{\"ok\":1,\"playing\":true}");
        cJSON_Delete(root);
    } else httpd_resp_sendstr(req, "{\"ok\":1,\"playing\":true}");
    return ESP_OK;
}

/* GET /api/audio/stop */
static esp_err_t _api_stop(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    audio_lock();
    if (s_asp) {
        esp_audio_simple_player_stop(s_asp);
        esp_audio_simple_player_destroy(s_asp);
        s_asp = NULL;
        s_playing = false;
        s_playing_file[0] = '\0';
    }
    audio_unlock();
    httpd_resp_sendstr(req, "{\"ok\":1,\"playing\":false}"); return ESP_OK;
}

/* GET /api/audio/play_status */
static esp_err_t _api_play_status(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    audio_lock();
    bool playing = s_playing.load(std::memory_order_acquire);
    char file_snap[128]; strlcpy(file_snap, s_playing_file, sizeof(file_snap));
    audio_unlock();

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_sendstr(req, "{}"); return ESP_OK; }
    cJSON_AddBoolToObject(root, "playing", playing);
    cJSON_AddStringToObject(root, "file", file_snap);
    char *j = cJSON_PrintUnformatted(root);
    if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
    else httpd_resp_sendstr(req, "{}");
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── File Manager Handlers ── */

/* GET /api/files/list?dir=/ */
static esp_err_t _api_files_list(httpd_req_t *req) {
    if (!SDCardDriver::instance().available()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}"); return ESP_OK;
    }
    char q[256] = {};
    char raw_dir[128] = "/";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strlen(q) > 0) {
        httpd_query_key_value(q, "dir", raw_dir, sizeof(raw_dir));
    }
    if (raw_dir[0] == '\0') strlcpy(raw_dir, "/", sizeof(raw_dir));
    _url_decode(raw_dir);
    char dir[256];
    if (!_path_sanitize(raw_dir, dir, sizeof(dir))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid path\"}"); return ESP_OK;
    }
    DIR *d = opendir(dir);
    if (!d) { httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Cannot open directory\"}"); return ESP_OK; }

    uint64_t total_kb = 0, free_kb = 0;
    {   /* Get filesystem capacity via fatfs */
        FATFS *fs; DWORD free_clust;
        if (f_getfree("", &free_clust, &fs) == FR_OK && fs) {
            total_kb = (fs->n_fatent - 2) * fs->csize * fs->ssize / 1024;
            free_kb = free_clust * fs->csize * fs->ssize / 1024;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ok", 1);
    cJSON *files_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "files", files_arr);

    const char *display = dir;
    if (strncmp(dir, "/sdcard", 7) == 0) { display = dir + 7; if (display[0] == '\0') display = "/"; }
    cJSON_AddStringToObject(root, "current", display);
    cJSON_AddNumberToObject(root, "total_kb", (double)total_kb);
    cJSON_AddNumberToObject(root, "free_kb", (double)free_kb);

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char fpath[512]; snprintf(fpath, sizeof(fpath), "%s/%s", dir, e->d_name);
        /* Defense-in-depth: verify the constructed path is still within /sdcard
         * (prevents crafted directory names from escaping the mount point). */
        if (strncmp(fpath, "/sdcard/", 8) != 0 && strcmp(fpath, "/sdcard") != 0) continue;
        struct stat st;
        bool is_dir = false; int64_t fsize = 0; time_t mtime = 0;
        if (stat(fpath, &st) == 0) { is_dir = S_ISDIR(st.st_mode); fsize = is_dir ? 0 : (int64_t)st.st_size; mtime = st.st_mtime; }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", e->d_name);
        cJSON_AddBoolToObject(item, "is_dir", is_dir);
        cJSON_AddNumberToObject(item, "size", (double)fsize);
        cJSON_AddNumberToObject(item, "mtime", (double)mtime);
        cJSON_AddItemToArray(files_arr, item);
    }
    closedir(d);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (json) { httpd_resp_send(req, json, strlen(json)); cJSON_free(json); }
    else httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/files/download?path=xxx */
static esp_err_t _api_files_download(httpd_req_t *req) {
    if (!SDCardDriver::instance().available()) { httpd_resp_sendstr(req, "SD card not available"); return ESP_OK; }
    audio_lock();
    if (s_is_recording || s_playing) { audio_unlock(); httpd_resp_sendstr(req, "Audio is active"); return ESP_OK; }
    audio_unlock();

    char q[256] = {}, raw[256] = {};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || !strlen(q)) { httpd_resp_sendstr(req, "Missing path"); return ESP_OK; }
    httpd_query_key_value(q, "path", raw, sizeof(raw));
    if (!strlen(raw)) { httpd_resp_sendstr(req, "Missing path"); return ESP_OK; }
    _url_decode(raw);

    char fpath[320];
    if (!_path_sanitize(raw, fpath, sizeof(fpath))) { httpd_resp_sendstr(req, "Invalid path"); return ESP_OK; }

    struct stat st;
    if (stat(fpath, &st) != 0 || S_ISDIR(st.st_mode)) { httpd_resp_sendstr(req, "Not a file"); return ESP_OK; }

    FILE *f = fopen(fpath, "rb");
    if (!f) { httpd_resp_sendstr(req, "Cannot open file"); return ESP_OK; }

    audio_lock();
    if (s_is_recording || s_playing) { audio_unlock(); fclose(f); httpd_resp_sendstr(req, "Audio is active"); return ESP_OK; }
    s_fm_busy = true;
    audio_unlock();

    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    const char *fname = strrchr(fpath, '/'); fname = fname ? fname + 1 : fpath;
    /* Sanitize filename for Content-Disposition header — strip quotes and CR/LF
     * to prevent HTTP header injection via malicious filenames on the SD card. */
    char safe_fname[256];
    {
        const char *src = fname;
        char *dst = safe_fname;
        char *end = safe_fname + sizeof(safe_fname) - 1;
        while (*src && dst < end) {
            if (*src == '"' || *src == '\r' || *src == '\n') { src++; continue; }
            *dst++ = *src++;
        }
        *dst = '\0';
    }
    char disp[384]; snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", safe_fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "application/octet-stream");
    if (fsize > 0) { char clen[32]; snprintf(clen, sizeof(clen), "%ld", fsize); httpd_resp_set_hdr(req, "Content-Length", clen); }

    uint8_t *chunk = (uint8_t *)malloc(1024);
    if (!chunk) { fclose(f); s_fm_busy = false; httpd_resp_sendstr(req, "Out of memory"); return ESP_OK; }
    size_t n;
    while ((n = fread(chunk, 1, 1024, f)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)chunk, (int)n) != ESP_OK) break;
    }
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    s_fm_busy = false;
    ESP_LOGI(TAG, "File downloaded: %s (%ld bytes)", fpath, fsize);
    return ESP_OK;
}

/* POST /api/files/delete — body: {"path": "xxx"} */
static esp_err_t _api_files_delete(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (!SDCardDriver::instance().available()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}"); return ESP_OK; }

    /* Validate Content-Length to prevent buffer overflow */
    if (req->content_len > 511) {
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Body too large\"}");
        return ESP_OK;
    }

    audio_lock();
    if (s_is_recording || s_playing) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Audio is active\"}"); return ESP_OK; }
    s_fm_busy = true; audio_unlock();

    char buf[512]; int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Empty body\"}"); return ESP_OK; }
    buf[received] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid JSON\"}"); return ESP_OK; }
    cJSON *j_path = cJSON_GetObjectItem(root, "path");
    if (!j_path || !cJSON_IsString(j_path) || !j_path->valuestring) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Missing path\"}"); cJSON_Delete(root); return ESP_OK; }
    char fpath[320]; bool safe = _path_sanitize(j_path->valuestring, fpath, sizeof(fpath));
    cJSON_Delete(root);
    if (!safe) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid path\"}"); return ESP_OK; }

    struct stat st;
    if (stat(fpath, &st) != 0) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"File not found\"}"); return ESP_OK; }
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(fpath) != 0) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Cannot delete directory\"}"); return ESP_OK; }
    } else {
        if (unlink(fpath) != 0) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Delete failed\"}"); return ESP_OK; }
    }
    s_fm_busy = false;
    ESP_LOGI(TAG, "File deleted: %s", fpath);
    httpd_resp_sendstr(req, "{\"ok\":1}"); return ESP_OK;
}

/* POST /api/files/delete_batch — body: {"paths": ["xxx", "yyy"]} */
static esp_err_t _api_files_delete_batch(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (!SDCardDriver::instance().available()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}"); return ESP_OK; }
    audio_lock();
    if (s_is_recording || s_playing) { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Audio is active\"}"); return ESP_OK; }
    s_fm_busy = true; audio_unlock();

    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) { s_fm_busy = false; char err[80]; snprintf(err, sizeof(err), "{\"ok\":0,\"error\":\"Request too large (max %u bytes)\"}", (unsigned)kMaxBody); httpd_resp_sendstr(req, err); return ESP_OK; }
    char *buf = (char *)malloc(kMaxBody);
    if (!buf) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Out of memory\"}"); return ESP_OK; }
    int received = httpd_req_recv(req, buf, kMaxBody - 1);
    if (received <= 0) { free(buf); s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Empty body\"}"); return ESP_OK; }
    buf[received] = '\0';
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid JSON\"}"); return ESP_OK; }
    cJSON *j_paths = cJSON_GetObjectItem(root, "paths");
    if (!j_paths || !cJSON_IsArray(j_paths)) { s_fm_busy = false; httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Missing paths array\"}"); cJSON_Delete(root); return ESP_OK; }

    int deleted = 0, failed = 0;
    char fpath[320];
    cJSON *item;
    cJSON_ArrayForEach(item, j_paths) {
        if (!cJSON_IsString(item) || !item->valuestring) { failed++; continue; }
        if (!_path_sanitize(item->valuestring, fpath, sizeof(fpath))) { failed++; continue; }
        struct stat st;
        if (stat(fpath, &st) != 0) { failed++; continue; }
        int ret = S_ISDIR(st.st_mode) ? rmdir(fpath) : unlink(fpath);
        if (ret == 0) deleted++; else failed++;
    }
    cJSON_Delete(root);
    s_fm_busy = false;
    char resp[128]; snprintf(resp, sizeof(resp), "{\"ok\":1,\"deleted\":%d,\"failed\":%d}", deleted, failed);
    httpd_resp_sendstr(req, resp); return ESP_OK;
}

/* ── ULog handlers ── */

/* GET /api/ulog/status */
static esp_err_t _api_ulog_status(httpd_req_t *req) {
    ulog_writer_t *ulog = ulog_writer_get();
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    cJSON_AddBoolToObject(root, "running", ulog_writer_get_state(ulog) == ULOG_STATE_RUNNING);
    cJSON_AddBoolToObject(root, "camera_streaming", CameraApp::instance().streaming());
    cJSON_AddNumberToObject(root, "camera_frame_count", (double)CameraApp::instance().frameCount());
    const char *fp = ulog_writer_get_filepath(ulog);
    cJSON_AddStringToObject(root, "filepath", fp ? fp : "");
    cJSON_AddNumberToObject(root, "bytes_written", (double)ulog_writer_get_bytes_written(ulog));
    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (json) { httpd_resp_sendstr(req, json); cJSON_free((void*)json); }
    else httpd_resp_sendstr(req, "{}");
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/ulog/start */
static esp_err_t _api_ulog_start(httpd_req_t *req) {
    if (!SDCardDriver::instance().available()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}"); return ESP_OK;
    }
    ulog_writer_t *ulog = ulog_writer_get();
    esp_err_t err = ulog_writer_start(ulog);
    if (err == ESP_OK) {
        /* Start continuous audio recording to ULog */
        esp_err_t audio_err = AudioUlogRecorder::instance().start();
        if (audio_err != ESP_OK) {
            ESP_LOGW(TAG, "Audio ULog recorder start failed: %s", esp_err_to_name(audio_err));
        }
    }
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        /* Return status so client can update UI immediately */
        const char *fp = ulog_writer_get_filepath(ulog);
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddBoolToObject(root, "running", true);
            cJSON_AddStringToObject(root, "filepath", fp ? fp : "");
            cJSON_AddNumberToObject(root, "bytes_written", (double)ulog_writer_get_bytes_written(ulog));
            char *j = cJSON_PrintUnformatted(root);
            if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
            else httpd_resp_sendstr(req, "{\"ok\":1,\"running\":true}");
            cJSON_Delete(root);
        } else httpd_resp_sendstr(req, "{\"ok\":1,\"running\":true}");
    }
    else httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Start failed\"}");
    return ESP_OK;
}

/* POST /api/ulog/stop */
static esp_err_t _api_ulog_stop(httpd_req_t *req) {
    ulog_writer_t *ulog = ulog_writer_get();
    /* Capture final state before stopping */
    const char *fp = ulog_writer_get_filepath(ulog);
    size_t final_bytes = ulog_writer_get_bytes_written(ulog);
    char filepath_snap[128] = {};
    if (fp) strlcpy(filepath_snap, fp, sizeof(filepath_snap));
    /* Stop audio recording before stopping ULog */
    AudioUlogRecorder::instance().stop();
    ulog_writer_stop(ulog);
    httpd_resp_set_type(req, "application/json");
    /* Return status so client can update UI immediately */
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "running", false);
        cJSON_AddStringToObject(root, "filepath", filepath_snap);
        cJSON_AddNumberToObject(root, "bytes_written", (double)final_bytes);
        char *j = cJSON_PrintUnformatted(root);
        if (j) { httpd_resp_sendstr(req, j); cJSON_free(j); }
        else httpd_resp_sendstr(req, "{\"ok\":1,\"running\":false}");
        cJSON_Delete(root);
    } else httpd_resp_sendstr(req, "{\"ok\":1,\"running\":false}");
    return ESP_OK;
}

/* ── Web UI handler ──────────────────────────────────────────────── */

/* GET / — serve index HTML */
static esp_err_t _index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── HTTP server registration ───────────────────────────────────── */

static void _register_handlers(httpd_handle_t server) {
    httpd_uri_t uri;
    memset(&uri, 0, sizeof(uri));

    /* Web UI */
    uri.uri = "/"; uri.method = HTTP_GET; uri.handler = _index_handler;
    httpd_register_uri_handler(server, &uri);

    /* WiFi APIs */
    uri.uri = "/api/wifi/scan"; uri.method = HTTP_GET; uri.handler = _api_wifi_scan;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/wifi/connect"; uri.method = HTTP_POST; uri.handler = _api_wifi_connect;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/wifi/status"; uri.method = HTTP_GET; uri.handler = _api_wifi_status;
    httpd_register_uri_handler(server, &uri);

    /* System APIs */
    uri.uri = "/api/system/info"; uri.method = HTTP_GET; uri.handler = _api_system_info;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/system/stats"; uri.method = HTTP_GET; uri.handler = _api_system_stats;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/system/timezone"; uri.method = HTTP_GET; uri.handler = _api_timezone_get;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/system/timezone"; uri.method = HTTP_POST; uri.handler = _api_timezone_set;
    httpd_register_uri_handler(server, &uri);

    /* Audio APIs */
    uri.uri = "/api/audio/volume"; uri.method = HTTP_POST; uri.handler = _api_audio_volume_set;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/audio/volume"; uri.method = HTTP_GET; uri.handler = _api_audio_volume_get;
    httpd_register_uri_handler(server, &uri);

    /* Audio recording APIs */
    uri.uri = "/api/audio/record_start"; uri.method = HTTP_GET; uri.handler = _api_rec_start;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/audio/record_stop"; uri.method = HTTP_GET; uri.handler = _api_rec_stop;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/audio/record_status"; uri.method = HTTP_GET; uri.handler = _api_rec_status;
    httpd_register_uri_handler(server, &uri);

    /* Audio playback APIs */
    uri.uri = "/api/audio/list"; uri.method = HTTP_GET; uri.handler = _api_audio_list;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/audio/play"; uri.method = HTTP_GET; uri.handler = _api_play;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/audio/play_status"; uri.method = HTTP_GET; uri.handler = _api_play_status;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/audio/stop"; uri.method = HTTP_GET; uri.handler = _api_stop;
    httpd_register_uri_handler(server, &uri);

    /* File Manager APIs */
    uri.uri = "/api/files/list"; uri.method = HTTP_GET; uri.handler = _api_files_list;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/files/download"; uri.method = HTTP_GET; uri.handler = _api_files_download;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/files/delete"; uri.method = HTTP_POST; uri.handler = _api_files_delete;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/files/delete_batch"; uri.method = HTTP_POST; uri.handler = _api_files_delete_batch;
    httpd_register_uri_handler(server, &uri);

    /* ULog APIs */
    uri.uri = "/api/ulog/status"; uri.method = HTTP_GET; uri.handler = _api_ulog_status;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/ulog/start"; uri.method = HTTP_POST; uri.handler = _api_ulog_start;
    httpd_register_uri_handler(server, &uri);
    uri.uri = "/api/ulog/stop"; uri.method = HTTP_POST; uri.handler = _api_ulog_stop;
    httpd_register_uri_handler(server, &uri);

    /* SD Card APIs */
    uri.uri = "/api/sdcard/info"; uri.method = HTTP_GET; uri.handler = _api_sdcard_info;
    httpd_register_uri_handler(server, &uri);

    /* Bluetooth Audio APIs */
    uri.uri = "/api/bt/status"; uri.method = HTTP_GET; uri.handler = _api_bt_status;
    httpd_register_uri_handler(server, &uri);

    ESP_LOGI(TAG, "HTTP handlers registered (%d endpoints)", 26);
}

/* ── mDNS ────────────────────────────────────────────────────────── */

static void _add_mdns_service(void) {
    /* Ensure mDNS is initialized (reference-counted, shared across modules) */
    if (!shared_mdns_ensure()) {
        ESP_LOGW(TAG, "mDNS init failed, skipping service registration");
        return;
    }

    const char *hostname = shared_mdns_hostname();

    mdns_txt_item_t txt[] = {
        {"path", "/"},
        {"board", "ESP32-S31-Korvo-1"},
    };

    esp_err_t err = mdns_service_add("ESP32-S31 Web Config", "_http", "_tcp",
            8080, txt, sizeof(txt) / sizeof(txt[0]));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mDNS advertised: %s.local:8080", hostname);
    } else {
        ESP_LOGW(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

/* Background task: waits for WiFi, starts SNTP on STA connect,
 * auto-starts ULog after SNTP sync, and manages HTTP server
 * lifecycle across WiFi up/down transitions. */
static void _web_config_task(void *arg)
{
    ESP_LOGI(TAG, "Web config background task started");

    /* Load timezone early (not dependent on WiFi) */
    _load_timezone_from_nvs();

    bool ulog_autostart_done = false;
    bool prev_sta_up = false;
    int sntp_log_counter = 0;  /* Throttle SNTP waiting log */

    while (s_server_running.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        wifi_service_status_t st;
        WifiService::instance().get_status(&st);

        /* ── SNTP: start on first STA connect with IP ── */
        if (st.sta_connected && !s_sntp_initialized.load(std::memory_order_acquire)) {
            ESP_LOGI(TAG, "STA connected (IP=%s), starting SNTP...", st.sta_ip);
            sntp_start_and_ulog_autostart();
        }

        /* Diagnostic: log SNTP waiting state periodically */
        if (st.sta_connected && !s_sntp_synced.load(std::memory_order_acquire)) {
            if (++sntp_log_counter >= 10) {  /* Every 10s */
                sntp_log_counter = 0;
                char tz_snap[32] = {};
                if (s_timezone_mutex && xSemaphoreTake(s_timezone_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
                    xSemaphoreGive(s_timezone_mutex);
                } else {
                    strlcpy(tz_snap, s_timezone, sizeof(tz_snap));
                }
                ESP_LOGI(TAG, "SNTP: waiting for time sync (server=%s, TZ=%s)...",
                         CONFIG_SNTP_SERVER_0, tz_snap);
            }
        } else {
            sntp_log_counter = 0;
        }

        /* ── ULog: auto-start once SNTP is synced and ULog is idle ── */
        if (s_sntp_synced.load(std::memory_order_acquire) && !ulog_autostart_done) {
            ulog_writer_t *ulog = ulog_writer_get();
            ulog_state_t state = ulog_writer_get_state(ulog);
            if (state == ULOG_STATE_IDLE) {
                ulog_autostart_done = true;
                esp_err_t err = ulog_writer_start(ulog);
                if (err == ESP_OK) {
                    esp_err_t audio_err = AudioUlogRecorder::instance().start();
                    if (audio_err != ESP_OK) {
                        ESP_LOGW(TAG, "Audio ULog recorder start failed: %s", esp_err_to_name(audio_err));
                    }
                    ESP_LOGI(TAG, "ULog auto-started after SNTP sync");
                } else {
                    ESP_LOGW(TAG, "ULog auto-start failed: %s", esp_err_to_name(err));
                }
            } else if (state != ULOG_STATE_UNINIT) {
                /* Already running (or errored) — stop polling */
                ulog_autostart_done = true;
            }
        }

        /* ── WiFi up/down tracking ── */
        bool sta_up = st.sta_connected;
        if (prev_sta_up && !sta_up) {
            ESP_LOGW(TAG, "WiFi STA disconnected");
        } else if (!prev_sta_up && sta_up) {
            ESP_LOGI(TAG, "WiFi STA connected: ssid=%s ip=%s", st.sta_ssid, st.sta_ip);
        }
        prev_sta_up = sta_up;
    }

    ESP_LOGI(TAG, "Web config background task exiting");
    vTaskDelete(NULL);
}

void web_config_server_start(void) {
    if (s_server_running.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Server already running");
        return;
    }

    /* Create audio mutex before starting the server (handlers use it immediately) */
    if (!s_audio_mutex) {
        s_audio_mutex = xSemaphoreCreateMutex();
    }

    /* Pre-allocate audio task TCB */
    if (!s_audio_tcb) {
        s_audio_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;  /* Different from captive portal's default 32768 */
    config.max_uri_handlers = 32;
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    config.core_id = 0;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return;
    }

    _register_handlers(s_httpd);
    _add_mdns_service();

    /* Register AAC encoder once — called from app_main() before any tasks exist,
     * so no concurrency protection is needed (single-threaded at this point). */
    {
        static bool s_enc_registered = false;
        if (!s_enc_registered) {
            esp_audio_enc_register_default();
            s_enc_registered = true;
        }
    }

    s_server_running.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Web config server started on port 8080");

    /* Start background task for SNTP + ULog auto-start */
    BaseType_t ret = xTaskCreate(_web_config_task, "web_cfg_bg",
            4096, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create web config background task");
    }
}

void web_config_server_stop(void) {
    if (!s_server_running.load(std::memory_order_acquire)) return;

    /* Signal background task to stop first */
    s_server_running.store(false, std::memory_order_release);

    /* Give background task time to notice the flag */
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }

    /* Free pre-allocated audio task TCB (no longer needed after server stop) */
    if (s_audio_tcb) {
        heap_caps_free(s_audio_tcb);
        s_audio_tcb = NULL;
    }

    ESP_LOGI(TAG, "Web config server stopped");
}