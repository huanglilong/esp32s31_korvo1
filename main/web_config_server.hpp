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
 *   - System information
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