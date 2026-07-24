/*
 * AudioUlogRecorder — Continuous audio recording to ULog via uORB.
 *
 * Background task: I2S PCM read → AAC-ADTS encode → publish audio_frame uORB.
 * ULog writer polls audio_frame and writes to .ulg file on SD card.
 *
 * AAC config: 16kHz stereo 64kbps ADTS (matches existing .aac recording).
 * Frame size: 1024 samples × 2ch × 2B = 4096 bytes PCM input per AAC frame.
 * Output: ~1KB ADTS frame at ~15.6 fps → ~16 KB/s data rate.
 */

#include "audio_ulog_recorder.hpp"
#include "audio_driver.hpp"
#include "app_config.h"
#include "topics.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* AAC encoder */
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_aac_enc.h"

#include "uorb.h"

static const char *TAG = "audio_ulog";

/* ── PCM read buffer ── */
/* 480 samples × 2 channels × 2 bytes = 1920 bytes per I2S read.
 * Multiple reads accumulate into encoder input buffer. */
#define PCM_BUF_SAMPLES  480
#define PCM_BUF_BYTES    (PCM_BUF_SAMPLES * 2 * sizeof(int16_t))

/* ── Task config ── */
#define AUDIO_ULOG_TASK_STACK_SIZE  10240
#define AUDIO_ULOG_TASK_PRIORITY    2
#define AUDIO_ULOG_TASK_CORE        1

/* ── AAC encoder config ── */
#define AAC_SAMPLE_RATE     16000
#define AAC_CHANNEL         2
#define AAC_BITS_PER_SAMPLE 16
#define AAC_BITRATE         64000

/* ── External state from web_config_server (for mutual exclusion) ── */
/* Accessor functions declared in web_config_server.hpp — we check them
 * to avoid conflicting with .aac recording or playback (shared I2S). */
#include "web_config_server.hpp"

/* ── AudioUlogRecorder singleton ── */

AudioUlogRecorder& AudioUlogRecorder::instance()
{
    static AudioUlogRecorder s_instance;
    return s_instance;
}

AudioUlogRecorder::AudioUlogRecorder() = default;
AudioUlogRecorder::~AudioUlogRecorder() = default;

/* ── Background task ── */

void AudioUlogRecorder::_task_func(void *arg)
{
    AudioUlogRecorder *self = static_cast<AudioUlogRecorder *>(arg);
    ESP_LOGI(TAG, "Audio ULog task started");

    /* Allocate PCM read buffer from PSRAM */
    int16_t *pcm_buf = (int16_t *)heap_caps_calloc(1, PCM_BUF_BYTES,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        self->_running.store(false, std::memory_order_release);
        self->_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(NULL);
        return;
    }

    /* Open AAC encoder */
    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate = AAC_SAMPLE_RATE;
    aac_cfg.channel = AAC_CHANNEL;
    aac_cfg.bits_per_sample = AAC_BITS_PER_SAMPLE;
    aac_cfg.bitrate = AAC_BITRATE;
    aac_cfg.adts_used = true;

    esp_audio_enc_config_t enc_cfg = {};
    enc_cfg.type = ESP_AUDIO_TYPE_AAC;
    enc_cfg.cfg = &aac_cfg;
    enc_cfg.cfg_sz = sizeof(aac_cfg);

    esp_audio_enc_handle_t encoder = nullptr;
    if (esp_audio_enc_open(&enc_cfg, &encoder) != ESP_AUDIO_ERR_OK || !encoder) {
        ESP_LOGE(TAG, "AAC encoder open failed");
        heap_caps_free(pcm_buf);
        self->_running.store(false, std::memory_order_release);
        self->_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(NULL);
        return;
    }

    int enc_in_size = 0;
    int enc_out_size = 0;
    if (esp_audio_enc_get_frame_size(encoder, &enc_in_size, &enc_out_size) != ESP_AUDIO_ERR_OK
        || enc_in_size <= 0) {
        ESP_LOGE(TAG, "AAC get_frame_size failed");
        esp_audio_enc_close(encoder);
        heap_caps_free(pcm_buf);
        self->_running.store(false, std::memory_order_release);
        self->_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "AAC encoder: in=%d out=%d", enc_in_size, enc_out_size);

    /* Allocate encoder buffers from PSRAM */
    uint8_t *enc_in_buf = (uint8_t *)heap_caps_calloc(1, enc_in_size,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *enc_out_buf = (uint8_t *)heap_caps_calloc(1, enc_out_size,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc_in_buf || !enc_out_buf) {
        ESP_LOGE(TAG, "Encoder buffer alloc failed");
        if (enc_in_buf) heap_caps_free(enc_in_buf);
        if (enc_out_buf) heap_caps_free(enc_out_buf);
        esp_audio_enc_close(encoder);
        heap_caps_free(pcm_buf);
        self->_running.store(false, std::memory_order_release);
        self->_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(NULL);
        return;
    }

    int enc_in_count = 0;
    uint32_t frame_index = 0;
    orb_advert_t pub_handle = ORB_ADVERT_INVALID;

    /* Advertise audio_frame topic */
    pub_handle = orb_advertise(ORB_ID(audio_frame));
    if (pub_handle < 0) {
        ESP_LOGE(TAG, "Failed to advertise audio_frame topic");
        heap_caps_free(enc_in_buf);
        heap_caps_free(enc_out_buf);
        esp_audio_enc_close(encoder);
        heap_caps_free(pcm_buf);
        self->_running.store(false, std::memory_order_release);
        self->_task_handle.store(nullptr, std::memory_order_release);
        vTaskDelete(NULL);
        return;
    }

    /* ── Main recording loop ── */
    uint32_t diag_counter = 0;
    uint32_t diag_last = 0;
    uint32_t read_fail_count = 0;
    ESP_LOGI(TAG, "Entering main loop (enc_in_size=%d, pub_handle=%d)", enc_in_size, pub_handle);
    while (self->_running.load(std::memory_order_acquire)) {
        /* Read PCM from I2S */
        int n = AudioDriver::instance().codec_read((uint8_t *)pcm_buf, PCM_BUF_BYTES);
        if (n <= 0) {
            read_fail_count++;
            if (read_fail_count <= 5 || (read_fail_count % 1000) == 0) {
                ESP_LOGW(TAG, "codec_read failed (n=%d, fails=%u)", n, (unsigned)read_fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (read_fail_count > 0) {
            ESP_LOGI(TAG, "codec_read recovered after %u failures", (unsigned)read_fail_count);
            read_fail_count = 0;
        }

        /* Accumulate PCM into encoder input buffer */
        const uint8_t *src = (const uint8_t *)pcm_buf;
        int remaining = n;
        while (remaining > 0) {
            int space = enc_in_size - enc_in_count;
            int copy = (remaining < space) ? remaining : space;
            memcpy(enc_in_buf + enc_in_count, src, copy);
            enc_in_count += copy;
            src += copy;
            remaining -= copy;

            /* When encoder input buffer is full, encode one AAC frame */
            if (enc_in_count >= enc_in_size) {
                esp_audio_enc_in_frame_t in_frame = {
                    .buffer = enc_in_buf,
                    .len = (uint32_t)enc_in_size
                };
                esp_audio_enc_out_frame_t out_frame = {
                    .buffer = enc_out_buf,
                    .len = (uint32_t)enc_out_size
                };

                if (esp_audio_enc_process(encoder, &in_frame, &out_frame) == ESP_AUDIO_ERR_OK
                    && out_frame.encoded_bytes > 0) {
                    /* Publish audio_frame uORB topic */
                    audio_frame_s af = {};
                    af.timestamp = (uint64_t)esp_timer_get_time();
                    af.frame_index = frame_index++;
                    af.sample_rate = AAC_SAMPLE_RATE;
                    af.channel = AAC_CHANNEL;
                    af.bits_per_sample = AAC_BITS_PER_SAMPLE;
                    af.aac_size = (uint16_t)out_frame.encoded_bytes;

                    /* Copy AAC data (typically ~512B, fits in 1024B buffer) */
                    size_t copy_len = out_frame.encoded_bytes;
                    if (copy_len > sizeof(af.aac_data)) {
                        copy_len = sizeof(af.aac_data);
                        af.aac_size = (uint16_t)copy_len;
                        ESP_LOGE(TAG, "AAC frame too large (%u > %u), truncated — increase aac_data buffer!",
                                 (unsigned)out_frame.encoded_bytes, (unsigned)sizeof(af.aac_data));
                    }
                    memcpy(af.aac_data, out_frame.buffer, copy_len);

                    orb_publish(ORB_ID(audio_frame), pub_handle, &af);

                    self->_frame_count.fetch_add(1, std::memory_order_relaxed);
                    self->_bytes_published.fetch_add((uint32_t)copy_len,
                                                      std::memory_order_relaxed);

                    /* Periodic diagnostic log (every 500 frames ~ 32s) */
                    diag_counter++;
                    if (diag_counter - diag_last >= 500) {
                        diag_last = diag_counter;
                        ESP_LOGI(TAG, "Recording: frames=%u, bytes=%u",
                                 (unsigned)self->_frame_count.load(std::memory_order_relaxed),
                                 (unsigned)self->_bytes_published.load(std::memory_order_relaxed));
                    }
                }
                enc_in_count = 0;
            }
        }
    }

    /* ── Cleanup ── */
    ESP_LOGI(TAG, "Audio ULog task stopping (frames=%u, bytes=%u)",
             (unsigned)self->_frame_count.load(std::memory_order_relaxed),
             (unsigned)self->_bytes_published.load(std::memory_order_relaxed));

    if (pub_handle >= 0) {
        /* No orb_unadvertise in this uORB implementation —
         * the handle is lightweight and doesn't need cleanup */
    }

    esp_audio_enc_close(encoder);
    heap_caps_free(enc_in_buf);
    heap_caps_free(enc_out_buf);
    heap_caps_free(pcm_buf);

    self->_task_handle.store(nullptr, std::memory_order_release);
    vTaskDelete(NULL);
}

/* ── Public API ── */

esp_err_t AudioUlogRecorder::start()
{
    if (_running.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    /* Mutual exclusion: cannot run alongside .aac recording or playback */
    if (web_config_is_aac_recording()) {
        ESP_LOGW(TAG, "Cannot start: .aac recording is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (web_config_is_playing()) {
        ESP_LOGW(TAG, "Cannot start: playback is active");
        return ESP_ERR_INVALID_STATE;
    }

    if (!AudioDriver::instance().available()) {
        ESP_LOGE(TAG, "Audio driver not available");
        return ESP_ERR_INVALID_STATE;
    }

    /* Reset counters */
    _frame_count.store(0, std::memory_order_relaxed);
    _bytes_published.store(0, std::memory_order_relaxed);

    /* Allocate TCB from internal SRAM (small, reused) */
    if (!_task_tcb) {
        _task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!_task_tcb) {
            ESP_LOGE(TAG, "Failed to allocate TCB");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Allocate stack from PSRAM */
    _task_stack = (StackType_t *)heap_caps_malloc(
                      AUDIO_ULOG_TASK_STACK_SIZE * sizeof(StackType_t),
                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate task stack");
        return ESP_ERR_NO_MEM;
    }

    _running.store(true, std::memory_order_release);

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        _task_func,
        "ulog_audio",
        AUDIO_ULOG_TASK_STACK_SIZE,
        this,
        AUDIO_ULOG_TASK_PRIORITY,
        _task_stack,
        _task_tcb,
        AUDIO_ULOG_TASK_CORE);

    if (!h) {
        ESP_LOGE(TAG, "Failed to create task");
        _running.store(false, std::memory_order_release);
        heap_caps_free(_task_stack);
        _task_stack = nullptr;
        return ESP_FAIL;
    }

    _task_handle.store(h, std::memory_order_release);
    ESP_LOGI(TAG, "Audio ULog recorder started");
    return ESP_OK;
}

void AudioUlogRecorder::stop()
{
    if (!_running.load(std::memory_order_acquire)) {
        return;
    }

    ESP_LOGI(TAG, "Stopping audio ULog recorder...");
    _running.store(false, std::memory_order_release);

    /* Wait for task to self-delete (it releases resources) */
    for (int i = 0; i < 20 && _task_handle.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    TaskHandle_t h = _task_handle.load(std::memory_order_acquire);
    if (h) {
        ESP_LOGW(TAG, "Task did not exit in time, force deleting");
        vTaskDelete(h);
        _task_handle.store(nullptr, std::memory_order_release);
    }

    /* Free stack (TCB is reused) */
    if (_task_stack) {
        heap_caps_free(_task_stack);
        _task_stack = nullptr;
    }

    ESP_LOGI(TAG, "Audio ULog recorder stopped");
}
