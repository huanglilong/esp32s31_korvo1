#pragma once

/*
 * AudioUlogRecorder — Continuous audio recording to ULog via uORB.
 *
 * Reads PCM from I2S (ES8389 ADC), encodes to AAC-ADTS, and publishes
 * audio_frame uORB topics. The ULog writer automatically picks up these
 * topics and writes them to .ulg files on the SD card.
 *
 * Lifecycle: tied to ULog writer — start when ULog starts, stop when ULog stops.
 * Must NOT run simultaneously with .aac file recording or playback (shared I2S).
 *
 * AAC config: 16kHz stereo 64kbps ADTS (matches existing recording settings).
 */

#include <atomic>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AudioUlogRecorder {
public:
    static AudioUlogRecorder& instance();

    /**
     * @brief Start continuous audio recording to uORB.
     *
     * Creates a background task that reads I2S PCM → AAC encode → publish audio_frame.
     * No-op if already running.
     *
     * @return ESP_OK on success, ESP_ERR_INVALID_STATE if recording/playback is active
     */
    esp_err_t start();

    /**
     * @brief Stop continuous audio recording.
     *
     * Signals the task to stop, waits for clean exit, releases AAC encoder.
     */
    void stop();

    /** @return true if the recording task is currently running */
    bool running() const { return _running.load(std::memory_order_acquire); }

    /** @return total AAC frames published since last start */
    uint32_t frame_count() const { return _frame_count.load(std::memory_order_relaxed); }

    /** @return total AAC bytes published since last start */
    uint32_t bytes_published() const { return _bytes_published.load(std::memory_order_relaxed); }

    /* Delete copy/move */
    AudioUlogRecorder(const AudioUlogRecorder&) = delete;
    AudioUlogRecorder& operator=(const AudioUlogRecorder&) = delete;

private:
    AudioUlogRecorder();
    ~AudioUlogRecorder();

    /** Background task function */
    static void _task_func(void *arg);

    std::atomic<bool>      _running{false};
    std::atomic<uint32_t>  _frame_count{0};
    std::atomic<uint32_t>  _bytes_published{0};

    std::atomic<TaskHandle_t> _task_handle{nullptr};
    StackType_t           *_task_stack{nullptr};
    StaticTask_t          *_task_tcb{nullptr};
};
