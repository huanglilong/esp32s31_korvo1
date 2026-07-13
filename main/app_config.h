#pragma once
#include "sdkconfig.h"
#include "hal/gpio_types.h"
#include "driver/i2c_master.h"
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif

/* ── NVS shared keys ────────────────────────────────────────────── */
#define NVS_NAMESPACE_SETTINGS        "settings"
#define NVS_KEY_WIFI_SSID             "ssid"
#define NVS_KEY_WIFI_PASS             "pass"
#define NVS_KEY_VOLUME                "volume"
#define NVS_KEY_BRIGHTNESS            "brightness"
#define NVS_KEY_CAM_STREAM            "cam_stream"
#define NVS_KEY_TIMEZONE              "timezone"

/* ── Volume / Brightness constants ──────────────────────────────── */
#define VOLUME_MIN                    0
#define VOLUME_MAX                    100
#define VOLUME_DEFAULT                60
#define BRIGHTNESS_MIN                20
#define BRIGHTNESS_MAX                100
#define BRIGHTNESS_DEFAULT            80

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
#define AUDIO_I2C_ADDR        (0x10)   /* ES8389 I2C address (7-bit, left-shifted) */
#define AUDIO_I2C_FREQ        (400000) /* 400 kHz */

/* ── Audio I2S ──────────────────────────────────────────────────── */
#define AUDIO_I2S_NUM         (0)
#define AUDIO_I2S_MCK_IO      static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_MCLK_IO)
#define AUDIO_I2S_BCK_IO      static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_BCLK_IO)
#define AUDIO_I2S_WS_IO       static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_WS_IO)
#define AUDIO_I2S_DO_IO       static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_DOUT_IO)
#define AUDIO_I2S_DI_IO       static_cast<gpio_num_t>(CONFIG_EXAMPLE_I2S_DIN_IO)

/* ── Audio Codec (ES8389) ──────────────────────────────────────── */
#define AUDIO_CODEC_CHIP      "es8389"
#define AUDIO_CODEC_NAME_ADC  "audio_adc"
#define AUDIO_CODEC_NAME_DAC  "audio_dac"

/* ── SNTP config ─────────────────────────────────────────────────── */
#ifndef CONFIG_SNTP_SERVER_0
#define CONFIG_SNTP_SERVER_0 "pool.ntp.org"
#endif
#ifndef CONFIG_SNTP_DEFAULT_TIMEZONE
#define CONFIG_SNTP_DEFAULT_TIMEZONE "CST-8"
#endif
/* ── SD Card ────────────────────────────────────────────────────── */
#define SDMMC_MOUNT_POINT     "/sdcard"
#define SDMMC_FREQ_HZ         (40000000)  /* 40 MHz SDMMC clock */

/* ── mDNS ───────────────────────────────────────────────────────── */
#define EXAMPLE_MDNS_HOST_NAME "esp-web"

/* Shared mDNS initialization guard with reference counting.
 * MUST be called after WiFi is connected (delegated hostname needs IP).
 * Individual modules should only add/remove their own services,
 * never call mdns_free() directly. */
void shared_mdns_mutex_init(void);
bool shared_mdns_ensure(void);
void shared_mdns_release(void);
void shared_mdns_update_delegate_ip(void);

/* Returns the unique mDNS hostname (e.g. "esp-web-a1b2c3").
 * Valid after shared_mdns_ensure() has been called. */
const char *shared_mdns_hostname(void);

#ifdef __cplusplus
}
#endif