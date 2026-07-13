/*
 * AudioDriver — ES8389 audio codec driver for ESP32-S31-Korvo-1.
 *
 * Reference-counted init/deinit, thread-safe codec register access,
 * volume control via ES8389 hardware registers.
 */

#include "audio_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include <cstring>

#include "app_config.h"
#include "topics.h"

static const char *TAG = "AudioDriver";

/* ES8389 I2C address */
#define ES8389_ADDR  0x10

/* ES8389 register map (partial — enough for basic init + volume) */
#define ES8389_CONTROL1       0x00
#define ES8389_CONTROL2       0x01
#define ES8389_CHIP_POWER      0x02
#define ES8389_ADCPOWER        0x03
#define ES8389_DACPOWER        0x04
#define ES8389_CHIP_LOPOW1     0x05
#define ES8389_CHIP_LOPOW2     0x06
#define ES8389_ANAVOL_MANAGER  0x07
#define ES8389_MASTERMODE      0x08
#define ES8389_ADCCONTROL1     0x09
#define ES8389_ADCCONTROL2     0x0A
#define ES8389_ADCCONTROL3     0x0B
#define ES8389_ADCCONTROL4     0x0C
#define ES8389_ADCCONTROL5     0x0D
#define ES8389_ADCCONTROL6     0x0E
#define ES8389_ADCCONTROL7     0x0F
#define ES8389_ADCCONTROL8     0x10
#define ES8389_ADCCONTROL9     0x11
#define ES8389_ADCCONTROL10    0x12
#define ES8389_ADCCONTROL11    0x13
#define ES8389_ADCCONTROL12    0x14
#define ES8389_ADCCONTROL13    0x15
#define ES8389_ADCCONTROL14    0x16
#define ES8389_DACCONTROL1     0x17
#define ES8389_DACCONTROL2     0x18
#define ES8389_DACCONTROL3     0x19
#define ES8389_DACCONTROL4     0x1A
#define ES8389_DACCONTROL5     0x1B
#define ES8389_DACCONTROL6     0x1C
#define ES8389_DACCONTROL7     0x1D
#define ES8389_DACCONTROL8     0x1E
#define ES8389_DACCONTROL9     0x1F
#define ES8389_DACCONTROL10    0x20
#define ES8389_DACCONTROL11    0x21
#define ES8389_DACCONTROL12    0x22
#define ES8389_DACCONTROL13    0x23
#define ES8389_DACCONTROL14    0x24
#define ES8389_DACCONTROL15    0x25
#define ES8389_DACCONTROL16    0x26
#define ES8389_DACCONTROL17    0x27
#define ES8389_DACCONTROL18    0x28
#define ES8389_DACCONTROL19    0x29
#define ES8389_DACCONTROL20    0x2A
#define ES8389_DACCONTROL21    0x2B
#define ES8389_DACCONTROL22    0x2C
#define ES8389_DACCONTROL23    0x2D
#define ES8389_DACCONTROL24    0x2E
#define ES8389_DACCONTROL25    0x2F

/*============================================================================
 * Singleton
 *============================================================================*/
AudioDriver& AudioDriver::instance() {
    static AudioDriver s;
    return s;
}

AudioDriver::AudioDriver() :
    _lifecycle_mutex(nullptr),
    _codec_mutex(nullptr),
    _refcount(0),
    _volume(VOLUME_DEFAULT),
    _rx_handle(nullptr),
    _tx_handle(nullptr),
    _i2c_bus(nullptr),
    _i2c_dev(nullptr),
    _vol_pub(ORB_ADVERT_INVALID),
    _codec_ops_in_flight(0)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

AudioDriver::~AudioDriver() {
    deinit();
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
}

/*============================================================================
 * ES8389 I2C helpers
 *============================================================================*/
esp_err_t AudioDriver::_codec_write_reg(uint8_t reg, uint8_t value) {
    if (!_i2c_dev) return ESP_ERR_INVALID_STATE;

    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(_i2c_dev, data, sizeof(data), pdMS_TO_TICKS(100));
}

esp_err_t AudioDriver::_codec_read_reg(uint8_t reg, uint8_t *value) {
    if (!_i2c_dev) return ESP_ERR_INVALID_STATE;

    return i2c_master_transmit_receive(_i2c_dev, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

/*============================================================================
 * ES8389 initialization sequence
 *============================================================================*/
esp_err_t AudioDriver::_es8389_init() {
    esp_err_t ret;

    /* Reset ES8389 */
    ret = _codec_write_reg(ES8389_CONTROL2, 0x58);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = _codec_write_reg(ES8389_CONTROL2, 0x00);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Power up analog and digital blocks */
    ret = _codec_write_reg(ES8389_CHIP_POWER, 0xFF);
    if (ret != ESP_OK) return ret;

    ret = _codec_write_reg(ES8389_ADCPOWER, 0x00);  /* Power up ADC */
    if (ret != ESP_OK) return ret;

    ret = _codec_write_reg(ES8389_DACPOWER, 0x3C);  /* Power up DAC (L+R) */
    if (ret != ESP_OK) return ret;

    /* Set master mode: ES8389 as slave (I2S clocks from ESP32-S31) */
    ret = _codec_write_reg(ES8389_MASTERMODE, 0x00);
    if (ret != ESP_OK) return ret;

    /* Configure ADC: I2S 16-bit, stereo */
    ret = _codec_write_reg(ES8389_ADCCONTROL1, 0x00);
    if (ret != ESP_OK) return ret;
    ret = _codec_write_reg(ES8389_ADCCONTROL2, 0x18);  /* 16-bit I2S */
    if (ret != ESP_OK) return ret;
    ret = _codec_write_reg(ES8389_ADCCONTROL3, 0x02);
    if (ret != ESP_OK) return ret;
    ret = _codec_write_reg(ES8389_ADCCONTROL4, 0x0C);  /* ADC gain 0dB */
    if (ret != ESP_OK) return ret;

    /* Configure DAC: I2S 16-bit, stereo */
    ret = _codec_write_reg(ES8389_DACCONTROL1, 0x18);  /* 16-bit I2S */
    if (ret != ESP_OK) return ret;
    ret = _codec_write_reg(ES8389_DACCONTROL2, 0x02);
    if (ret != ESP_OK) return ret;
    ret = _codec_write_reg(ES8389_DACCONTROL3, 0x02);
    if (ret != ESP_OK) return ret;

    /* Unmute DAC */
    ret = _codec_write_reg(ES8389_DACCONTROL4, 0x00);
    if (ret != ESP_OK) return ret;
    ret = _codec_write_reg(ES8389_DACCONTROL5, 0x00);
    if (ret != ESP_OK) return ret;

    /* Set initial volume */
    set_volume(_volume.load(std::memory_order_relaxed));

    ESP_LOGI(TAG, "ES8389 initialized");
    return ESP_OK;
}

/*============================================================================
 * Init / Deinit (refcounted)
 *============================================================================*/
void AudioDriver::init() {
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    int ref = _refcount.load(std::memory_order_relaxed);
    if (ref > 0) {
        _refcount.store(ref + 1, std::memory_order_relaxed);
        xSemaphoreGive(_lifecycle_mutex);
        ESP_LOGI(TAG, "AudioDriver already initialized, refcount=%d", ref + 1);
        return;
    }

    esp_err_t ret;

    /* 1. Initialize I2C bus */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = AUDIO_I2C_NUM,
        .sda_io_num = AUDIO_I2C_SDA_IO,
        .scl_io_num = AUDIO_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    ret = i2c_new_master_bus(&i2c_cfg, &_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    /* 2. Add ES8389 I2C device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8389_ADDR,
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8389 I2C device add failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(_i2c_bus);
        _i2c_bus = nullptr;
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    /* 3. Initialize I2S channels */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, &_tx_handle, &_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel init failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(_i2c_dev);
        _i2c_dev = nullptr;
        i2c_del_master_bus(_i2c_bus);
        _i2c_bus = nullptr;
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    /* Configure I2S standard mode */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)AUDIO_I2S_MCK_IO,
            .bclk = (gpio_num_t)AUDIO_I2S_BCK_IO,
            .ws   = (gpio_num_t)AUDIO_I2S_WS_IO,
            .dout = (gpio_num_t)AUDIO_I2S_DO_IO,
            .din  = (gpio_num_t)AUDIO_I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    /* Configure TX channel */
    ret = i2s_channel_init_std_mode(_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Configure RX channel */
    ret = i2s_channel_init_std_mode(_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Create codec mutex */
    {
        SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
        if (!mtx) {
            ESP_LOGE(TAG, "Codec mutex create failed");
            goto cleanup;
        }
        _codec_mutex.store(mtx, std::memory_order_release);
    }

    /* Initialize ES8389 codec */
    ret = _es8389_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8389 init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Enable I2S channels */
    i2s_channel_enable(_tx_handle);
    i2s_channel_enable(_rx_handle);

    /* 7. Restore volume from NVS */
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
            int32_t saved_vol = VOLUME_DEFAULT;
            nvs_get_i32(h, NVS_KEY_VOLUME, &saved_vol);
            nvs_close(h);
            set_volume((int)saved_vol);
        }
    }

    _refcount.store(1, std::memory_order_relaxed);
    xSemaphoreGive(_lifecycle_mutex);
    ESP_LOGI(TAG, "AudioDriver initialized (48kHz, 16-bit, stereo)");
    return;

cleanup:
    /* Delete codec mutex if created */
    if (_codec_mutex.load(std::memory_order_relaxed)) {
        SemaphoreHandle_t mtx_cleanup = _codec_mutex.load(std::memory_order_relaxed);
        _codec_mutex.store(nullptr, std::memory_order_release);
        vSemaphoreDelete(mtx_cleanup);
    }
    if (_rx_handle) {
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
    }
    if (_tx_handle) {
        i2s_del_channel(_tx_handle);
        _tx_handle = nullptr;
    }
    if (_i2c_dev) {
        i2c_master_bus_rm_device(_i2c_dev);
        _i2c_dev = nullptr;
    }
    if (_i2c_bus) {
        i2c_del_master_bus(_i2c_bus);
        _i2c_bus = nullptr;
    }
    xSemaphoreGive(_lifecycle_mutex);
}

void AudioDriver::deinit() {
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    int ref = _refcount.load(std::memory_order_relaxed);
    if (ref <= 0) {
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    if (ref > 1) {
        _refcount.store(ref - 1, std::memory_order_relaxed);
        xSemaphoreGive(_lifecycle_mutex);
        ESP_LOGI(TAG, "AudioDriver deinit skipped, refcount=%d", ref - 1);
        return;
    }

    _refcount.store(0, std::memory_order_relaxed);

    /* Wait for in-flight codec ops */
    for (int i = 0; i < 100 && _codec_ops_in_flight.load(std::memory_order_acquire) > 0; i++) {
        xSemaphoreGive(_lifecycle_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);
    }

    /* Disable I2S */
    if (_tx_handle) {
        i2s_channel_disable(_tx_handle);
        i2s_del_channel(_tx_handle);
        _tx_handle = nullptr;
    }
    if (_rx_handle) {
        i2s_channel_disable(_rx_handle);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
    }

    /* Remove I2C device */
    if (_i2c_dev) {
        i2c_master_bus_rm_device(_i2c_dev);
        _i2c_dev = nullptr;
    }
    if (_i2c_bus) {
        i2c_del_master_bus(_i2c_bus);
        _i2c_bus = nullptr;
    }

    /* Delete codec mutex */
    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_relaxed);
    if (mtx) {
        vSemaphoreDelete(mtx);
        _codec_mutex.store(nullptr, std::memory_order_release);
    }

    xSemaphoreGive(_lifecycle_mutex);
    ESP_LOGI(TAG, "AudioDriver deinitialized");
}

/*============================================================================
 * Codec operations (thread-safe)
 *============================================================================*/
void AudioDriver::set_volume(int volume) {
    if (volume < VOLUME_MIN) volume = VOLUME_MIN;
    if (volume > VOLUME_MAX) volume = VOLUME_MAX;

    _volume.store(volume, std::memory_order_relaxed);

    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_acquire);
    if (!mtx) return;

    xSemaphoreTake(mtx, portMAX_DELAY);

    /* ES8389 DAC volume: 0-33 steps, 0.5dB per step. Map 0-100 to 0-33. */
    uint8_t vol_reg = (uint8_t)(volume * 33 / 100);
    _codec_write_reg(ES8389_DACCONTROL24, vol_reg);  /* Left volume */
    _codec_write_reg(ES8389_DACCONTROL25, vol_reg);  /* Right volume */

    xSemaphoreGive(mtx);

    /* Publish volume_state via uORB */
    orb_advert_t pub = _vol_pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t expected = ORB_ADVERT_INVALID;
        orb_advert_t new_pub = orb_advertise(ORB_ID(volume_state));
        _vol_pub.compare_exchange_strong(expected, new_pub,
                std::memory_order_release, std::memory_order_acquire);
        pub = _vol_pub.load(std::memory_order_acquire);
    }
    if (pub != ORB_ADVERT_INVALID) {
        volume_state_s vs = {};
        vs.timestamp = esp_timer_get_time();
        vs.volume = volume;
        orb_publish(ORB_ID(volume_state), pub, &vs);
    }

    ESP_LOGI(TAG, "Volume set to %d", volume);
}

void AudioDriver::set_mic_gain(int gain_db) {
    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_acquire);
    if (!mtx) return;

    xSemaphoreTake(mtx, portMAX_DELAY);

    /* ES8389 ADC gain: 0-24dB, 3dB steps. Register 0x0C bits [4:0] */
    uint8_t gain_reg = 0;
    if (gain_db >= 24) gain_reg = 8;
    else if (gain_db >= 21) gain_reg = 7;
    else if (gain_db >= 18) gain_reg = 6;
    else if (gain_db >= 15) gain_reg = 5;
    else if (gain_db >= 12) gain_reg = 4;
    else if (gain_db >= 9) gain_reg = 3;
    else if (gain_db >= 6) gain_reg = 2;
    else if (gain_db >= 3) gain_reg = 1;
    _codec_write_reg(ES8389_ADCCONTROL4, gain_reg);

    xSemaphoreGive(mtx);
    ESP_LOGI(TAG, "Mic gain set to %d dB", gain_db);
}

int AudioDriver::codec_write(const uint8_t *data, int size) {
    if (!data || size <= 0) return -1;

    i2s_chan_handle_t tx = _tx_handle;
    if (!tx) return -1;

    /* Increment in-flight counter */
    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx, data, size, &bytes_written, pdMS_TO_TICKS(1000));

    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

    if (ret != ESP_OK) {
        return -1;
    }
    return (int)bytes_written;
}