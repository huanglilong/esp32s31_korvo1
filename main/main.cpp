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
 *   8. ULog writer init (auto-starts after SNTP sync via web_config_task)
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
#include "mdns.h"
#include "lwip/apps/netbiosns.h"

#include "uorb.h"
#include "ulog_writer.h"
#include "wifi_service.hpp"
#include "web_config_server.hpp"
#include "app_config.h"
#include "topics.h"

#include "drivers/audio/audio_driver.hpp"
#include "drivers/sdcard/sdcard_driver.hpp"
#include "drivers/camera/camera_driver.hpp"
#include "drivers/system_monitor/system_monitor.hpp"
#include "logger/logger.hpp"
#include "git_info.h"

static const char *TAG = "main";

/* ── Board device configs (esp_board_manager style) ────────────── */

static dev_audio_codec_config_t s_audio_cfg = {};
static dev_fs_fat_config_t s_sdcard_cfg = {};
static dev_camera_config_t s_camera_cfg = {};

static void _build_audio_config() {
    s_audio_cfg.name = AUDIO_CODEC_NAME_DAC;
    s_audio_cfg.chip = AUDIO_CODEC_CHIP;
    s_audio_cfg.adc_enabled = true;
    s_audio_cfg.dac_enabled = true;
    s_audio_cfg.adc_max_channel = 2;
    s_audio_cfg.dac_max_channel = 2;
    s_audio_cfg.adc_init_gain = 0;
    s_audio_cfg.dac_init_gain = 0;
    s_audio_cfg.mclk_enabled = true;
    s_audio_cfg.aec_enabled = false;

    /* Power amplifier config */
    s_audio_cfg.pa_cfg.port = 43;         /* GPIO43 controls NS4150B PA (CTRL pin) */
    s_audio_cfg.pa_cfg.gain = 0.0f;
    s_audio_cfg.pa_cfg.active_level = 1;  /* Active high */

    /* I2C config */
    s_audio_cfg.i2c_cfg.port = AUDIO_I2C_NUM;
    s_audio_cfg.i2c_cfg.address = AUDIO_I2C_ADDR;
    s_audio_cfg.i2c_cfg.frequency = AUDIO_I2C_FREQ;
    s_audio_cfg.i2c_cfg.sda_io = (int16_t)CONFIG_EXAMPLE_I2C_SDA_IO;
    s_audio_cfg.i2c_cfg.scl_io = (int16_t)CONFIG_EXAMPLE_I2C_SCL_IO;

    /* I2S config */
    s_audio_cfg.i2s_cfg.port = AUDIO_I2S_NUM;
    s_audio_cfg.i2s_cfg.clk_src = 0;  /* I2S_CLK_SRC_DEFAULT */
    s_audio_cfg.i2s_cfg.mclk_io = (int16_t)CONFIG_EXAMPLE_I2S_MCLK_IO;
    s_audio_cfg.i2s_cfg.bclk_io = (int16_t)CONFIG_EXAMPLE_I2S_BCLK_IO;
    s_audio_cfg.i2s_cfg.ws_io = (int16_t)CONFIG_EXAMPLE_I2S_WS_IO;
    s_audio_cfg.i2s_cfg.dout_io = (int16_t)CONFIG_EXAMPLE_I2S_DOUT_IO;
    s_audio_cfg.i2s_cfg.din_io = (int16_t)CONFIG_EXAMPLE_I2S_DIN_IO;
    s_audio_cfg.i2s_cfg.sample_rate_hz = 48000;
    s_audio_cfg.i2s_cfg.mclk_freq_hz = 48000 * 256;
    s_audio_cfg.i2s_cfg.tx_aux_out_io = -1;
}

static void _build_sdcard_config() {
    s_sdcard_cfg.name = "sdcard";
    s_sdcard_cfg.mount_point = SDMMC_MOUNT_POINT;
    s_sdcard_cfg.frequency = SDMMC_FREQ_HZ;
    s_sdcard_cfg.vfs_config.format_if_mount_failed = false;
    s_sdcard_cfg.vfs_config.max_files = 8;
    s_sdcard_cfg.vfs_config.allocation_unit_size = 16 * 1024;
    s_sdcard_cfg.sub_type = "sdmmc";

    s_sdcard_cfg.sdmmc.slot = 0;
    s_sdcard_cfg.sdmmc.bus_width = 4;
    s_sdcard_cfg.sdmmc.slot_flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_sdcard_cfg.sdmmc.pins.clk = CONFIG_EXAMPLE_PIN_CLK;
    s_sdcard_cfg.sdmmc.pins.cmd = CONFIG_EXAMPLE_PIN_CMD;
    s_sdcard_cfg.sdmmc.pins.d0 = CONFIG_EXAMPLE_PIN_D0;
    s_sdcard_cfg.sdmmc.pins.d1 = CONFIG_EXAMPLE_PIN_D1;
    s_sdcard_cfg.sdmmc.pins.d2 = CONFIG_EXAMPLE_PIN_D2;
    s_sdcard_cfg.sdmmc.pins.d3 = CONFIG_EXAMPLE_PIN_D3;
}

static void _build_camera_config() {
    s_camera_cfg.name = "camera";
    s_camera_cfg.type = "camera";
    s_camera_cfg.sub_type = "dvp";

    s_camera_cfg.sub_cfg.dvp.i2c_name = AUDIO_CODEC_NAME_ADC;  /* share I2C bus */
    s_camera_cfg.sub_cfg.dvp.i2c_freq = AUDIO_I2C_FREQ;
    s_camera_cfg.sub_cfg.dvp.reset_io = GPIO_NUM_NC;
    s_camera_cfg.sub_cfg.dvp.pwdn_io = GPIO_NUM_NC;
    s_camera_cfg.sub_cfg.dvp.xclk_freq = CONFIG_EXAMPLE_CAMERA_XCLK_FREQ;

    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[0] = CONFIG_EXAMPLE_CAMERA_D0_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[1] = CONFIG_EXAMPLE_CAMERA_D1_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[2] = CONFIG_EXAMPLE_CAMERA_D2_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[3] = CONFIG_EXAMPLE_CAMERA_D3_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[4] = CONFIG_EXAMPLE_CAMERA_D4_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[5] = CONFIG_EXAMPLE_CAMERA_D5_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[6] = CONFIG_EXAMPLE_CAMERA_D6_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.data_io[7] = CONFIG_EXAMPLE_CAMERA_D7_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.pclk_io = CONFIG_EXAMPLE_CAMERA_PCLK_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.hsync_io = CONFIG_EXAMPLE_CAMERA_HSYNC_IO;
    s_camera_cfg.sub_cfg.dvp.dvp_io.vsync_io = CONFIG_EXAMPLE_CAMERA_VSYNC_IO;
}

/* ── Shared mDNS (reference-counted) ──────────────────────────────── */
static SemaphoreHandle_t s_mdns_mutex = NULL;
static int  s_mdns_refcount = 0;
static bool s_mdns_initialized = false;

/* ── mDNS hostname ──────────────────────────────────────────────── */
static char s_mdns_unique_hostname[24] = {0};

const char *shared_mdns_hostname(void)
{
    return s_mdns_unique_hostname;
}

static void _build_mdns_hostname() {
    uint8_t mac[6];
    /* Use base MAC for consistent hostname across reboots */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK || esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_mdns_unique_hostname, sizeof(s_mdns_unique_hostname),
                 "esp-web-%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        strlcpy(s_mdns_unique_hostname, "esp-web", sizeof(s_mdns_unique_hostname));
    }
    ESP_LOGI(TAG, "Device mDNS hostname: %s", s_mdns_unique_hostname);
}

static SemaphoreHandle_t _mdns_mutex_get(void)
{
    return s_mdns_mutex;
}

void shared_mdns_mutex_init(void)
{
    if (!s_mdns_mutex) {
        s_mdns_mutex = xSemaphoreCreateMutex();
    }
}

bool shared_mdns_ensure(void)
{
    SemaphoreHandle_t mtx = _mdns_mutex_get();
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);

    if (s_mdns_initialized) {
        s_mdns_refcount++;
        if (mtx) xSemaphoreGive(mtx);
        return true;
    }
    if (mdns_init() != ESP_OK) {
        if (mtx) xSemaphoreGive(mtx);
        return false;
    }

    _build_mdns_hostname();

    /* Primary hostname: "esp-web" — convenient for single-device use. */
    mdns_hostname_set("esp-web");

    /* Delegated hostname: "esp-web-XXXXXX" — unique per device. */
    if (strcmp(s_mdns_unique_hostname, "esp-web") != 0) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            static mdns_ip_addr_t s_delegate_addr = {};
            s_delegate_addr.addr.u_addr.ip4 = ip_info.ip;
            s_delegate_addr.addr.type = ESP_IPADDR_TYPE_V4;
            s_delegate_addr.next = NULL;
            mdns_delegate_hostname_add(s_mdns_unique_hostname, &s_delegate_addr);
            ESP_LOGI(TAG, "mDNS: esp-web.local + %s.local -> " IPSTR,
                     s_mdns_unique_hostname, IP2STR(&ip_info.ip));
        } else {
            mdns_delegate_hostname_add(s_mdns_unique_hostname, NULL);
            ESP_LOGW(TAG, "mDNS: esp-web.local + %s.local (no IP yet)", s_mdns_unique_hostname);
        }
    } else {
        ESP_LOGI(TAG, "mDNS: esp-web.local");
    }

    netbiosns_init();
    netbiosns_set_name("esp-web");

    s_mdns_initialized = true;
    s_mdns_refcount = 1;

    if (mtx) xSemaphoreGive(mtx);
    return true;
}

void shared_mdns_release(void)
{
    SemaphoreHandle_t mtx = _mdns_mutex_get();
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);

    if (!s_mdns_initialized) {
        if (mtx) xSemaphoreGive(mtx);
        return;
    }
    s_mdns_refcount--;
    if (s_mdns_refcount <= 0) {
        mdns_free();
        s_mdns_initialized = false;
        s_mdns_refcount = 0;
        ESP_LOGI(TAG, "mDNS: fully deinitialized (last user released)");
    }

    if (mtx) xSemaphoreGive(mtx);
}

void shared_mdns_update_delegate_ip(void)
{
    SemaphoreHandle_t mtx = _mdns_mutex_get();
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);

    if (!s_mdns_initialized || strcmp(s_mdns_unique_hostname, "esp-web") == 0) {
        if (mtx) xSemaphoreGive(mtx);
        return;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        static mdns_ip_addr_t s_delegate_addr = {};
        s_delegate_addr.addr.u_addr.ip4 = ip_info.ip;
        s_delegate_addr.addr.type = ESP_IPADDR_TYPE_V4;
        s_delegate_addr.next = NULL;
        mdns_delegate_hostname_set_address(s_mdns_unique_hostname, &s_delegate_addr);
        ESP_LOGI(TAG, "mDNS delegate: %s.local -> " IPSTR,
                 s_mdns_unique_hostname, IP2STR(&ip_info.ip));
    }

    if (mtx) xSemaphoreGive(mtx);
}

/* ── ULog initialization (without start) ─────────────────────────── */
/* ULog is initialized here (register topics, set git info), but NOT started.
 * Actual start is deferred until SNTP time sync completes, handled by the
 * web_config_task in web_config_server.cpp. This ensures ULog files get
 * correct wall-clock timestamps. */
static void _init_ulog() {
    ulog_init_config_t ulog_cfg = {};
    ulog_cfg.session_counter = 0;
    ulog_cfg.has_wall_clock = false;  /* Will be set to true by SNTP callback */
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

        ESP_LOGI(TAG, "ULog writer initialized (will auto-start after SNTP sync)");
    } else {
        ESP_LOGW(TAG, "ULog writer init failed (SD card not available?)");
    }
}

/* ── Main ───────────────────────────────────────────────────────── */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S31-Korvo-1 booting...");
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

    /* 2a. Initialize shared mDNS mutex before any tasks */
    shared_mdns_mutex_init();

    /* 3. Build board device configs */
    _build_audio_config();
    _build_sdcard_config();
    _build_camera_config();

    /* 4. Mount SD card (config-driven) */
    void *sdcard_handle = nullptr;
    int sd_ret = SDCardDriver::instance().init(&s_sdcard_cfg, sizeof(s_sdcard_cfg), &sdcard_handle);
    bool sd_ok = (sd_ret == 0);
    if (sd_ok) {
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "SD card not available — audio playback/storage disabled");
    }

    /* 5. Initialize audio driver (config-driven) */
    void *audio_handle = nullptr;
    int audio_ret = AudioDriver::instance().init(&s_audio_cfg, sizeof(s_audio_cfg), &audio_handle);
    if (audio_ret == 0) {
        ESP_LOGI(TAG, "Audio driver initialized");
    } else {
        ESP_LOGW(TAG, "Audio driver not available");
    }

    /* 5a. Initialize camera driver (config-driven) */
    void *camera_handle = nullptr;
    int cam_ret = CameraDriver::instance().init(&s_camera_cfg, sizeof(s_camera_cfg), &camera_handle);
    if (cam_ret == 0) {
        ESP_LOGI(TAG, "Camera driver initialized");
    } else {
        ESP_LOGW(TAG, "Camera driver not available");
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

    /* 8a. Print git info after logger init so it's saved to SD card */
    ESP_LOGI(TAG, "Git Info: %s", GIT_LOG1);
    ESP_LOGI(TAG, "  branch:  %s", GIT_BRANCH);
    ESP_LOGI(TAG, "  commit:  %s", GIT_COMMIT);
    ESP_LOGI(TAG, "  author:  %s", GIT_AUTHOR);
    ESP_LOGI(TAG, "  date:    %s", GIT_DATE);
    ESP_LOGI(TAG, "  message: %s", GIT_MSG);

    /* 9. Initialize ULog writer (auto-starts after SNTP sync via web_config_task) */
    if (sd_ok) {
        _init_ulog();
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