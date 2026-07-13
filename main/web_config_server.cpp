/**
 * @file web_config_server.cpp
 * @brief Web configuration server for ESP32-S31-Korvo-1.
 *
 * HTTP server on port 8080 with REST APIs:
 *   - GET  /api/wifi/scan      — scan nearby WiFi networks
 *   - POST /api/wifi/connect   — connect to WiFi network
 *   - GET  /api/wifi/status    — get current WiFi status
 *   - GET  /api/system/info    — system information
 *   - GET  /api/system/stats   — system performance stats
 *   - POST /api/audio/volume   — set speaker volume
 *   - GET  /api/audio/volume   — get current volume
 *   - GET  /api/sdcard/info    — SD card status
 *
 * Web UI:
 *   - GET  /                   — simple WiFi config webpage
 *
 * mDNS: advertises _http._tcp on esp-web-XXXXXX.local:8080
 */

#include "web_config_server.hpp"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_sntp.h"
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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <ctime>

#include "app_config.h"
#include "wifi_service.hpp"
#include "drivers/audio/audio_driver.hpp"
#include "drivers/sdcard/sdcard_driver.hpp"
#include "drivers/system_monitor/system_monitor.hpp"
#include "ulog_writer.h"

static const char *TAG = "web_config";

static httpd_handle_t s_httpd = nullptr;

/* ── SNTP state ──────────────────────────────────────────────────── */
static std::atomic<bool> s_sntp_initialized{false};
static std::atomic<bool> s_sntp_synced{false};

/* Saved timezone string (e.g. "CST-8") — persisted in NVS */
static char s_timezone[32] = {};
static std::atomic<bool> s_timezone_loaded{false};
static SemaphoreHandle_t s_timezone_mutex = nullptr;  /* Protects s_timezone r/w */

/* ── Timezone helpers ──────────────────────────────────────────────── */

static void _load_timezone_from_nvs(void)
{
    /* Create mutex on first call */
    if (!s_timezone_mutex) {
        s_timezone_mutex = xSemaphoreCreateMutex();
    }

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
<title>ESP32-S31 Korvo-1 配置</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #e0e0e0; padding: 20px; max-width: 600px; margin: 0 auto; }
  h1 { text-align: center; color: #e94560; margin-bottom: 20px; }
  .card { background: #16213e; border-radius: 12px; padding: 20px; margin-bottom: 16px; }
  .card h2 { font-size: 18px; color: #0f3460; background: #e94560; display: inline-block; padding: 4px 12px; border-radius: 6px; margin-bottom: 12px; }
  label { display: block; font-size: 14px; color: #aaa; margin-bottom: 4px; }
  input, select { width: 100%; padding: 10px; border: 1px solid #333; border-radius: 8px; background: #0f3460; color: #fff; font-size: 14px; margin-bottom: 12px; }
  button { background: #e94560; color: #fff; border: none; padding: 10px 20px; border-radius: 8px; font-size: 14px; cursor: pointer; width: 100%; }
  button:hover { background: #d63851; }
  .status { font-size: 13px; color: #0f0; margin-top: 8px; }
  .error { color: #f44; }
  table { width: 100%; font-size: 13px; border-collapse: collapse; margin-top: 8px; }
  td { padding: 4px 8px; border-bottom: 1px solid #333; }
  td:first-child { color: #888; width: 120px; }
</style>
</head>
<body>
<h1>ESP32-S31 Korvo-1</h1>

<div class="card">
  <h2>WiFi 扫描</h2>
  <button onclick="scanWiFi()">扫描网络</button>
  <div id="scan-status" class="status"></div>
  <table id="scan-results"></table>
</div>

<div class="card">
  <h2>WiFi 连接</h2>
  <label>SSID</label><input type="text" id="ssid" placeholder="WiFi 名称">
  <label>密码</label><input type="password" id="password" placeholder="WiFi 密码">
  <button onclick="connectWiFi()">连接</button>
  <div id="wifi-status" class="status"></div>
</div>

<div class="card">
  <h2>音量控制</h2>
  <label>音量 (0-100)</label>
  <input type="range" id="volume" min="0" max="100" value="60" oninput="document.getElementById('volume-val').textContent=this.value">
  <span id="volume-val" style="font-size:24px;vertical-align:middle;">60</span>
  <button onclick="setVolume()">设置音量</button>
  <div id="vol-status" class="status"></div>
</div>

<div class="card">
  <h2>系统信息</h2>
  <button onclick="refreshSysInfo()">刷新</button>
  <table id="sys-info"></table>
</div>

<script>
async function apiGet(path) {
  try { const r = await fetch(path); return await r.json(); }
  catch(e) { return {error: e.message}; }
}
async function apiPost(path, body) {
  try {
    const r = await fetch(path, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
    return await r.json();
  } catch(e) { return {error: e.message}; }
}

async function scanWiFi() {
  document.getElementById('scan-status').textContent = '正在扫描...';
  const data = await apiGet('/api/wifi/scan');
  if (data.error) { document.getElementById('scan-status').innerHTML = '<span class="error">' + data.error + '</span>'; return; }
  document.getElementById('scan-status').textContent = '找到 ' + (data.networks ? data.networks.length : 0) + ' 个网络';
  let html = '<tr><td>SSID</td><td>信号</td><td>加密</td><td></td></tr>';
  if (data.networks) {
    data.networks.forEach(n => {
      const auth = n.authmode > 0 ? 'WPA' : 'Open';
      html += '<tr><td>' + n.ssid + '</td><td>' + n.rssi + ' dBm</td><td>' + auth + '</td>';
      html += '<td><button style="padding:2px 8px;font-size:12px;" onclick="selectSSID(\'' + n.ssid + '\')">选择</button></td></tr>';
    });
  }
  document.getElementById('scan-results').innerHTML = html;
}

function selectSSID(ssid) {
  document.getElementById('ssid').value = ssid;
}

async function connectWiFi() {
  const ssid = document.getElementById('ssid').value;
  const pass = document.getElementById('password').value;
  if (!ssid) { document.getElementById('wifi-status').innerHTML = '<span class="error">请输入 SSID</span>'; return; }
  document.getElementById('wifi-status').textContent = '正在连接...';
  const data = await apiPost('/api/wifi/connect', {ssid, password:pass});
  if (data.error) { document.getElementById('wifi-status').innerHTML = '<span class="error">' + data.error + '</span>'; }
  else { document.getElementById('wifi-status').textContent = '已连接到 ' + data.ssid + ' IP: ' + data.ip; }
}

async function setVolume() {
  const vol = parseInt(document.getElementById('volume').value);
  const data = await apiPost('/api/audio/volume', {volume:vol});
  if (data.error) { document.getElementById('vol-status').innerHTML = '<span class="error">' + data.error + '</span>'; }
  else { document.getElementById('vol-status').textContent = '音量已设置为 ' + data.volume; }
}

async function refreshSysInfo() {
  const data = await apiGet('/api/system/info');
  let html = '';
  if (data.error) { html = '<tr><td colspan="2" class="error">' + data.error + '</td></tr>'; }
  else {
    html += '<tr><td>芯片</td><td>' + (data.chip || 'N/A') + '</td></tr>';
    html += '<tr><td>CPU 频率</td><td>' + (data.cpu_freq || 'N/A') + ' MHz</td></tr>';
    html += '<tr><td>Flash 大小</td><td>' + (data.flash_size || 'N/A') + ' MB</td></tr>';
    html += '<tr><td>PSRAM</td><td>' + (data.psram_size || 'N/A') + ' MB</td></tr>';
    html += '<tr><td>SDK</td><td>' + (data.sdk_version || 'N/A') + '</td></tr>';
    html += '<tr><td>WiFi</td><td>' + (data.wifi_connected ? '已连接 '+data.wifi_ssid : '未连接') + '</td></tr>';
    html += '<tr><td>SD 卡</td><td>' + (data.sdcard_mounted ? '已挂载' : '未检测到') + '</td></tr>';
    html += '<tr><td>空闲堆内存</td><td>' + (data.free_heap || 'N/A') + ' KB</td></tr>';
    html += '<tr><td>运行时间</td><td>' + (data.uptime || 'N/A') + ' 秒</td></tr>';
    html += '<tr><td>NTP 同步</td><td>' + (data.sntp_synced ? '✅ 已同步' : '⏳ 未同步') + '</td></tr>';
    html += '<tr><td>时区</td><td>' + (data.timezone || 'N/A') + '</td></tr>';
    if (data.current_time) { html += '<tr><td>当前时间</td><td>' + data.current_time + '</td></tr>'; }
  }
  document.getElementById('sys-info').innerHTML = html;
}

// Auto-refresh system info on load
refreshSysInfo();
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
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    const char *ssid = ssid_item ? ssid_item->valuestring : nullptr;
    const char *pass = pass_item ? pass_item->valuestring : nullptr;

    cJSON *resp = cJSON_CreateObject();
    if (!ssid) {
        cJSON_AddStringToObject(resp, "error", "Missing SSID");
    } else {
        esp_err_t err = WifiService::instance().connect(ssid, pass);
        if (err == ESP_OK) {
            wifi_service_status_t st;
            WifiService::instance().get_status(&st);
            cJSON_AddStringToObject(resp, "ssid", ssid);
            cJSON_AddStringToObject(resp, "ip", st.sta_ip);
            cJSON_AddBoolToObject(resp, "connected", true);
        } else if (err == ESP_ERR_TIMEOUT) {
            cJSON_AddStringToObject(resp, "error", "Connection timed out");
        } else {
            cJSON_AddStringToObject(resp, "error", "Connection failed");
        }
    }

    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(resp);
    if (root) cJSON_Delete(root);
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
    cJSON_AddNumberToObject(root, "psram_size", 16);  /* ESP32-S31-WROOM-3 has 16MB PSRAM */

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
    if (s_timezone_mutex) xSemaphoreTake(s_timezone_mutex, portMAX_DELAY);
    strlcpy(s_timezone, new_tz, sizeof(s_timezone));
    setenv("TZ", s_timezone, 1);
    tzset();
    if (s_timezone_mutex) xSemaphoreGive(s_timezone_mutex);

    /* Persist to NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_TIMEZONE, s_timezone);
        nvs_commit(h);
        nvs_close(h);
    }

    cJSON_Delete(json);
    ESP_LOGI(TAG, "Timezone set to: %s", s_timezone);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "timezone", s_timezone);
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

/* POST /api/audio/volume */
static esp_err_t _api_audio_volume_set(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *vol_item = cJSON_GetObjectItem(root, "volume");

    cJSON *resp = cJSON_CreateObject();
    if (!vol_item) {
        cJSON_AddStringToObject(resp, "error", "Missing volume parameter");
    } else {
        int vol = vol_item->valueint;
        if (vol < VOLUME_MIN) vol = VOLUME_MIN;
        if (vol > VOLUME_MAX) vol = VOLUME_MAX;

        AudioDriver::instance().set_volume(vol);

        /* Save to NVS */
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, NVS_KEY_VOLUME, vol);
            nvs_commit(h);
            nvs_close(h);
        }

        cJSON_AddNumberToObject(resp, "volume", vol);
    }

    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(resp);
    if (root) cJSON_Delete(root);
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

/* GET /api/sdcard/info */
static esp_err_t _api_sdcard_info(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mounted", SDCardDriver::instance().available());
    cJSON_AddStringToObject(root, "mountpoint", SDMMC_MOUNT_POINT);

    if (SDCardDriver::instance().available()) {
        struct stat st;
        if (stat(SDMMC_MOUNT_POINT, &st) == 0) {
            cJSON_AddNumberToObject(root, "total_kb", st.st_size / 1024);  /* Approximation */
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Web UI handler ──────────────────────────────────────────────── */

/* GET / — serve index HTML */
static esp_err_t _index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── HTTP server registration ───────────────────────────────────── */

static void _register_handlers(httpd_handle_t server) {
    httpd_uri_t uri;
    memset(&uri, 0, sizeof(uri));

    /* Web UI */
    uri.uri = "/";
    uri.method = HTTP_GET;
    uri.handler = _index_handler;
    httpd_register_uri_handler(server, &uri);

    /* WiFi APIs */
    uri.uri = "/api/wifi/scan";
    uri.method = HTTP_GET;
    uri.handler = _api_wifi_scan;
    httpd_register_uri_handler(server, &uri);

    uri.uri = "/api/wifi/connect";
    uri.method = HTTP_POST;
    uri.handler = _api_wifi_connect;
    httpd_register_uri_handler(server, &uri);

    uri.uri = "/api/wifi/status";
    uri.method = HTTP_GET;
    uri.handler = _api_wifi_status;
    httpd_register_uri_handler(server, &uri);

    /* System APIs */
    uri.uri = "/api/system/info";
    uri.method = HTTP_GET;
    uri.handler = _api_system_info;
    httpd_register_uri_handler(server, &uri);

    uri.uri = "/api/system/stats";
    uri.method = HTTP_GET;
    uri.handler = _api_system_stats;
    httpd_register_uri_handler(server, &uri);

    /* Audio APIs */
    uri.uri = "/api/audio/volume";
    uri.method = HTTP_POST;
    uri.handler = _api_audio_volume_set;
    httpd_register_uri_handler(server, &uri);

    uri.uri = "/api/audio/volume";
    uri.method = HTTP_GET;
    uri.handler = _api_audio_volume_get;
    httpd_register_uri_handler(server, &uri);

    /* SD Card APIs */
    uri.uri = "/api/sdcard/info";
    uri.method = HTTP_GET;
    uri.handler = _api_sdcard_info;
    httpd_register_uri_handler(server, &uri);

    /* Timezone APIs */
    uri.uri = "/api/system/timezone";
    uri.method = HTTP_GET;
    uri.handler = _api_timezone_get;
    httpd_register_uri_handler(server, &uri);

    uri.uri = "/api/system/timezone";
    uri.method = HTTP_POST;
    uri.handler = _api_timezone_set;
    httpd_register_uri_handler(server, &uri);

    ESP_LOGI(TAG, "HTTP handlers registered");
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
                ESP_LOGI(TAG, "SNTP: waiting for time sync (server=%s, TZ=%s)...",
                         CONFIG_SNTP_SERVER_0, s_timezone);
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;  /* Different from captive portal's default 32768 */
    config.max_uri_handlers = 14;
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

    ESP_LOGI(TAG, "Web config server stopped");
}