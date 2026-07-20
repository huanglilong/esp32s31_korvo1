/**
 * @file wifi_service.cpp
 * @brief C++ facade over Brookesia service_wifi — unified WiFi service for ESP32-S31.
 *
 * WiFi lifecycle (init/start) is managed by the Brookesia ServiceManager.
 * This facade provides scan, connect, disconnect, and status query through
 * the Brookesia WiFi service helper API.
 */

#include "wifi_service.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "app_config.h"

#if CONFIG_APP_BROOKESIA_BUILD
#include "brookesia/service_wifi.hpp"
#include "brookesia/service_manager/service/base.hpp"
#endif

static const char *TAG = "wifi_service";

/* ── Singleton ──────────────────────────────────────────────────── */

WifiService& WifiService::instance() {
    static WifiService s;
    return s;
}

/* ── Private helpers ─────────────────────────────────────────────── */

bool WifiService::_is_service_ready() const {
#if CONFIG_APP_BROOKESIA_BUILD
    using WifiHelper = esp_brookesia::service::helper::Wifi;
    return WifiHelper::is_available() && WifiHelper::is_running();
#else
    return false;
#endif
}

void WifiService::_read_sta_info(wifi_service_status_t *status) {
    if (!status) return;

    /* Default: disconnected */
    status->sta_connected = false;
    status->sta_configured = false;
    status->sta_ip[0] = '\0';
    status->sta_ssid[0] = '\0';
    status->sta_rssi = 0;

#if CONFIG_APP_BROOKESIA_BUILD
    using WifiHelper = esp_brookesia::service::helper::Wifi;

    if (!_is_service_ready()) {
        return;
    }

    /* Query general state */
    auto state_result = WifiHelper::call_function_sync<std::string>(
        WifiHelper::FunctionId::GetGeneralState,
        esp_brookesia::service::helper::Timeout(3000)
    );
    if (state_result.has_value()) {
        const std::string &state = state_result.value();
        status->sta_connected = (state == "Connected");
        status->sta_configured = (state == "Connected" || state == "Started" ||
                                  state == "Connecting" || state == "Disconnecting");
    }

    /* If connected, get AP info from the station interface */
    if (status->sta_connected) {
        auto ap_result = WifiHelper::call_function_sync<boost::json::object>(
            WifiHelper::FunctionId::GetConnectAp,
            esp_brookesia::service::helper::Timeout(3000)
        );
        if (ap_result.has_value()) {
            const auto &obj = ap_result.value();
            auto it = obj.find("ssid");
            if (it != obj.end() && it->value().is_string()) {
                std::string ssid = std::string(it->value().as_string());
                strlcpy(status->sta_ssid, ssid.c_str(), sizeof(status->sta_ssid));
                strlcpy(_current_ssid, ssid.c_str(), sizeof(_current_ssid));
            }
        }

        /* Get STA IP from ESP-IDF netif (most reliable source) */
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                snprintf(status->sta_ip, sizeof(status->sta_ip),
                         IPSTR, IP2STR(&ip_info.ip));
            }
        }

        /* Get RSSI */
        int rssi_raw = 0;
        if (esp_wifi_sta_get_rssi(&rssi_raw) == ESP_OK) {
            status->sta_rssi = (int8_t)rssi_raw;
        }
    }
#endif
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t WifiService::init() {
    /* No-op: Brookesia ServiceManager handles WiFi init via auto-registered plugin. */
    ESP_LOGI(TAG, "WiFi service init (delegated to Brookesia ServiceManager)");
    return ESP_OK;
}

esp_err_t WifiService::start() {
    /* No-op: Brookesia ServiceManager handles WiFi start via auto-registered plugin.
     * The service_wifi plugin auto-loads persisted credentials and auto-connects
     * on start. If no credentials are stored, SoftAP provisioning starts. */
    ESP_LOGI(TAG, "WiFi service start (delegated to Brookesia ServiceManager)");
    return ESP_OK;
}

esp_err_t WifiService::scan(wifi_scan_result_t *results, uint16_t *out_count) {
    if (!results || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

#if CONFIG_APP_BROOKESIA_BUILD
    using WifiHelper = esp_brookesia::service::helper::Wifi;

    if (!_is_service_ready()) {
        ESP_LOGW(TAG, "WiFi service not ready for scan");
        return ESP_ERR_INVALID_STATE;
    }

    /* Trigger scan start */
    auto start_result = WifiHelper::call_function_sync(
        WifiHelper::FunctionId::TriggerScanStart,
        esp_brookesia::service::helper::Timeout(5000)
    );
    if (!start_result.has_value()) {
        ESP_LOGW(TAG, "Failed to trigger WiFi scan");
        return ESP_FAIL;
    }

    /* Wait for scan to complete (scan results are published via event).
     * Poll the general state until scan is done or timeout.
     * Brookesia scan typically takes 2-5 seconds. */
    bool scan_done = false;
    int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = 15000 * 1000;  /* 15 seconds */

    while (!scan_done && (esp_timer_get_time() - start_us) < timeout_us) {
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Check if we can get scan results via GetConnectedAps or
         * by checking general state has moved past scanning.
         * The simplest approach: use esp_wifi_scan_get_ap_records directly
         * since Brookesia uses the same ESP-IDF WiFi stack underneath. */
        uint16_t ap_count = 0;
        esp_err_t err = esp_wifi_scan_get_ap_num(&ap_count);
        if (err == ESP_OK && ap_count > 0) {
            scan_done = true;
        }
    }

    if (!scan_done) {
        ESP_LOGW(TAG, "WiFi scan timed out");
        return ESP_ERR_TIMEOUT;
    }

    /* Get scan results directly from ESP-IDF */
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return ESP_OK;
    if (ap_count > WIFI_SERVICE_SCAN_MAX) ap_count = WIFI_SERVICE_SCAN_MAX;

    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        strlcpy(results[i].ssid, (const char *)ap_records[i].ssid, sizeof(results[i].ssid));
        results[i].rssi = ap_records[i].rssi;
        results[i].authmode = ap_records[i].authmode;
    }
    *out_count = ap_count;
    free(ap_records);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "WiFi scan not available (Brookesia not enabled)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t WifiService::connect(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

#if CONFIG_APP_BROOKESIA_BUILD
    using WifiHelper = esp_brookesia::service::helper::Wifi;

    if (!_is_service_ready()) {
        ESP_LOGW(TAG, "WiFi service not ready for connect");
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    /* Set target AP credentials */
    auto set_result = WifiHelper::call_function_sync(
        WifiHelper::FunctionId::SetConnectAp,
        std::string(ssid),
        std::string(password ? password : ""),
        esp_brookesia::service::helper::Timeout(5000)
    );
    if (!set_result.has_value()) {
        ESP_LOGW(TAG, "Failed to set connect AP: %s", set_result.error().c_str());
        return ESP_FAIL;
    }

    /* Trigger connect action */
    auto connect_result = WifiHelper::call_function_sync(
        WifiHelper::FunctionId::TriggerGeneralAction,
        std::string("Connect"),
        esp_brookesia::service::helper::Timeout(5000)
    );
    if (!connect_result.has_value()) {
        ESP_LOGW(TAG, "Failed to trigger connect: %s", connect_result.error().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STA connect triggered for %s", ssid);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "WiFi connect not available (Brookesia not enabled)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t WifiService::disconnect() {
#if CONFIG_APP_BROOKESIA_BUILD
    using WifiHelper = esp_brookesia::service::helper::Wifi;

    if (!_is_service_ready()) return ESP_ERR_INVALID_STATE;

    auto result = WifiHelper::call_function_sync(
        WifiHelper::FunctionId::TriggerGeneralAction,
        std::string("Disconnect"),
        esp_brookesia::service::helper::Timeout(5000)
    );
    if (!result.has_value()) {
        ESP_LOGW(TAG, "Failed to trigger disconnect: %s", result.error().c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void WifiService::get_status(wifi_service_status_t *status) {
    if (!status) return;
    _read_sta_info(status);
}

esp_err_t WifiService::wait_connected(uint32_t timeout_ms) {
    int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while ((esp_timer_get_time() - start_us) < timeout_us) {
        wifi_service_status_t st;
        _read_sta_info(&st);
        if (st.sta_connected) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return ESP_ERR_TIMEOUT;
}

void *WifiService::get_ap_netif() {
    /* Brookesia manages netif internally.
     * Return the AP netif from ESP-IDF for compatibility. */
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

esp_err_t WifiService::apply_sta_config(const char *ssid, const char *password) {
    if (ssid && strlen(ssid) > 0) {
        return connect(ssid, password);
    }
    return disconnect();
}
