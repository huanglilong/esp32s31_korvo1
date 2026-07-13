/**
 * @file wifi_service.cpp
 * @brief C++ facade over wifi_manager — unified WiFi service for ESP32-S31.
 *
 * Wraps wifi_manager C API, manages uORB wifi_state publication,
 * captive portal, SNTP startup, and NVS credential persistence.
 */

#include "wifi_service.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "app_config.h"
#include "wifi_manager.h"
#include "captive_dns.h"
#include "generated/wifi_state.h"
#include "topics.h"
#include "esp_http_server.h"

static const char *TAG = "wifi_service";

/* ── Captive Portal HTTP Server (port 80) ──────────────────────────
 * Responds to all requests with a 302 redirect to the Web Config
 * portal (port 8080). This enables automatic captive portal detection
 * on phones (Android: connectivitycheck.gstatic.com, iOS: captive.apple.com).
 * The DNS hijacking (captive_dns) redirects all domain lookups here,
 * and this HTTP handler completes the captive portal detection flow. */

static httpd_handle_t s_captive_httpd = nullptr;

static esp_err_t _captive_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* iOS captive portal detection: http://captive.apple.com/hotspot-detect.html
     * must return "Success" (plain text, no HTML) for iOS to detect captive portal. */
    if (uri && strcmp(uri, "/hotspot-detect.html") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Success", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Android captive portal detection: various /generate_204 endpoints.
     * Android expects HTTP 204 No Content to confirm internet access.
     * Returning a 302 redirect here signals "captive portal detected" and
     * triggers the system captive portal dialog. */
    if (uri && strstr(uri, "generate_204")) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1:8080/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* All other requests: redirect to Web Config portal.
     * This catches both user browser requests and other captive portal
     * detection methods (e.g., connectivitycheck.gstatic.com on Android). */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1:8080/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t _captive_httpd_start(void)
{
    if (s_captive_httpd) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 4;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.core_id = 0;

    esp_err_t err = httpd_start(&s_captive_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Captive HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register handlers for known captive portal detection URLs.
     * iOS: http://captive.apple.com/hotspot-detect.html → "Success"
     * Android: various /generate_204 endpoints → 302 redirect
     * All others: catch-all wildcard → 302 redirect to Web Config */

    httpd_uri_t hotspot_uri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = _captive_handler,
    };
    esp_err_t _r = httpd_register_uri_handler(s_captive_httpd, &hotspot_uri);
    if (_r != ESP_OK) ESP_LOGE(TAG, "Failed to register /hotspot-detect.html: %s", esp_err_to_name(_r));

    httpd_uri_t gen204_uri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = _captive_handler,
    };
    _r = httpd_register_uri_handler(s_captive_httpd, &gen204_uri);
    if (_r != ESP_OK) ESP_LOGE(TAG, "Failed to register /generate_204: %s", esp_err_to_name(_r));

    /* Catch-all: register "/" with wildcard matching (user_ctx != NULL).
     * This matches any path not handled by the specific handlers above.
     * Also register a POST handler to silence 404 warnings from non-HTTP
     * protocols like WeChat mmtls being misrouted here via DNS hijacking. */
    httpd_uri_t catchall_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = _captive_handler,
        .user_ctx = (void *)1,  /* Enable wildcard matching */
    };
    esp_err_t _r2 = httpd_register_uri_handler(s_captive_httpd, &catchall_uri);
    if (_r2 != ESP_OK) ESP_LOGE(TAG, "Failed to register catch-all GET: %s", esp_err_to_name(_r2));

    /* POST catch-all: silently discard non-GET requests (mmtls, etc.)
     * that arrive due to DNS hijacking. These are expected noise
     * during provisioning mode and are harmless. */
    httpd_uri_t catchall_post = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = _captive_handler,
        .user_ctx = (void *)1,
    };
    _r2 = httpd_register_uri_handler(s_captive_httpd, &catchall_post);
    if (_r2 != ESP_OK) ESP_LOGE(TAG, "Failed to register catch-all POST: %s", esp_err_to_name(_r2));

    ESP_LOGI(TAG, "Captive portal HTTP server started on port 80");
    return ESP_OK;
}

/* ── Singleton ──────────────────────────────────────────────────── */

WifiService& WifiService::instance() {
    static WifiService s;
    return s;
}

/* ── NVS helpers ─────────────────────────────────────────────────── */

bool WifiService::_read_nvs_creds(char *ssid_out, size_t ssid_size,
                                   char *pass_out, size_t pass_size) {
    nvs_handle_t h;
    ssid_out[0] = '\0';
    pass_out[0] = '\0';

    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = ssid_size;
    nvs_get_str(h, NVS_KEY_WIFI_SSID, ssid_out, &len);
    len = pass_size;
    nvs_get_str(h, NVS_KEY_WIFI_PASS, pass_out, &len);
    nvs_close(h);

    return (strlen(ssid_out) > 0);
}

void WifiService::_save_nvs_creds(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_str(h, NVS_KEY_WIFI_SSID, ssid);
    if (password && strlen(password) > 0) {
        nvs_set_str(h, NVS_KEY_WIFI_PASS, password);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS: ssid=%s", ssid);
}

/* ── uORB wifi_state publisher ──────────────────────────────────── */

void WifiService::_publish_wifi_state(bool connected, const char *ssid, int8_t rssi) {
    /* Lazy-init uORB advertiser (thread-safe CAS loop) */
    orb_advert_t pub = _wifi_state_pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t expected = ORB_ADVERT_INVALID;
        orb_advert_t new_pub = orb_advertise(ORB_ID(wifi_state));
        if (!_wifi_state_pub.compare_exchange_strong(expected, new_pub,
                std::memory_order_release, std::memory_order_acquire)) {
            /* Another thread already set it — ignore our unused handle */
        }
        pub = _wifi_state_pub.load(std::memory_order_acquire);
    }

    if (pub == ORB_ADVERT_INVALID) return;

    wifi_state_s ws = {};
    ws.timestamp = esp_timer_get_time();
    ws.connected = connected;
    ws.scanning = false;
    ws.rssi = rssi;
    if (ssid) {
        strlcpy(ws.ssid, ssid, sizeof(ws.ssid));
    }
    orb_publish(ORB_ID(wifi_state), pub, &ws);
}

/* ── State callback (called by wifi_manager from event handler) ── */

void WifiService::_state_callback(bool connected, void *user_ctx) {
    WifiService *self = static_cast<WifiService *>(user_ctx);
    if (!self) return;

    /* Update uORB wifi_state */
    wifi_manager_status_t st;
    wifi_manager_get_status(&st);

    /* Start captive portal DNS when AP becomes active.
     * AP startup is asynchronous — the netif IP is only valid
     * after WIFI_EVENT_AP_START fires. */
    if (st.ap_active && !self->_captive_dns_started.load(std::memory_order_acquire)) {
        esp_netif_t *ap_netif = wifi_manager_get_ap_netif();
        if (ap_netif) {
            captive_dns_config_t dns_cfg = {
                .ap_netif = ap_netif,
                .redirect_ip = 0,
                .configure_dhcp_dns = true,
            };
            if (captive_dns_start(&dns_cfg) == ESP_OK) {
                self->_captive_dns_started.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "Captive portal DNS started on AP interface");
                /* Start HTTP server on port 80 for captive portal detection */
                _captive_httpd_start();
            } else {
                ESP_LOGW(TAG, "Captive portal DNS start failed");
            }
        }
    }

    /* Stop captive portal once STA is connected — no longer needed.
     * Prevents background app traffic (WeChat mmtls, push notifications,
     * etc.) from flooding the captive HTTP server with 404 errors.
     * Also stop the HTTP server to free port 80 for CameraStream. */
    if (connected && self->_captive_dns_started.load(std::memory_order_acquire)) {
        captive_dns_stop();
        self->_captive_dns_started.store(false, std::memory_order_release);
        ESP_LOGI(TAG, "Captive portal DNS stopped (STA connected)");
        if (s_captive_httpd) {
            httpd_stop(s_captive_httpd);
            s_captive_httpd = nullptr;
            ESP_LOGI(TAG, "Captive portal HTTP server stopped (STA connected)");
        }
    }

    int8_t rssi = 0;
    if (connected) {
        int rssi_raw = 0;
        esp_wifi_sta_get_rssi(&rssi_raw);
        rssi = (int8_t)rssi_raw;
        self->_publish_wifi_state(true, st.sta_ssid, rssi);

        /* Update mDNS delegated hostname IP now that WiFi has an address */
        shared_mdns_update_delegate_ip();
    } else {
        self->_publish_wifi_state(false, "", 0);
    }

    /* Start SNTP on first connect */
    if (connected && !self->_sntp_started.load(std::memory_order_acquire)) {
        self->set_sntp_started();
        /* Web config server task will detect this and start SNTP */
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t WifiService::init() {
    if (_initialized.load(std::memory_order_acquire)) return ESP_OK;

    /* Create scan mutex */
    _scan_mutex = xSemaphoreCreateMutexStatic(&_scan_mutex_buf);
    if (!_scan_mutex) return ESP_ERR_NO_MEM;

    /* Initialize wifi_manager (netif + event loop + wifi driver) */
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register state callback for uORB publishing */
    wifi_manager_register_state_callback(_state_callback, this);

    _initialized.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "WiFi service initialized");
    return ESP_OK;
}

esp_err_t WifiService::start() {
    if (!_initialized.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "WifiService not initialized — call init() first");
        return ESP_ERR_INVALID_STATE;
    }

    char ssid[33] = {};
    char pass[65] = {};
    bool has_creds = _read_nvs_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    wifi_manager_config_t cfg = {};
    if (has_creds) {
        cfg.sta_ssid = ssid;
        cfg.sta_password = (strlen(pass) > 0) ? pass : nullptr;
        strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
        ESP_LOGI(TAG, "Starting WiFi with STA: ssid=%s", ssid);
    } else {
        /* No stored credentials — AP-only provisioning mode.
         * Use a descriptive AP SSID prefix for this device. */
        cfg.ap_ssid_prefix = "esp-s31";
        ESP_LOGI(TAG, "Starting WiFi in AP-only provisioning mode");
    }

    /* We don't use close_on_sta — keep AP always available for
     * WIFI6 (headless) boards to always have a provisioning path. */
    cfg.ap_behavior = "keep";

    esp_err_t err = wifi_manager_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Captive portal DNS will be started from _state_callback
     * when the AP becomes active (AP_START event). AP startup is
     * asynchronous — the netif IP is not yet assigned here. */

    _started.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t WifiService::scan(wifi_scan_result_t *results, uint16_t *out_count) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!results || !out_count) return ESP_ERR_INVALID_ARG;

    /* Serialize scans (single mutex, short critical section) */
    if (xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_manager_scan_record_t raw[WIFI_SERVICE_SCAN_MAX];
    uint16_t count = 0;
    esp_err_t err = wifi_manager_scan_aps(raw, WIFI_SERVICE_SCAN_MAX, &count);

    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            strlcpy(results[i].ssid, raw[i].ssid, sizeof(results[i].ssid));
            results[i].rssi = raw[i].rssi;
            results[i].authmode = raw[i].authmode;
        }
        *out_count = count;
    }

    xSemaphoreGive(_scan_mutex);
    return err;
}

esp_err_t WifiService::connect(const char *ssid, const char *password) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    /* Save credentials to NVS BEFORE attempting to connect,
     * so they persist even if the network is temporarily unavailable. */
    _save_nvs_creds(ssid, password);
    strlcpy(_current_ssid, ssid, sizeof(_current_ssid));

    /* Use wifi_manager_apply_sta_config to hot-swap credentials */
    wifi_manager_config_t cfg = {};
    cfg.sta_ssid = ssid;
    cfg.sta_password = (password && strlen(password) > 0) ? password : nullptr;

    esp_err_t err = wifi_manager_apply_sta_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_sta_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for connection (blocking, caller runs in a task) */
    err = wifi_manager_wait_connected(15000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Connected to %s", ssid);
    } else {
        ESP_LOGW(TAG, "Connect to %s timed out (credentials saved, will retry on boot)", ssid);
    }

    return err;
}

esp_err_t WifiService::disconnect() {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;

    /* Apply empty STA config — wifi_manager will switch to AP-only provision mode */
    wifi_manager_config_t cfg = {};
    cfg.ap_behavior = "keep";
    return wifi_manager_apply_sta_config(&cfg);
}

esp_err_t WifiService::apply_sta_config(const char *ssid, const char *password) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;

    wifi_manager_config_t cfg = {};
    cfg.sta_ssid = (ssid && strlen(ssid) > 0) ? ssid : nullptr;
    cfg.sta_password = (password && strlen(password) > 0) ? password : nullptr;
    /* Keep AP running for provisioning */
    cfg.ap_behavior = "keep";

    esp_err_t err = wifi_manager_apply_sta_config(&cfg);
    if (err == ESP_OK) {
        if (ssid && strlen(ssid) > 0) {
            _save_nvs_creds(ssid, password);
            strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
        }
    }
    return err;
}

void WifiService::get_status(wifi_service_status_t *status) {
    if (!status) return;

    wifi_manager_status_t st;
    wifi_manager_get_status(&st);

    status->sta_connected = st.sta_connected;
    status->sta_configured = st.sta_configured;
    strlcpy(status->sta_ip, st.sta_ip, sizeof(status->sta_ip));
    strlcpy(status->sta_ssid, st.sta_ssid ? st.sta_ssid : _current_ssid, sizeof(status->sta_ssid));
    status->sta_rssi = 0;
    if (st.sta_connected) {
        int rssi_raw = 0;
        esp_wifi_sta_get_rssi(&rssi_raw);
        status->sta_rssi = (int8_t)rssi_raw;
    }
}

esp_err_t WifiService::wait_connected(uint32_t timeout_ms) {
    return wifi_manager_wait_connected(timeout_ms);
}

void *WifiService::get_ap_netif() {
    return wifi_manager_get_ap_netif();
}
