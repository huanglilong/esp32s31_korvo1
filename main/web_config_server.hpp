#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web config server FreeRTOS task.
 *
 * Launches an HTTP server on port 8080 with a web UI for configuring:
 *   - WiFi SSID / Password
 *   - Speaker Volume (0-100)
 *   - Timezone (GET/POST /api/system/timezone, Web UI selector)
 *   - System information (including NTP sync status and current time)
 *
 * Also starts a background task that:
 *   - Detects WiFi STA connection (GOT_IP) and starts SNTP time sync
 *   - Auto-starts ULog logging after SNTP time is synced
 *   - Logs WiFi up/down transitions and SNTP waiting state
 *
 * Settings are persisted to NVS namespace "settings".
 * Designed for headless boards without an LCD.
 */
void web_config_server_start(void);

/**
 * @brief Stop the web config server and cleanup.
 */
void web_config_server_stop(void);

#ifdef __cplusplus
}
#endif