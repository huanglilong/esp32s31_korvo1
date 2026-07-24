#pragma once

#include <atomic>
#include <cstring>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_playback.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_host.h"

/**
 * @brief Bluetooth Audio Driver for ESP32-S31-Korvo-1.
 *
 * Manages Classic Bluetooth A2DP Sink (receive audio from phone) and
 * AVRCP Target (remote control + metadata). Uses GMF pipeline to route
 * incoming audio through decoder -> resampler -> channel converter -> codec.
 *
 * Thread-safe singleton.
 */
class BtAudioDriver {
public:
    static BtAudioDriver& instance();

    esp_err_t init();
    void deinit();

    bool available() const { return _initialized.load(std::memory_order_acquire); }
    bool connected() const { return _connected.load(std::memory_order_acquire); }
    bool streaming() const { return _streaming.load(std::memory_order_acquire); }
    const char* device_address() const { return _device_addr; }
    const char* device_name() const { return _device_name; }
    const char* metadata_title() const { return _meta_title; }
    const char* metadata_artist() const { return _meta_artist; }
    const char* playback_status_str() const;

    esp_err_t start_discovery();
    esp_err_t stop_discovery();
    esp_err_t connect(const char *addr_str);
    esp_err_t disconnect();

    BtAudioDriver(const BtAudioDriver&) = delete;
    BtAudioDriver& operator=(const BtAudioDriver&) = delete;

private:
    BtAudioDriver();
    ~BtAudioDriver();

    static void _event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data);
    void _handle_event(esp_bt_audio_event_t event, void *event_data);
    void _on_stream_allocated(esp_bt_audio_stream_handle_t stream);
    void _on_stream_started(esp_bt_audio_stream_handle_t stream);
    void _on_stream_stopped(esp_bt_audio_stream_handle_t stream);
    void _on_stream_released(esp_bt_audio_stream_handle_t stream);
    esp_err_t _init_gmf_pool();
    void _deinit_gmf_pool();

    std::atomic<bool> _initialized;
    std::atomic<bool> _connected;
    std::atomic<bool> _streaming;
    char _device_addr[18];
    char _device_name[64];
    char _meta_title[256];
    char _meta_artist[256];
    std::atomic<esp_bt_audio_playback_status_t> _playback_status;
    void *_pool;       /* esp_gmf_pool_handle_t */
    void *_sink_pipe;  /* esp_gmf_pipeline_handle_t */
    void *_sink_task;  /* esp_gmf_task_handle_t */

    /* Cached device name from the last discovery event.
     * When a device connects, we look up its name by address from this cache.
     * Limitation: if the phone connects without prior discovery (e.g., auto-reconnect
     * from phone side), the name will show as "Unknown" until next discovery cycle. */
    static constexpr int MAX_CACHED_NAMES = 4;
    struct _cached_name_t { uint8_t addr[6]; char name[64]; };
    _cached_name_t _name_cache[MAX_CACHED_NAMES];
    int _name_cache_count{0};
    void _cache_name(const uint8_t addr[6], const char *name);
    void _lookup_name(const uint8_t addr[6]);  /* copies cached name → _device_name */
};
