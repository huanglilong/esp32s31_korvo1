/*
 * AudioDriver — ES8389 audio codec driver for ESP32-S31-Korvo-1.
 *
 * Follows esp_board_manager Audio Codec device pattern:
 *   - init() takes dev_audio_codec_config_t + returns dev_audio_codec_handles_t*
 *   - Uses esp_codec_dev APIs (audio_codec_new_i2s_data, esp_codec_dev_new, etc.)
 *   - Config-driven: I2C/I2S pins and settings from config struct
 *   - Reference-counted init/deinit, thread-safe codec access
 *   - Bridges to ESP-GMF via esp_codec_dev_handle_t
 */

#include "audio_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include <cstring>

#include "app_config.h"
#include "topics.h"

static const char *TAG = "AudioDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
AudioDriver& AudioDriver::instance() {
    static AudioDriver s;
    return s;
}

AudioDriver::AudioDriver() :
    _lifecycle_mutex(nullptr),
    _codec_mutex(SemaphoreHandle_t(nullptr)),
    _refcount(0),
    _volume(VOLUME_DEFAULT),
    _codec_ops_in_flight(0),
    _handles(nullptr),
    _vol_pub(ORB_ADVERT_INVALID)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

AudioDriver::~AudioDriver() {
    deinit();
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
    SemaphoreHandle_t cm = _codec_mutex.exchange(nullptr, std::memory_order_acq_rel);
    if (cm) {
        vSemaphoreDelete(cm);
    }
}

/*============================================================================
 * I2C control interface init
 *============================================================================*/
esp_err_t AudioDriver::_init_i2c_ctrl(const board_codec_i2c_t *i2c_cfg) {
    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (!h || !i2c_cfg) return ESP_ERR_INVALID_STATE;

    /* First create the I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (i2c_port_num_t)i2c_cfg->port,
        .sda_io_num = (gpio_num_t)i2c_cfg->sda_io,
        .scl_io_num = (gpio_num_t)i2c_cfg->scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    i2c_master_bus_handle_t bus_handle = nullptr;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus (port=%d, sda=%d, scl=%d): %s",
                 (int)i2c_cfg->port, (int)i2c_cfg->sda_io, (int)i2c_cfg->scl_io,
                 esp_err_to_name(ret));
        return ret;
    }

    /* Let esp_codec_dev create its own I2C device on top of our bus.
     * Pass bus_handle so it doesn't create another bus. */
    audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
        .port = (uint8_t)i2c_cfg->port,
        .addr = (uint8_t)(i2c_cfg->address & 0xFF),
        .bus_handle = bus_handle,
    };

    h->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
    if (!h->ctrl_if) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        i2c_del_master_bus(bus_handle);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*============================================================================
 * I2S data interface init
 *============================================================================*/
esp_err_t AudioDriver::_init_i2s_data(const board_codec_i2s_t *i2s_cfg, bool is_tx) {
    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (!h || !i2s_cfg) return ESP_ERR_INVALID_STATE;

    /* Create I2S channel pair */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
            i2s_cfg->port, I2S_ROLE_MASTER);
    i2s_chan_handle_t tx_handle = nullptr;
    i2s_chan_handle_t rx_handle = nullptr;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channels: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure I2S standard mode */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)i2s_cfg->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)i2s_cfg->mclk_io,
            .bclk = (gpio_num_t)i2s_cfg->bclk_io,
            .ws   = (gpio_num_t)i2s_cfg->ws_io,
            .dout = (gpio_num_t)i2s_cfg->dout_io,
            .din  = (gpio_num_t)i2s_cfg->din_io,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    /* Init TX channel */
    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S TX: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        i2s_del_channel(tx_handle);
        return ret;
    }

    /* Init RX channel */
    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S RX: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        i2s_del_channel(tx_handle);
        return ret;
    }

    /* Enable channels */
    i2s_channel_enable(tx_handle);
    i2s_channel_enable(rx_handle);

    /* Store handles */
    h->tx_handle = tx_handle;
    h->rx_handle = rx_handle;

    /* Create esp_codec_dev data interface with pre-created handles */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = (uint8_t)i2s_cfg->port,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
        .clk_src = i2s_cfg->clk_src,
    };

    h->data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!h->data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        i2s_channel_disable(tx_handle);
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        i2s_del_channel(tx_handle);
        h->tx_handle = nullptr;
        h->rx_handle = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2S data interface created (%lu Hz, stereo)",
             (unsigned long)i2s_cfg->sample_rate_hz);
    return ESP_OK;
}

/*============================================================================
 * ES8389 codec init via esp_codec_dev
 *============================================================================*/
esp_err_t AudioDriver::_init_es8389_codec(const dev_audio_codec_config_t *cfg) {
    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (!h || !cfg) return ESP_ERR_INVALID_STATE;

    /* Create GPIO interface for PA control */
    h->gpio_if = audio_codec_new_gpio();
    if (!h->gpio_if) {
        ESP_LOGE(TAG, "Failed to create GPIO interface");
        return ESP_FAIL;
    }

    /* Determine codec work mode */
    esp_codec_dev_type_t work_mode = ESP_CODEC_DEV_TYPE_NONE;
    if (cfg->dac_enabled) {
        work_mode = (esp_codec_dev_type_t)(work_mode | ESP_CODEC_DEV_TYPE_OUT);
    }
    if (cfg->adc_enabled) {
        work_mode = (esp_codec_dev_type_t)(work_mode | ESP_CODEC_DEV_TYPE_IN);
    }

    /* Configure ES8389 codec */
    es8389_codec_cfg_t es_cfg = {
        .ctrl_if = h->ctrl_if,
        .gpio_if = h->gpio_if,
        .codec_mode = (esp_codec_dec_work_mode_t)work_mode,
        .pa_pin = cfg->pa_cfg.port,
        .pa_reverted = (cfg->pa_cfg.active_level == 1) ? false : true,
        .master_mode = false,  /* ESP32-S31 is I2S master */
        .use_mclk = cfg->mclk_enabled,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_gain = cfg->pa_cfg.gain,
        },
        .no_dac_ref = false,
        .mclk_div = 256,
    };

    h->codec_if = es8389_codec_new(&es_cfg);
    if (!h->codec_if) {
        ESP_LOGE(TAG, "Failed to create ES8389 codec interface");
        return ESP_FAIL;
    }

    /* Create unified codec device — field order: dev_type, codec_if, data_if */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = work_mode,
        .codec_if = h->codec_if,
        .data_if = h->data_if,
    };
    h->codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!h->codec_dev) {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_FAIL;
    }

    /* Open codec with sample rate/format — field order: bits_per_sample, channel, channel_mask, sample_rate, mclk_multiple */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = (uint8_t)((cfg->dac_max_channel > 0) ? cfg->dac_max_channel : 2),
        .channel_mask = 0,
        .sample_rate = (uint32_t)cfg->i2s_cfg.sample_rate_hz,
        .mclk_multiple = 256,
    };
    esp_err_t ret = (esp_err_t)esp_codec_dev_open(h->codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open codec device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ES8389 codec initialized via esp_codec_dev");
    return ESP_OK;
}

/*============================================================================
 * Init / Deinit (refcounted, config-driven, esp_codec_dev-based)
 *============================================================================*/
int AudioDriver::init(dev_audio_codec_config_t *cfg, int cfg_size, void **handle) {
    if (!cfg || !handle) {
        ESP_LOGE(TAG, "Invalid parameters: cfg=%p, handle=%p", (void*)cfg, (void*)handle);
        return -1;
    }
    if (cfg_size != (int)sizeof(dev_audio_codec_config_t)) {
        ESP_LOGE(TAG, "Invalid cfg_size: %d (expected %d)", cfg_size, (int)sizeof(dev_audio_codec_config_t));
        return -1;
    }

    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    int ref = _refcount.load(std::memory_order_relaxed);
    if (ref > 0) {
        _refcount.store(ref + 1, std::memory_order_relaxed);
        if (handle) *handle = _handles.load(std::memory_order_acquire);
        xSemaphoreGive(_lifecycle_mutex);
        ESP_LOGI(TAG, "AudioDriver already initialized, refcount=%d", ref + 1);
        return 0;
    }

    /* Allocate handles struct */
    dev_audio_codec_handles_t *h = new(std::nothrow) dev_audio_codec_handles_t();
    if (!h) {
        ESP_LOGE(TAG, "Failed to allocate handles");
        xSemaphoreGive(_lifecycle_mutex);
        return -1;
    }
    memset(h, 0, sizeof(*h));
    _handles.store(h, std::memory_order_release);

    esp_err_t ret;

    /* 1. Initialize I2C control interface */
    ret = _init_i2c_ctrl(&cfg->i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C control init failed");
        goto cleanup;
    }

    /* 2. Initialize I2S data interface */
    ret = _init_i2s_data(&cfg->i2s_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S data init failed");
        goto cleanup;
    }

    /* 3. Initialize ES8389 codec via esp_codec_dev */
    ret = _init_es8389_codec(cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8389 codec init failed");
        goto cleanup;
    }

    /* Create codec mutex for thread-safe codec ops */
    {
        SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
        if (!mtx) {
            ESP_LOGE(TAG, "Codec mutex create failed");
            goto cleanup;
        }
        _codec_mutex.store(mtx, std::memory_order_release);
    }

    /* 4. Set initial volume */
    set_volume(_volume.load(std::memory_order_relaxed));

    /* 5. Restore volume from NVS */
    {
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &nvs_h) == ESP_OK) {
            int32_t saved_vol = VOLUME_DEFAULT;
            nvs_get_i32(nvs_h, NVS_KEY_VOLUME, &saved_vol);
            nvs_close(nvs_h);
            set_volume((int)saved_vol);
        }
    }

    _refcount.store(1, std::memory_order_relaxed);
    if (handle) *handle = h;
    xSemaphoreGive(_lifecycle_mutex);
    ESP_LOGI(TAG, "AudioDriver initialized via esp_codec_dev (%lu Hz, 16-bit, stereo)",
             (unsigned long)cfg->i2s_cfg.sample_rate_hz);
    return 0;

cleanup:
    dev_audio_codec_handles_t *h_cleanup = _handles.load(std::memory_order_acquire);
    if (h_cleanup) {
        if (h_cleanup->codec_dev) {
            esp_codec_dev_close(h_cleanup->codec_dev);
            esp_codec_dev_delete(h_cleanup->codec_dev);
            h_cleanup->codec_dev = nullptr;
        }
        if (h_cleanup->codec_if) {
            /* codec_if is owned by esp_codec_dev, no explicit free */
            h_cleanup->codec_if = nullptr;
        }
        if (h_cleanup->data_if) {
            audio_codec_delete_data_if(h_cleanup->data_if);
            h_cleanup->data_if = nullptr;
        }
        if (h_cleanup->ctrl_if) {
            audio_codec_delete_ctrl_if(h_cleanup->ctrl_if);
            h_cleanup->ctrl_if = nullptr;
        }
        if (h_cleanup->gpio_if) {
            audio_codec_delete_gpio_if(h_cleanup->gpio_if);
            h_cleanup->gpio_if = nullptr;
        }
        if (h_cleanup->tx_handle) {
            i2s_channel_disable(h_cleanup->tx_handle);
            i2s_del_channel(h_cleanup->tx_handle);
            h_cleanup->tx_handle = nullptr;
        }
        if (h_cleanup->rx_handle) {
            i2s_channel_disable(h_cleanup->rx_handle);
            i2s_del_channel(h_cleanup->rx_handle);
            h_cleanup->rx_handle = nullptr;
        }
        delete h_cleanup;
        _handles.store(nullptr, std::memory_order_release);
    }
    xSemaphoreGive(_lifecycle_mutex);
    return -1;
}

void AudioDriver::deinit() {
    if (!_lifecycle_mutex) return;
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

    /* Atomically nullify codec mutex to prevent new ops */
    SemaphoreHandle_t cm = _codec_mutex.exchange(nullptr, std::memory_order_acq_rel);

    /* Wait for in-flight codec ops to complete */
    for (int i = 0; i < 100 && _codec_ops_in_flight.load(std::memory_order_acquire) > 0; i++) {
        xSemaphoreGive(_lifecycle_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);
    }

    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (h) {
        if (h->codec_dev) {
            esp_codec_dev_close(h->codec_dev);
            esp_codec_dev_delete(h->codec_dev);
            h->codec_dev = nullptr;
        }
        if (h->codec_if) {
            h->codec_if = nullptr;
        }
        if (h->data_if) {
            audio_codec_delete_data_if(h->data_if);
            h->data_if = nullptr;
        }
        if (h->ctrl_if) {
            audio_codec_delete_ctrl_if(h->ctrl_if);
            h->ctrl_if = nullptr;
        }
        if (h->gpio_if) {
            audio_codec_delete_gpio_if(h->gpio_if);
            h->gpio_if = nullptr;
        }
        if (h->tx_handle) {
            i2s_channel_disable(h->tx_handle);
            i2s_del_channel(h->tx_handle);
            h->tx_handle = nullptr;
        }
        if (h->rx_handle) {
            i2s_channel_disable(h->rx_handle);
            i2s_del_channel(h->rx_handle);
            h->rx_handle = nullptr;
        }
        delete h;
        _handles.store(nullptr, std::memory_order_release);
    }

    /* Delete codec mutex after all resources are freed */
    if (cm) {
        vSemaphoreDelete(cm);
    }

    xSemaphoreGive(_lifecycle_mutex);
    ESP_LOGI(TAG, "AudioDriver deinitialized");
}

/*============================================================================
 * Codec operations (thread-safe, via esp_codec_dev, P4-style mutex protocol)
 *============================================================================*/
esp_codec_dev_handle_t AudioDriver::codec_dev_handle() const {
    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    return h ? h->codec_dev : nullptr;
}

void AudioDriver::set_volume(int volume) {
    if (volume < VOLUME_MIN) volume = VOLUME_MIN;
    if (volume > VOLUME_MAX) volume = VOLUME_MAX;

    _volume.store(volume, std::memory_order_relaxed);

    /* Increment in-flight counter before checking mutex */
    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);

    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_acquire);
    if (!mtx) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    xSemaphoreTake(mtx, portMAX_DELAY);

    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (h && h->codec_dev) {
        esp_codec_dev_set_out_vol(h->codec_dev, volume);
    }

    xSemaphoreGive(mtx);
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

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
    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);

    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_acquire);
    if (!mtx) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    xSemaphoreTake(mtx, portMAX_DELAY);

    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (h && h->codec_dev) {
        esp_codec_dev_set_in_gain(h->codec_dev, (float)gain_db);
    }

    xSemaphoreGive(mtx);
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

    ESP_LOGI(TAG, "Mic gain set to %d dB", gain_db);
}

int AudioDriver::codec_write(const uint8_t *data, int size) {
    if (!data || size <= 0) return -1;

    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);

    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_acquire);
    if (!mtx) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return -1;
    }

    xSemaphoreTake(mtx, portMAX_DELAY);

    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    int ret = -1;
    if (h && h->codec_dev) {
        ret = esp_codec_dev_write(h->codec_dev, (void*)data, size);
    }

    xSemaphoreGive(mtx);
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

    return ret;
}

int AudioDriver::codec_read(uint8_t *data, int size) {
    if (!data || size <= 0) return -1;

    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);

    SemaphoreHandle_t mtx = _codec_mutex.load(std::memory_order_acquire);
    if (!mtx) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return -1;
    }

    xSemaphoreTake(mtx, portMAX_DELAY);

    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    int ret = -1;
    if (h && h->codec_dev) {
        ret = esp_codec_dev_read(h->codec_dev, data, size);
    }

    xSemaphoreGive(mtx);
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

    return ret;
}
