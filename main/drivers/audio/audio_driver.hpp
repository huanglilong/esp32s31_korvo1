#pragma once

#include <atomic>

/*
 * AudioDriver — manages ES8389 audio codec via esp_codec_dev.
 *
 * Follows esp_board_manager Audio Codec device pattern:
 *   - dev_audio_codec_config_t: YAML-like configuration struct
 *   - dev_audio_codec_handles_t: device handle with codec_dev + interfaces
 *   - Uses esp_codec_dev APIs: audio_codec_new_i2s_data(), esp_codec_dev_new(), etc.
 *   - This enables bridge to ESP-GMF pipeline elements later
 *
 * Reference-counted init/deinit, thread-safe codec operations.
 * Publishes volume_state via uORB when volume changes.
 *
 * Hardware: ES8389 stereo codec (I2S duplex + I2C control).
 *   - Dual analog mic input → ADC → I2S RX
 *   - I2S TX → DAC → NS4150B PA → Speakers
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "uorb.h"

/* ── esp_board_manager-compatible config structures ────────────── */

/** Power amplifier configuration (matches esp_board_manager board_codec_pa_t) */
typedef struct {
    int16_t     port;          /*!< GPIO port number, -1 if not used */
    float       gain;          /*!< Amplifier gain value (dB) */
    int16_t     active_level;  /*!< Active level (1=high, 0=low) */
} board_codec_pa_t;

/** I2C interface configuration (matches esp_board_manager board_codec_i2c_t) */
typedef struct {
    int8_t      port;       /*!< I2C port number */
    int16_t     address;    /*!< I2C device address (7-bit, left-shifted) */
    int32_t     frequency;  /*!< I2C clock frequency in Hz */
    int16_t     sda_io;     /*!< SDA GPIO */
    int16_t     scl_io;     /*!< SCL GPIO */
} board_codec_i2c_t;

/** I2S interface configuration (matches esp_board_manager board_codec_i2s_t) */
typedef struct {
    int8_t      port;               /*!< I2S port number */
    int         clk_src;            /*!< I2S clock source */
    int16_t     mclk_io;            /*!< MCLK GPIO, -1 if not used */
    int16_t     bclk_io;            /*!< BCLK GPIO */
    int16_t     ws_io;              /*!< WS/LRCK GPIO */
    int16_t     dout_io;            /*!< Data out (DAC) GPIO, -1 if not used */
    int16_t     din_io;             /*!< Data in (ADC) GPIO, -1 if not used */
    uint32_t    sample_rate_hz;     /*!< Sample rate in Hz */
    uint32_t    mclk_freq_hz;       /*!< MCLK frequency in Hz */
    int16_t     tx_aux_out_io;      /*!< Aux TX output IO, -1=disabled */
} board_codec_i2s_t;

/** Audio codec configuration (matches esp_board_manager dev_audio_codec_config_t) */
typedef struct {
    const char       *name;           /*!< Codec device name */
    const char       *chip;           /*!< Codec chip type ("es8389", "es8311", etc.) */
    bool              adc_enabled;    /*!< Enable ADC (mic input) */
    bool              dac_enabled;    /*!< Enable DAC (speaker output) */
    uint8_t           adc_max_channel;/*!< Max ADC channels */
    uint8_t           dac_max_channel;/*!< Max DAC channels */
    int               adc_init_gain;  /*!< ADC initial gain in dB */
    int               dac_init_gain;  /*!< DAC initial gain in dB */
    bool              mclk_enabled;   /*!< Enable MCLK output */
    bool              aec_enabled;    /*!< Enable AEC */
    board_codec_pa_t  pa_cfg;         /*!< Power amplifier config */
    board_codec_i2c_t i2c_cfg;        /*!< I2C interface config */
    board_codec_i2s_t i2s_cfg;        /*!< I2S interface config */
} dev_audio_codec_config_t;

/** Audio codec device handles (matches esp_board_manager dev_audio_codec_handles_t) */
typedef struct {
    esp_codec_dev_handle_t       codec_dev;   /*!< Codec device handle */
    const audio_codec_data_if_t *data_if;     /*!< Data interface handle */
    const audio_codec_ctrl_if_t *ctrl_if;     /*!< Control interface handle */
    const audio_codec_gpio_if_t *gpio_if;     /*!< GPIO interface handle */
    const audio_codec_if_t      *codec_if;    /*!< Codec interface (ES8389) */
    i2s_chan_handle_t            tx_handle;   /*!< I2S TX channel handle */
    i2s_chan_handle_t            rx_handle;   /*!< I2S RX channel handle */
    void                        *i2c_handle;  /*!< I2C bus handle (opaque) */
} dev_audio_codec_handles_t;

/* ── AudioDriver class ─────────────────────────────────────────── */

class AudioDriver {
public:
    static AudioDriver& instance();

    /**
     * @brief  Initialize audio codec with given configuration.
     *         Follows esp_board_manager dev_audio_codec_init() signature.
     *         Uses esp_codec_dev APIs. Reference-counted. Thread-safe.
     *
     * @param[in]  cfg      Pointer to dev_audio_codec_config_t
     * @param[in]  cfg_size Size of config struct
     * @param[out] handle   Pointer to receive dev_audio_codec_handles_t*
     * @return 0 on success, -1 on failure
     */
    int init(dev_audio_codec_config_t *cfg, int cfg_size, void **handle);

    /** Deinitialize audio (refcounted). Thread-safe. */
    void deinit();

    /** @return true if audio is currently initialized */
    bool available() const { return _refcount.load(std::memory_order_relaxed) > 0; }

    /** @return the device handles struct */
    const dev_audio_codec_handles_t* handles() const {
        return _handles.load(std::memory_order_acquire);
    }

    /** @return the esp_codec_dev handle for ESP-GMF integration */
    esp_codec_dev_handle_t codec_dev_handle() const;

    /* Thread-safe codec operations */

    /** Set speaker output volume (0-100). Publishes volume_state via uORB. */
    void set_volume(int volume);

    /** Get current volume (cached). */
    int volume() const { return _volume.load(std::memory_order_relaxed); }

    /** Set microphone input gain (dB). Thread-safe. */
    void set_mic_gain(int gain_db);

    /** Write PCM data to codec DAC. Thread-safe.
     *  @return data_size on success, -1 on failure */
    int codec_write(const uint8_t *data, int size);

    /** Read PCM data from codec ADC. Thread-safe.
     *  @return data_size on success, -1 on failure */
    int codec_read(uint8_t *data, int size);

    /* Delete copy/move */
    AudioDriver(const AudioDriver&) = delete;
    AudioDriver& operator=(const AudioDriver&) = delete;

private:
    AudioDriver();
    ~AudioDriver();

    /** Initialize I2C bus for codec control. */
    esp_err_t _init_i2c_ctrl(const board_codec_i2c_t *i2c_cfg);

    /** Initialize I2S data interface. */
    esp_err_t _init_i2s_data(const board_codec_i2s_t *i2s_cfg, bool is_tx);

    /** Initialize ES8389 codec via esp_codec_dev. */
    esp_err_t _init_es8389_codec(const dev_audio_codec_config_t *cfg);

    SemaphoreHandle_t       _lifecycle_mutex;
    std::atomic<int>        _refcount;
    std::atomic<int>        _volume;

    std::atomic<dev_audio_codec_handles_t*> _handles;

    std::atomic<orb_advert_t>   _vol_pub;
};
