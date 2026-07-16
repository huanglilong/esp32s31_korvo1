/*
 * AudioDriver — ES8389 audio codec driver for ESP32-S31-Korvo-1.
 *
 * Follows esp_board_manager Audio Codec device pattern:
 *   - init() takes dev_audio_codec_config_t + returns dev_audio_codec_handles_t*
 *   - Internally delegates to BSP (espressif/esp32_s31_korvo_1):
 *     bsp_audio_codec_speaker_init() for DAC + PA,
 *     bsp_audio_codec_microphone_init() for ADC
 *   - Config-driven: I2C/I2S pins and settings from config struct
 *   - Reference-counted init/deinit, thread-safe codec access
 *   - Bridges to ESP-GMF via esp_codec_dev_handle_t
 */

#include "audio_driver.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
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
 * Audio init via BSP — delegates to bsp_audio_codec_speaker/microphone_init
 *============================================================================*/
esp_err_t AudioDriver::_init_bsp_audio(const dev_audio_codec_config_t *cfg) {
    dev_audio_codec_handles_t *h = _handles.load(std::memory_order_acquire);
    if (!h || !cfg) return ESP_ERR_INVALID_STATE;

    esp_codec_dev_handle_t speaker_dev = nullptr;
    esp_codec_dev_handle_t mic_dev = nullptr;

    /* Initialize speaker (DAC) via BSP — same as BSP display_audio_photo example */
    if (cfg->dac_enabled) {
        speaker_dev = bsp_audio_codec_speaker_init();
        if (!speaker_dev) {
            ESP_LOGE(TAG, "BSP speaker init failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "BSP speaker initialized (ES8389 DAC + PA)");

        esp_codec_dev_sample_info_t spk_fs = {
            .bits_per_sample = 16,
            .channel = cfg->dac_max_channel,
            .sample_rate = cfg->i2s_cfg.sample_rate_hz,
        };
        if (esp_codec_dev_open(speaker_dev, &spk_fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Speaker open returned non-OK");
        }
        ESP_LOGI(TAG, "Speaker codec device opened (sample_rate=%lu, channels=%d)",
                 (unsigned long)spk_fs.sample_rate, spk_fs.channel);
    }

    /* Initialize microphone (ADC) via BSP — same as BSP display_audio_photo example */
    if (cfg->adc_enabled) {
        mic_dev = bsp_audio_codec_microphone_init();
        if (!mic_dev) {
            ESP_LOGE(TAG, "BSP microphone init failed");
            if (speaker_dev) {
                esp_codec_dev_close(speaker_dev);
                esp_codec_dev_delete(speaker_dev);
            }
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "BSP microphone initialized (ES8389 ADC)");

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = cfg->adc_max_channel,
            .sample_rate = cfg->i2s_cfg.sample_rate_hz,
        };
        if (esp_codec_dev_open(mic_dev, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Mic open returned non-OK");
        }
        ESP_LOGI(TAG, "Microphone codec device opened (sample_rate=%lu, channels=%d)",
                 (unsigned long)fs.sample_rate, fs.channel);
    }

    h->codec_dev = speaker_dev ? speaker_dev : mic_dev;
    h->mic_dev = mic_dev;

    return ESP_OK;
}

/*============================================================================
 * Init / Deinit (refcounted, config-driven, BSP-based)
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

    /* Initialize audio via BSP (I2C + I2S + ES8389 codec) */
    esp_err_t ret = _init_bsp_audio(cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP audio init failed");
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

    /* Set initial volume */
    set_volume(_volume.load(std::memory_order_relaxed));

    /* Set initial mic gain */
    if (cfg->adc_enabled && cfg->adc_init_gain > 0) {
        set_mic_gain(cfg->adc_init_gain);
    }

    /* Restore volume from NVS */
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
    ESP_LOGI(TAG, "AudioDriver initialized via BSP (ES8389, MCLK-less)");
    return 0;

cleanup:
    dev_audio_codec_handles_t *h_cleanup = _handles.load(std::memory_order_acquire);
    if (h_cleanup) {
        if (h_cleanup->codec_dev) {
            esp_codec_dev_close(h_cleanup->codec_dev);
            esp_codec_dev_delete(h_cleanup->codec_dev);
            h_cleanup->codec_dev = nullptr;
        }
        if (h_cleanup->mic_dev) {
            esp_codec_dev_close(h_cleanup->mic_dev);
            esp_codec_dev_delete(h_cleanup->mic_dev);
            h_cleanup->mic_dev = nullptr;
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
        if (h->mic_dev) {
            esp_codec_dev_close(h->mic_dev);
            esp_codec_dev_delete(h->mic_dev);
            h->mic_dev = nullptr;
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
    if (h && h->mic_dev) {
        esp_codec_dev_set_in_gain(h->mic_dev, (float)gain_db);
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
        int status = esp_codec_dev_write(h->codec_dev, (void*)data, size);
        /* esp_codec_dev_write returns ESP_CODEC_DEV_OK (0) on success,
         * NOT the byte count. */
        ret = (status == ESP_CODEC_DEV_OK) ? size : -1;
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
    if (h) {
        /* Use mic_dev for recording (ADC input), fall back to codec_dev */
        esp_codec_dev_handle_t dev = h->mic_dev ? h->mic_dev : h->codec_dev;
        if (dev) {
            int status = esp_codec_dev_read(dev, data, size);
            /* esp_codec_dev_read returns ESP_CODEC_DEV_OK (0) on success,
             * NOT the byte count. The full `size` bytes are filled. */
            ret = (status == ESP_CODEC_DEV_OK) ? size : -1;
        }
    }

    xSemaphoreGive(mtx);
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

    return ret;
}
