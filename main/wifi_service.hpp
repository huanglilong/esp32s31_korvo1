#pragma once

/**
 * @file wifi_service.hpp
 * @brief WiFi service — manages Wi-Fi STA + SoftAP + SNTP + mDNS
 *
 * For headless boards (no LCD), provides:
 *   - STA auto-connect from NVS
 *   - SoftAP provisioning fallback when no stored credentials
 *   - Captive portal DNS redirect for phone-based WiFi setup
 *   - SNTP time sync after first connect
 *   - mDNS advertisement (esp-web.local)
 *   - uORB wifi_state publication
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

/* Scan result */
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

    /* One-time init: creates netifs, event loop, wifi driver. */
    esp_err_t init();

    /* Start WiFi with NVS-stored credentials. If NVS has no SSID,
     * starts SoftAP provisioning mode. */
    esp_err_t start();

    /* Blocking scan (max 20 records). */
    esp_err_t scan(wifi_scan_result_t *results, uint16_t *out_count);

    /* Connect to a new network. Saves to NVS on success. */
    esp_err_t connect(const char *ssid, const char *password);

    /* Disconnect from current network. STA is stopped but
     * SoftAP keeps running for provisioning. */
    esp_err_t disconnect();

    /* Get current status snapshot (thread-safe). */
    void get_status(wifi_service_status_t *status);

    /* Wait up to |timeout_ms| for STA connection. */
    esp_err_t wait_connected(uint32_t timeout_ms);

    /* Get the SoftAP netif handle. */
    void *get_ap_netif();

    /* Hot-swap STA credentials at runtime. */
    esp_err_t apply_sta_config(const char *ssid, const char *password);

    bool sntp_started() const { return _sntp_started.load(std::memory_order_acquire); }
    void set_sntp_started() { _sntp_started.store(true, std::memory_order_release); }

    bool sntp_synced() const { return _sntp_synced.load(std::memory_order_acquire); }
    void set_sntp_synced() { _sntp_synced.store(true, std::memory_order_release); }

    bool initialized() const { return _initialized.load(std::memory_order_acquire); }

private:
    WifiService() = default;
    ~WifiService() = default;
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    static void _wifi_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);
    static void _ip_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data);
    void _publish_wifi_state(bool connected, const char *ssid, int8_t rssi);
    bool _read_nvs_creds(char *ssid_out, size_t ssid_size,
                         char *pass_out, size_t pass_size);
    void _save_nvs_creds(const char *ssid, const char *password);

    std::atomic<bool> _initialized{false};
    std::atomic<bool> _started{false};
    std::atomic<bool> _sntp_started{false};
    std::atomic<bool> _sntp_synced{false};
    std::atomic<bool> _captive_dns_started{false};
    std::atomic<bool> _sta_connected{false};

    std::atomic<orb_advert_t> _wifi_state_pub{ORB_ADVERT_INVALID};

    StaticSemaphore_t _scan_mutex_buf;
    SemaphoreHandle_t _scan_mutex{nullptr};

    char _current_ssid[33] = {};
    EventGroupHandle_t _wifi_event_group{nullptr};
};

#endif /* __cplusplus */