#pragma once

#include <atomic>

/*
 * AudioDriver — manages ES8389 audio codec via I2S + I2C.
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
#include "uorb.h"

class AudioDriver {
public:
    static AudioDriver& instance();

    /** Initialize audio I2S + I2C + ES8389 codec (refcounted). Thread-safe. */
    void init();

    /** Deinitialize audio (refcounted). Thread-safe. */
    void deinit();

    /** @return true if audio is currently initialized */
    bool available() const { return _refcount.load(std::memory_order_relaxed) > 0; }

    /* Audio handles (read-only access) */
    i2s_chan_handle_t rx_handle() const { return _rx_handle; }
    i2s_chan_handle_t tx_handle() const { return _tx_handle; }

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

    /* Delete copy/move */
    AudioDriver(const AudioDriver&) = delete;
    AudioDriver& operator=(const AudioDriver&) = delete;

private:
    AudioDriver();
    ~AudioDriver();

    /** Write to ES8389 register via I2C. */
    esp_err_t _codec_write_reg(uint8_t reg, uint8_t value);
    /** Read from ES8389 register via I2C. */
    esp_err_t _codec_read_reg(uint8_t reg, uint8_t *value);

    /** Initialize ES8389 codec with default settings. */
    esp_err_t _es8389_init();

    SemaphoreHandle_t       _lifecycle_mutex;
    std::atomic<SemaphoreHandle_t> _codec_mutex;
    std::atomic<int>        _refcount;
    std::atomic<int>        _volume;

    i2s_chan_handle_t        _rx_handle;
    i2s_chan_handle_t        _tx_handle;
    i2c_master_bus_handle_t  _i2c_bus;
    i2c_master_dev_handle_t  _i2c_dev;

    std::atomic<orb_advert_t>   _vol_pub;
    std::atomic<int>            _codec_ops_in_flight;
};