#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web config server FreeRTOS task.
 *
 * Launches an HTTP server on port 8080 with a 4-tab web UI for:
 *   - WiFi: Auto-scan (10s), select SSID → password modal
 *   - Audio Recording (WAV PCM, I2S RX → SD card, status with file path)
 *   - Music Playback (esp_audio_simple_player, SD card WAV/MP3, play_status poll, auto-refresh 5s)
 *   - File Manager (list / download / delete with status bar updates)
 *   - ULog Control (start / stop / status with file path, auto-refresh 3s)
 *   - Speaker Volume (0-100, real-time slider with NVS debounce)
 *   - Timezone (GET/POST /api/system/timezone)
 *   - System information (auto-refresh 5s, NTP sync, current time, stats)
 *
 * Command APIs return full status in response for immediate UI update.
 * Client uses optimistic UI updates: button state changes instantly on click,
 * then syncs from command response. No second round-trip needed.
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

/**
 * @brief Check if .aac file recording is currently active.
 * Used by AudioUlogRecorder for mutual exclusion (shared I2S).
 */
bool web_config_is_aac_recording(void);

/**
 * @brief Check if audio playback is currently active.
 * Used by AudioUlogRecorder for mutual exclusion (shared I2S).
 */
bool web_config_is_playing(void);

#ifdef __cplusplus
}
#endif