/**
 * @file wifi_service.cpp
 * @brief WiFi service implementation for ESP32-S31 native Wi-Fi.
 *
 * Manages STA + SoftAP + captive portal DNS + SNTP + mDNS.
 * Uses native ESP-IDF WiFi APIs (not esp_wifi_remote — ESP32-S31 has built-in WiFi).
 */

#include "wifi_service.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_mac.h"
#include <cstring>

#include "example_config.h"
#include "generated/wifi_state.h"
#include "topics.h"

static const char *TAG = "wifi_service";

/* ── SoftAP configuration ────────────────────────────────────────── */
#define WIFI_AP_SSID_PREFIX  "esp-s31"
#define WIFI_AP_PASSWORD     "12345678"
#define WIFI_AP_CHANNEL      1
#define WIFI_AP_MAX_CONN     4

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
    orb_advert_t pub = _wifi_state_pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t expected = ORB_ADVERT_INVALID;
        orb_advert_t new_pub = orb_advertise(ORB_ID(wifi_state));
        if (!_wifi_state_pub.compare_exchange_strong(expected, new_pub,
                std::memory_order_release, std::memory_order_acquire)) {
            /* Another thread already set it */
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

/* ── mDNS helpers ───────────────────────────────────────────────── */

static char s_mdns_hostname[32] = {0};

static void _start_mdns(void) {
    uint8_t mac[6];
    char hostname[32];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(hostname, sizeof(hostname), "esp-web-%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        strlcpy(hostname, "esp-web", sizeof(hostname));
    }
    strlcpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname));

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mDNS hostname: %s.local", hostname);
    }

    mdns_instance_name_set("ESP32-S31 Korvo-1");
}

/* ── SNTP ────────────────────────────────────────────────────────── */

static void _start_sntp(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

static void _sntp_sync_cb(struct timeval *tv) {
    WifiService::instance().set_sntp_synced();
    ESP_LOGI(TAG, "SNTP time synced");
}

/* ── WiFi/IP event handlers ────────────────────────────────────── */

void WifiService::_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
    WifiService *self = static_cast<WifiService *>(arg);

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "STA disconnected, reason=%d", ev->reason);
            self->_sta_connected.store(false, std::memory_order_release);
            self->_publish_wifi_state(false, "", 0);
            /* Auto-reconnect is handled by ESP-WiFi */
            break;
        }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP started: ssid=%s", WIFI_AP_SSID_PREFIX);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP station connected: " MACSTR, MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "AP station disconnected: " MACSTR, MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "WiFi scan done");
            break;
        default:
            break;
        }
    }
}

void WifiService::_ip_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
    WifiService *self = static_cast<WifiService *>(arg);

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        self->_sta_connected.store(true, std::memory_order_release);

        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));

        /* Publish wifi_state */
        int8_t rssi = 0;
        int rssi_raw = 0;
        esp_wifi_sta_get_rssi(&rssi_raw);
        rssi = (int8_t)rssi_raw;
        self->_publish_wifi_state(true, self->_current_ssid, rssi);

        /* Start mDNS */
        _start_mdns();

        /* Start SNTP on first connect */
        if (!self->_sntp_started.load(std::memory_order_acquire)) {
            self->set_sntp_started();
            _start_sntp();
        }

        xEventGroupSetBits(self->_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t WifiService::init() {
    if (_initialized.load(std::memory_order_acquire)) return ESP_OK;

    _scan_mutex = xSemaphoreCreateMutexStatic(&_scan_mutex_buf);
    if (!_scan_mutex) return ESP_ERR_NO_MEM;

    _wifi_event_group = xEventGroupCreate();
    if (!_wifi_event_group) return ESP_ERR_NO_MEM;

    /* Initialize TCP/IP stack + event loop */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Default Wi-Fi event loop */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Create default STA and AP netifs */
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* Initialize Wi-Fi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register event handlers */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            &_wifi_event_handler, this, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            &_ip_event_handler, this, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Set Wi-Fi mode to STA + AP */
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register SNTP sync callback */
    sntp_set_time_sync_notification_cb(_sntp_sync_cb);

    _initialized.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "WiFi service initialized");
    return ESP_OK;
}

esp_err_t WifiService::start() {
    if (!_initialized.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "WifiService not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Configure SoftAP */
    wifi_config_t ap_cfg = {};
    strlcpy((char *)ap_cfg.ap.ssid, WIFI_AP_SSID_PREFIX, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, WIFI_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP config failed: %s", esp_err_to_name(err));
    }

    /* Check NVS for stored STA credentials */
    char ssid[33] = {};
    char pass[65] = {};
    bool has_creds = _read_nvs_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (has_creds) {
        wifi_config_t sta_cfg = {};
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(err));
        }
        strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
        ESP_LOGI(TAG, "Starting WiFi with STA: ssid=%s", ssid);
    } else {
        ESP_LOGI(TAG, "Starting WiFi in AP-only provisioning mode");
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    _started.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t WifiService::scan(wifi_scan_result_t *results, uint16_t *out_count) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!results || !out_count) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t number = WIFI_SERVICE_SCAN_MAX;
    wifi_ap_record_t ap_info[WIFI_SERVICE_SCAN_MAX];

    esp_wifi_scan_start(nullptr, true);
    esp_err_t err = esp_wifi_scan_get_ap_records(&number, ap_info);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < number; i++) {
            strlcpy(results[i].ssid, (const char *)ap_info[i].ssid, sizeof(results[i].ssid));
            results[i].rssi = ap_info[i].rssi;
            results[i].authmode = ap_info[i].authmode;
        }
        *out_count = number;
    }

    xSemaphoreGive(_scan_mutex);
    return err;
}

esp_err_t WifiService::connect(const char *ssid, const char *password) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    wifi_config_t sta_cfg = {};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    if (password && strlen(password) > 0) {
        strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    }
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Save credentials to NVS BEFORE attempting to connect,
     * so they persist even if the network is temporarily unavailable. */
    _save_nvs_creds(ssid, password);
    strlcpy(_current_ssid, ssid, sizeof(_current_ssid));

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for connection (blocking, caller runs in a task) */
    EventBits_t bits = xEventGroupWaitBits(_wifi_event_group,
            WIFI_CONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s", ssid);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Connect to %s timed out (credentials saved, will retry on boot)", ssid);
    return ESP_ERR_TIMEOUT;
}

esp_err_t WifiService::disconnect() {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    return esp_wifi_disconnect();
}

esp_err_t WifiService::apply_sta_config(const char *ssid, const char *password) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;

    /* Disconnect first, then apply new config and reconnect */
    esp_wifi_disconnect();

    wifi_config_t sta_cfg = {};
    if (ssid && strlen(ssid) > 0) {
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        if (password && strlen(password) > 0) {
            strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
        }
    }
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi reconnect failed: %s", esp_err_to_name(err));
        return err;
    }

    if (ssid && strlen(ssid) > 0) {
        _save_nvs_creds(ssid, password);
        strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
    }
    return ESP_OK;
}

void WifiService::get_status(wifi_service_status_t *status) {
    if (!status) return;

    status->sta_connected = _sta_connected.load(std::memory_order_acquire);
    status->sta_configured = (strlen(_current_ssid) > 0);

    /* Get IP from STA netif */
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(status->sta_ip, sizeof(status->sta_ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    strlcpy(status->sta_ssid, _current_ssid, sizeof(status->sta_ssid));
    status->sta_rssi = 0;
    if (status->sta_connected) {
        int rssi_raw = 0;
        esp_wifi_sta_get_rssi(&rssi_raw);
        status->sta_rssi = (int8_t)rssi_raw;
    }
}

esp_err_t WifiService::wait_connected(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(_wifi_event_group,
            WIFI_CONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

void *WifiService::get_ap_netif() {
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}