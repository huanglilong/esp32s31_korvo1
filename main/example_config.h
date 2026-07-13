#pragma once
#include "sdkconfig.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── NVS shared keys ────────────────────────────────────────────── */
#define NVS_NAMESPACE_SETTINGS        "settings"
#define NVS_KEY_WIFI_SSID             "ssid"
#define NVS_KEY_WIFI_PASS             "pass"
#define NVS_KEY_VOLUME                "volume"
#define NVS_KEY_CAM_STREAM            "cam_stream"

/* ── Volume constants ───────────────────────────────────────────── */
#define VOLUME_MIN                    0
#define VOLUME_MAX                    100
#define VOLUME_DEFAULT                60

/* ── WiFi event group bits ──────────────────────────────────────── */
#define WIFI_CONNECTED_BIT            BIT0

/* ── Audio I2S ──────────────────────────────────────────────────── */
#define EXAMPLE_AUDIO_SAMPLE_RATE     (48000)
#define EXAMPLE_AUDIO_MCLK_FREQ_HZ    (EXAMPLE_AUDIO_SAMPLE_RATE * 256)
#define EXAMPLE_VOICE_VOLUME          CONFIG_EXAMPLE_VOICE_VOLUME

/* ── Audio I2C ──────────────────────────────────────────────────── */
#define AUDIO_I2C_NUM         (0)
#define AUDIO_I2C_SCL_IO      static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2C_SCL_IO)
#define AUDIO_I2C_SDA_IO      static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2C_SDA_IO)

/* ── Audio I2S ──────────────────────────────────────────────────── */
#define AUDIO_I2S_NUM         (0)
#define AUDIO_I2S_MCK_IO      static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_MCLK_IO)
#define AUDIO_I2S_BCK_IO      static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_BCLK_IO)
#define AUDIO_I2S_WS_IO       static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_WS_IO)
#define AUDIO_I2S_DO_IO       static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_DOUT_IO)
#define AUDIO_I2S_DI_IO       static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_DIN_IO)

/* ── SD Card ────────────────────────────────────────────────────── */
#define SDMMC_MOUNT_POINT     "/sdcard"

/* ── mDNS ───────────────────────────────────────────────────────── */
#define EXAMPLE_MDNS_HOST_NAME "esp-web"

#ifdef __cplusplus
}
#endif