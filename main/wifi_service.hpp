#pragma once

/**
 * @file wifi_service.hpp
 * @brief Thin C++ facade over the Brookesia WiFi service.
 *
 * Wraps the Brookesia service_wifi helper API, providing a simplified
 * interface for the web config server and main app. Preserves the same
 * public API as the legacy wifi_manager-based implementation.
 *
 * WiFi lifecycle (init/start/connect) is managed entirely by the
 * Brookesia ServiceManager via auto-registered plugin. This class
 * only queries state and triggers actions through the service helper.
 *
 * Usage:
 *   1. Brookesia ServiceManager auto-starts WiFi service at boot.
 *   2. From UI/Web: scan(), connect(ssid, pass), get_status().
 *   3. SNTP is managed separately by the web_config_server task.
 */

#include <atomic>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum AP records per scan */
#define WIFI_SERVICE_SCAN_MAX 20

/* Scan result returned by WifiService::scan() */
struct wifi_scan_result_t {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
};

/* WiFi status snapshot */
struct wifi_service_status_t {
    bool sta_connected;
    bool sta_configured;
    char sta_ip[16];
    char sta_ssid[33];
    int8_t sta_rssi;
};

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class WifiService {
public:
    static WifiService& instance();

    /* No-op init — Brookesia ServiceManager handles WiFi lifecycle.
     * Kept for API compatibility; always returns ESP_OK. */
    esp_err_t init();

    /* No-op start — Brookesia ServiceManager handles WiFi lifecycle.
     * Kept for API compatibility; always returns ESP_OK. */
    esp_err_t start();

    /* Blocking scan: populates |results| (max WIFI_SERVICE_SCAN_MAX records).
     * Returns actual count via |out_count|. */
    esp_err_t scan(wifi_scan_result_t *results, uint16_t *out_count);

    /* Connect to a new network. Triggers Brookesia WiFi service
     * SetConnectAp + TriggerGeneralAction(Connect). */
    esp_err_t connect(const char *ssid, const char *password);

    /* Disconnect from current network. */
    esp_err_t disconnect();

    /* Get current status snapshot (thread-safe). */
    void get_status(wifi_service_status_t *status);

    /* Wait up to |timeout_ms| for STA connection.
     * Returns ESP_OK on connect, ESP_ERR_TIMEOUT on timeout. */
    esp_err_t wait_connected(uint32_t timeout_ms);

    /* Get the AP netif — returns nullptr (Brookesia manages netif internally).
     * Kept for API compatibility. */
    void *get_ap_netif();

    /* Hot-swap STA credentials at runtime (no restart required). */
    esp_err_t apply_sta_config(const char *ssid, const char *password);

    /* Whether SNTP has been started (called once after first IP). */
    bool sntp_started() const { return _sntp_started.load(std::memory_order_acquire); }
    void set_sntp_started() { _sntp_started.store(true, std::memory_order_release); }

    /* Whether SNTP has synced. */
    bool sntp_synced() const { return _sntp_synced.load(std::memory_order_acquire); }
    void set_sntp_synced() { _sntp_synced.store(true, std::memory_order_release); }

    /* Whether WiFi is initialized (for guard checks). */
    bool initialized() const { return _initialized.load(std::memory_order_acquire); }

private:
    WifiService() = default;
    ~WifiService() = default;
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    static void _state_callback(bool connected, void *user_ctx);

    /* Check if Brookesia WiFi service is available and running. */
    bool _is_service_ready() const;

    /* Read current STA info from Brookesia WiFi service. */
    void _read_sta_info(wifi_service_status_t *status);

    std::atomic<bool> _initialized{true};   /* Always true — Brookesia manages init */
    std::atomic<bool> _started{true};       /* Always true — Brookesia manages start */
    std::atomic<bool> _sntp_started{false};
    std::atomic<bool> _sntp_synced{false};

    char _current_ssid[33] = {};
};

#endif /* __cplusplus */