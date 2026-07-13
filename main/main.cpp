/**
 * @file main.cpp
 * @brief ESP32-S31-Korvo-1 main application entry point.
 *
 * Boot sequence:
 *   1. NVS init
 *   2. uORB init (IPC message bus)
 *   3. SD Card mount (if available)
 *   4. Audio driver init (ES8389 codec)
 *   5. WiFi init + start (STA or SoftAP provisioning)
 *   6. Web config server start
 *   7. Text logger start (SD card)
 *   8. ULog writer start (binary logging)
 *   9. System monitor start
 */

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_mac.h"

#include "uorb.h"
#include "ulog_writer.h"
#include "wifi_service.hpp"
#include "web_config_server.hpp"
#include "example_config.h"
#include "topics.h"

#include "drivers/audio/audio_driver.hpp"
#include "drivers/sdcard/sdcard_driver.hpp"
#include "drivers/system_monitor/system_monitor.hpp"
#include "logger/logger.hpp"
#include "git_info.h"

static const char *TAG = "main";

/* ── mDNS hostname ──────────────────────────────────────────────── */
static char s_mdns_unique_hostname[24] = {0};

static void _build_mdns_hostname() {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_mdns_unique_hostname, sizeof(s_mdns_unique_hostname),
                 "esp-web-%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        strlcpy(s_mdns_unique_hostname, "esp-web", sizeof(s_mdns_unique_hostname));
    }
    ESP_LOGI(TAG, "Device mDNS hostname: %s", s_mdns_unique_hostname);
}

/* ── ULog startup ───────────────────────────────────────────────── */
static void _start_ulog() {
    ulog_init_config_t ulog_cfg = {};
    ulog_cfg.session_counter = 0;
    ulog_cfg.has_wall_clock = WifiService::instance().sntp_synced();
    strlcpy(ulog_cfg.sys_name, "esp32s31_korvo1", sizeof(ulog_cfg.sys_name));
    strlcpy(ulog_cfg.ver_sw, GIT_COMMIT, sizeof(ulog_cfg.ver_sw));
    strlcpy(ulog_cfg.ver_hw, "ESP32-S31-Korvo-1", sizeof(ulog_cfg.ver_hw));
    strlcpy(ulog_cfg.arch, "esp32s31", sizeof(ulog_cfg.arch));
    strlcpy(ulog_cfg.sys_mcu, "ESP32-S31", sizeof(ulog_cfg.sys_mcu));
    strlcpy(ulog_cfg.sys_os_name, "FreeRTOS", sizeof(ulog_cfg.sys_os_name));
    strlcpy(ulog_cfg.sys_os_ver, esp_get_idf_version(), sizeof(ulog_cfg.sys_os_ver));

    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(ulog_cfg.sys_uuid, sizeof(ulog_cfg.sys_uuid),
                 "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    ulog_writer_t *writer = ulog_writer_get();
    if (ulog_writer_init(writer, SDMMC_MOUNT_POINT, &ulog_cfg) == ESP_OK) {
        /* Set git info */
        ulog_git_info_t gi = {};
        strlcpy(gi.branch, GIT_BRANCH, sizeof(gi.branch));
        strlcpy(gi.commit, GIT_COMMIT, sizeof(gi.commit));
        strlcpy(gi.author, GIT_AUTHOR, sizeof(gi.author));
        strlcpy(gi.date, GIT_DATE, sizeof(gi.date));
        strlcpy(gi.message, GIT_MSG, sizeof(gi.message));
        ulog_writer_set_git_info(writer, &gi);

        /* Register topics for logging */
        ulog_writer_add_topic(writer, ORB_ID(wifi_state), 500);
        ulog_writer_add_topic(writer, ORB_ID(system_stats), 5000);
        ulog_writer_add_topic(writer, ORB_ID(volume_state), 1000);

        if (ulog_writer_start(writer) == ESP_OK) {
            ESP_LOGI(TAG, "ULog writer started");
        } else {
            ESP_LOGW(TAG, "ULog writer start failed");
        }
    } else {
        ESP_LOGW(TAG, "ULog writer init failed (SD card not available?)");
    }
}

/* ── Main ───────────────────────────────────────────────────────── */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S31-Korvo-1 booting...");
    ESP_LOGI(TAG, "Firmware: %s (%s)", GIT_LOG1, GIT_DATE);
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    /* 1. Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* 2. Initialize uORB (IPC message bus) */
    orb_init();
    ESP_LOGI(TAG, "uORB initialized");

    /* 3. Build mDNS hostname */
    _build_mdns_hostname();

    /* 4. Mount SD card */
    bool sd_ok = SDCardDriver::instance().init();
    if (sd_ok) {
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "SD card not available — audio playback/storage disabled");
    }

    /* 5. Initialize audio driver */
    AudioDriver::instance().init();
    if (AudioDriver::instance().available()) {
        ESP_LOGI(TAG, "Audio driver initialized");
    } else {
        ESP_LOGW(TAG, "Audio driver not available");
    }

    /* 6. Initialize WiFi service (which also initializes esp_netif) */
    ret = WifiService::instance().init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi service init failed: %s", esp_err_to_name(ret));
    } else {
        ret = WifiService::instance().start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi service start failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "WiFi service started");
        }
    }

    /* 7. Start web config server */
    web_config_server_start();

    /* 8. Start text logger (SD card) */
    if (sd_ok) {
        if (logger_init(SDMMC_MOUNT_POINT)) {
            logger_set_sd_level((logger_level_t)CONFIG_APP_LOG_SD_LEVEL);
            ESP_LOGI(TAG, "Text logger started");
        } else {
            ESP_LOGW(TAG, "Text logger init failed");
        }
    }

    /* 9. Start ULog writer (binary logging) */
    if (sd_ok) {
        _start_ulog();
    }

    /* 10. Start system monitor */
    if (SystemMonitor::instance().init()) {
        SystemMonitor::instance().start();
        ESP_LOGI(TAG, "System monitor started");
    } else {
        ESP_LOGW(TAG, "System monitor init failed");
    }

    ESP_LOGI(TAG, "Boot complete. Web config: http://esp-web-XXXXXX.local:8080");
    ESP_LOGI(TAG, "Free heap: %lu KB", esp_get_free_heap_size() / 1024);

    /* Main loop — print periodic status */
    while (true) {
        wifi_service_status_t wifi_st;
        WifiService::instance().get_status(&wifi_st);

        ESP_LOGI(TAG, "[Status] WiFi: %s (%s), Volume: %d, SD: %s, Free heap: %lu KB",
                 wifi_st.sta_connected ? wifi_st.sta_ssid : "AP mode",
                 wifi_st.sta_connected ? wifi_st.sta_ip : "192.168.4.1",
                 AudioDriver::instance().volume(),
                 sd_ok ? "ok" : "none",
                 esp_get_free_heap_size() / 1024);

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}