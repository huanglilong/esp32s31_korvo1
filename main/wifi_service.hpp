#pragma once

/**
 * @file wifi_service.hpp
 * @brief Thin C++ facade over the wifi_manager component.
 *
 * Wraps wifi_manager C API, manages uORB wifi_state publication,
 * SNTP startup, and NVS credential persistence. Provides a unified
 * WiFi service with captive portal support.
 *
 * Usage:
 *   1. At boot: WifiService::instance().init() — initializes wifi_manager.
 *   2. WifiService::instance().start() — reads NVS credentials, starts
 *      wifi_manager in STA+AP mode.
 *   3. From UI/Web: scan(), connect(ssid, pass), getStatus().
 *   4. WifiService publishes uORB wifi_state automatically via
 *      the wifi_manager state callback.
 */

#include <atomic>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "uorb.h"

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

    /* One-time init: init wifi_manager, register state callback for
     * uORB wifi_state publishing. Must be called before start(). */
    esp_err_t init();

    /* Start WiFi with NVS-stored credentials. If NVS has no SSID,
     * starts in AP-only mode (provisioning). Returns immediately;
     * connection status available via get_status() or uORB. */
    esp_err_t start();

    /* Blocking scan: populates |results| (max WIFI_SERVICE_SCAN_MAX records).
     * Returns actual count via |out_count|. */
    esp_err_t scan(wifi_scan_result_t *results, uint16_t *out_count);

    /* Connect to a new network. Saves to NVS before connecting so
     * credentials persist even if the network is temporarily unavailable. */
    esp_err_t connect(const char *ssid, const char *password);

    /* Disconnect from current network. STA interface is stopped
     * but AP keeps running for provisioning. */
    esp_err_t disconnect();

    /* Get current status snapshot (thread-safe). */
    void get_status(wifi_service_status_t *status);

    /* Wait up to |timeout_ms| for STA connection.
     * Returns ESP_OK on connect, ESP_ERR_TIMEOUT on timeout. */
    esp_err_t wait_connected(uint32_t timeout_ms);

    /* Get the wifi_manager's AP netif (for captive_dns). */
    void *get_ap_netif();

    /* Hot-swap STA credentials at runtime (no restart required).
     * Saves to NVS immediately. Reconnects with new config. */
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

    /* Read NVS credentials into ssid_out/pass_out. Returns true if
     * SSID is non-empty (STA mode), false if no stored credentials. */
    bool _read_nvs_creds(char *ssid_out, size_t ssid_size,
                         char *pass_out, size_t pass_size);

    /* Save credentials to NVS. */
    void _save_nvs_creds(const char *ssid, const char *password);

    /* Publish uORB wifi_state message. Thread-safe. */
    void _publish_wifi_state(bool connected, const char *ssid, int8_t rssi);

    std::atomic<bool> _initialized{false};
    std::atomic<bool> _started{false};
    std::atomic<bool> _sntp_started{false};
    std::atomic<bool> _sntp_synced{false};
    std::atomic<bool> _captive_dns_started{false};

    /* uORB wifi_state publisher */
    std::atomic<orb_advert_t> _wifi_state_pub{ORB_ADVERT_INVALID};

    StaticSemaphore_t _scan_mutex_buf;
    SemaphoreHandle_t _scan_mutex{nullptr};

    char _current_ssid[33] = {};
};

#endif /* __cplusplus */