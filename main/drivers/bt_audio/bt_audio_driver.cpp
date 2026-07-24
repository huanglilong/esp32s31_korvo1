/*
 * BtAudioDriver -- Bluetooth Audio driver for ESP32-S31-Korvo-1.
 *
 * Manages Classic Bluetooth A2DP Sink (receive audio from phone) and
 * AVRCP Target (remote control + metadata). Uses GMF pipeline to route
 * incoming audio through decoder -> resampler -> channel converter -> codec.
 *
 * Reference: esp-gmf/packages/esp_bt_audio/examples/bt_audio/
 */

#include "bt_audio_driver.hpp"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_playback.h"
#include "esp_bt_audio_vol.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_defs.h"

/* GMF headers */
#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"
#include "esp_gmf_element.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_simple_dec_default.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_types.h"
#include "drivers/audio/audio_driver.hpp"

static const char *TAG = "BtAudio";

/* -- Singleton --------------------------------------------- */
BtAudioDriver& BtAudioDriver::instance() {
    static BtAudioDriver s;
    return s;
}

BtAudioDriver::BtAudioDriver()
    : _initialized(false)
    , _connected(false)
    , _streaming(false)
    , _playback_status(ESP_BT_AUDIO_PLAYBACK_STATUS_STOPPED)
    , _pool(nullptr)
    , _sink_pipe(nullptr)
    , _sink_task(nullptr)
{
    _device_addr[0] = '\0';
    _device_name[0] = '\0';
    _meta_title[0] = '\0';
    _meta_artist[0] = '\0';
    memset(_name_cache, 0, sizeof(_name_cache));
    _name_cache_count = 0;
}

BtAudioDriver::~BtAudioDriver() { deinit(); }

/* -- Playback status string helper ------------------------- */
const char* BtAudioDriver::playback_status_str() const
{
    switch (_playback_status.load(std::memory_order_acquire)) {
        case ESP_BT_AUDIO_PLAYBACK_STATUS_PLAYING:  return "PLAYING";
        case ESP_BT_AUDIO_PLAYBACK_STATUS_PAUSED:   return "PAUSED";
        case ESP_BT_AUDIO_PLAYBACK_STATUS_STOPPED:  return "STOPPED";
        case ESP_BT_AUDIO_PLAYBACK_STATUS_FWD_SEEK: return "FWD_SEEK";
        case ESP_BT_AUDIO_PLAYBACK_STATUS_REV_SEEK: return "REV_SEEK";
        case ESP_BT_AUDIO_PLAYBACK_STATUS_ERROR:    return "ERROR";
        default: return "UNKNOWN";
    }
}

/* -- Name cache ------------------------------------------- */
void BtAudioDriver::_cache_name(const uint8_t addr[6], const char *name)
{
    if (!name || name[0] == '\0') return;

    /* Update existing entry if address already cached */
    for (int i = 0; i < _name_cache_count; i++) {
        if (memcmp(_name_cache[i].addr, addr, 6) == 0) {
            strlcpy(_name_cache[i].name, name, sizeof(_name_cache[i].name));
            return;
        }
    }

    /* Add new entry, evict oldest if full */
    int idx = (_name_cache_count < MAX_CACHED_NAMES) ? _name_cache_count++ : 0;
    if (_name_cache_count > MAX_CACHED_NAMES) {
        /* Shift entries left, dropping the oldest (index 0) */
        for (int i = 1; i < MAX_CACHED_NAMES; i++) {
            _name_cache[i - 1] = _name_cache[i];
        }
        idx = MAX_CACHED_NAMES - 1;
    }
    memcpy(_name_cache[idx].addr, addr, 6);
    strlcpy(_name_cache[idx].name, name, sizeof(_name_cache[idx].name));
}

void BtAudioDriver::_lookup_name(const uint8_t addr[6])
{
    for (int i = 0; i < _name_cache_count; i++) {
        if (memcmp(_name_cache[i].addr, addr, 6) == 0) {
            strlcpy(_device_name, _name_cache[i].name, sizeof(_device_name));
            return;
        }
    }
    /* Not found — leave _device_name as-is (could be "" or previous name) */
}

/* -- GMF pool registration helpers ------------------------ */
static esp_err_t _register_gmf_elements(esp_gmf_pool_handle_t pool)
{
    esp_err_t ret;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    esp_audio_enc_register_default();

    esp_audio_simple_dec_cfg_t dec_cfg = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
    esp_gmf_element_handle_t el = NULL;
    ret = esp_gmf_audio_dec_init(&dec_cfg, &el);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Init audio decoder failed"); return ret; }
    ret = esp_gmf_pool_register_element(pool, el, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register audio decoder failed"); return ret; }

    static float asrc_weight[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    esp_asrc_cfg_t asrc_cfg = DEFAULT_ESP_GMF_ASRC_CONFIG();
    asrc_cfg.weight = asrc_weight;
    asrc_cfg.weight_len = 4;
    el = NULL;
    ret = esp_gmf_asrc_init(&asrc_cfg, &el);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Init ASRC failed"); return ret; }
    ret = esp_gmf_pool_register_element(pool, el, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register ASRC failed"); return ret; }

    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    el = NULL;
    ret = esp_gmf_ch_cvt_init(&ch_cfg, &el);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Init ch cvt failed"); return ret; }
    ret = esp_gmf_pool_register_element(pool, el, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register ch cvt failed"); return ret; }

    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    el = NULL;
    ret = esp_gmf_bit_cvt_init(&bit_cfg, &el);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Init bit cvt failed"); return ret; }
    ret = esp_gmf_pool_register_element(pool, el, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register bit cvt failed"); return ret; }

    return ESP_OK;
}

static esp_err_t _register_gmf_io(esp_gmf_pool_handle_t pool)
{
    esp_err_t ret;
    esp_gmf_io_handle_t io = NULL;

    bt_io_cfg_t bt_rx_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_rx_cfg.dir = ESP_GMF_IO_DIR_READER;
    bt_rx_cfg.stream = NULL;
    ret = esp_gmf_io_bt_init(&bt_rx_cfg, &io);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Init BT reader failed"); return ret; }
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register BT reader failed"); return ret; }

    esp_codec_dev_handle_t codec_dev = AudioDriver::instance().codec_dev_handle();
    if (!codec_dev) {
        ESP_LOGE(TAG, "Audio codec device not available");
        return ESP_ERR_INVALID_STATE;
    }
    codec_dev_io_cfg_t codec_tx_cfg = ESP_GMF_IO_CODEC_DEV_CFG_DEFAULT();
    codec_tx_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    codec_tx_cfg.dev = codec_dev;
    io = NULL;
    ret = esp_gmf_io_codec_dev_init(&codec_tx_cfg, &io);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Init codec dev writer failed"); return ret; }
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register codec dev writer failed"); return ret; }

    return ESP_OK;
}

static esp_gmf_err_t _sink_pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (pkt && pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        ESP_LOGD(TAG, "[sink pipeline] state => %d", pkt->sub);
    }
    return ESP_GMF_ERR_OK;
}

esp_err_t BtAudioDriver::_init_gmf_pool()
{
    esp_gmf_pool_handle_t pool = NULL;
    esp_err_t ret = esp_gmf_pool_init(&pool);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GMF pool init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    _pool = pool;

    ret = _register_gmf_elements(pool);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register GMF elements failed"); return ret; }

    ret = _register_gmf_io(pool);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Register GMF IO failed"); return ret; }

    const char *el_names[] = {"aud_dec", "aud_asrc", "aud_ch_cvt", "aud_bit_cvt"};
    esp_gmf_pipeline_handle_t pipe = NULL;
    ret = esp_gmf_pool_new_pipeline(pool, "io_bt", el_names,
                                    sizeof(el_names) / sizeof(el_names[0]),
                                    "io_codec_dev", &pipe);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create sink pipeline failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_gmf_pipeline_set_event(pipe, _sink_pipe_event_cb, NULL);
    _sink_pipe = pipe;

    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = 5120;
    task_cfg.thread.prio = 15;
    task_cfg.thread.core = 0;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "bt2codec";
    esp_gmf_task_handle_t task = NULL;
    ret = esp_gmf_task_init(&task_cfg, &task);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create sink task failed: %s", esp_err_to_name(ret));
        return ret;
    }
    _sink_task = task;
    esp_gmf_pipeline_bind_task(pipe, task);

    ESP_LOGI(TAG, "GMF pool + sink pipeline initialized");
    return ESP_OK;
}

void BtAudioDriver::_deinit_gmf_pool()
{
    if (_sink_task) {
        esp_gmf_task_deinit((esp_gmf_task_handle_t)_sink_task);
        _sink_task = NULL;
    }
    if (_sink_pipe) {
        esp_gmf_pipeline_destroy((esp_gmf_pipeline_handle_t)_sink_pipe);
        _sink_pipe = NULL;
    }
    if (_pool) {
        esp_gmf_pool_deinit((esp_gmf_pool_handle_t)_pool);
        _pool = NULL;
    }
    ESP_LOGI(TAG, "GMF pool deinitialized");
}

/* -- Stream lifecycle management --------------------------- */
void BtAudioDriver::_on_stream_allocated(esp_bt_audio_stream_handle_t stream)
{
    if (!stream || !_sink_pipe) return;

    esp_bt_audio_stream_codec_info_t codec_info = {};
    if (esp_bt_audio_stream_get_codec_info(stream, &codec_info) != ESP_OK) {
        ESP_LOGE(TAG, "Get codec info failed");
        return;
    }
    ESP_LOGI(TAG, "Codec: type=%d, bits=%d, ch=%d, rate=%d",
             codec_info.codec_type, codec_info.bits,
             codec_info.channels, codec_info.sample_rate);

    /* Bind io_bt reader to the stream */
    esp_gmf_pipeline_handle_t pipe = (esp_gmf_pipeline_handle_t)_sink_pipe;
    esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_IN_INSTANCE(pipe), stream);

    /* Configure decoder */
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = (codec_info.codec_type == ESP_BT_AUDIO_STREAM_CODEC_SBC)
                       ? ESP_AUDIO_SIMPLE_DEC_TYPE_SBC : ESP_AUDIO_SIMPLE_DEC_TYPE_LC3;
    dec_cfg.dec_cfg = codec_info.codec_cfg;
    dec_cfg.cfg_size = codec_info.cfg_size;
    esp_gmf_audio_dec_reconfig(ESP_GMF_PIPELINE_GET_FIRST_ELEMENT(pipe), &dec_cfg);

    /* Configure ASRC destination: 16kHz stereo */
    esp_gmf_element_handle_t first_el = ESP_GMF_PIPELINE_GET_FIRST_ELEMENT(pipe);
    esp_gmf_element_handle_t asrc_el = NULL;
    esp_gmf_element_get_next_el(first_el, &asrc_el);
    if (asrc_el) {
        esp_gmf_asrc_set_dest_rate(asrc_el, 16000);
        esp_gmf_asrc_set_dest_ch(asrc_el, 2);
    }

    esp_gmf_pipeline_loading_jobs(pipe);
    ESP_LOGI(TAG, "Stream ALLOCATED -- pipeline prepared");
}

void BtAudioDriver::_on_stream_started(esp_bt_audio_stream_handle_t stream)
{
    (void)stream;
    if (!_sink_pipe) return;
    _streaming.store(true, std::memory_order_release);
    esp_gmf_pipeline_run((esp_gmf_pipeline_handle_t)_sink_pipe);
    ESP_LOGI(TAG, "Stream STARTED -- pipeline running");
}

void BtAudioDriver::_on_stream_stopped(esp_bt_audio_stream_handle_t stream)
{
    (void)stream;
    if (!_sink_pipe) return;
    _streaming.store(false, std::memory_order_release);
    esp_gmf_pipeline_stop((esp_gmf_pipeline_handle_t)_sink_pipe);
    esp_gmf_pipeline_reset((esp_gmf_pipeline_handle_t)_sink_pipe);
    ESP_LOGI(TAG, "Stream STOPPED -- pipeline stopped");
}

void BtAudioDriver::_on_stream_released(esp_bt_audio_stream_handle_t stream)
{
    (void)stream;
    _streaming.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "Stream RELEASED");
}

/* -- Event callback ---------------------------------------- */
void BtAudioDriver::_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    BtAudioDriver *self = static_cast<BtAudioDriver*>(user_data);
    if (self) self->_handle_event(event, event_data);
}

void BtAudioDriver::_handle_event(esp_bt_audio_event_t event, void *event_data)
{
    switch (event) {
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        auto *st = (esp_bt_audio_event_connection_st_t *)event_data;
        if (st->connected) {
            _connected.store(true, std::memory_order_release);
            snprintf(_device_addr, sizeof(_device_addr),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     st->addr[0], st->addr[1], st->addr[2],
                     st->addr[3], st->addr[4], st->addr[5]);
            /* Look up device name from discovery cache */
            _device_name[0] = '\0';
            _lookup_name(st->addr);
            if (_device_name[0] == '\0') {
                strlcpy(_device_name, "Unknown", sizeof(_device_name));
            }
            ESP_LOGI(TAG, "Connected: %s (%s)", _device_name, _device_addr);
            esp_bt_audio_classic_set_scan_mode(false, false);
        } else {
            _connected.store(false, std::memory_order_release);
            _device_addr[0] = '\0';
            _device_name[0] = '\0';
            ESP_LOGI(TAG, "Disconnected");
            esp_bt_audio_classic_set_scan_mode(true, true);
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        auto *st = (esp_bt_audio_event_stream_st_t *)event_data;
        switch (st->state) {
        case ESP_BT_AUDIO_STREAM_STATE_ALLOCATED:
            _on_stream_allocated(st->stream_handle); break;
        case ESP_BT_AUDIO_STREAM_STATE_STARTED:
            _on_stream_started(st->stream_handle); break;
        case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
            _on_stream_stopped(st->stream_handle); break;
        case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
            _on_stream_released(st->stream_handle); break;
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_MEDIA_CTRL_CMD: {
        auto *cmd = (esp_bt_audio_event_media_ctrl_t *)event_data;
        ESP_LOGI(TAG, "Media control: %d", cmd->cmd);
        break;
    }
    case ESP_BT_AUDIO_EVENT_PLAYBACK_STATUS_CHG: {
        auto *st = (esp_bt_audio_event_playback_st_t *)event_data;
        _playback_status.store((esp_bt_audio_playback_status_t)st->evt_param.play_status, std::memory_order_release);
        ESP_LOGI(TAG, "Playback status: %s", playback_status_str());
        break;
    }
    case ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA: {
        auto *meta = (esp_bt_audio_event_playback_metadata_t *)event_data;
        if (meta->type == ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE && meta->value && meta->length > 0) {
            size_t len = (size_t)meta->length < sizeof(_meta_title) - 1 ? (size_t)meta->length : sizeof(_meta_title) - 1;
            memcpy(_meta_title, meta->value, len);
            _meta_title[len] = '\0';
            ESP_LOGI(TAG, "Title: %s", _meta_title);
        } else if (meta->type == ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST && meta->value && meta->length > 0) {
            size_t len = (size_t)meta->length < sizeof(_meta_artist) - 1 ? (size_t)meta->length : sizeof(_meta_artist) - 1;
            memcpy(_meta_artist, meta->value, len);
            _meta_artist[len] = '\0';
            ESP_LOGI(TAG, "Artist: %s", _meta_artist);
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE: {
        auto *vol = (esp_bt_audio_event_vol_absolute_t *)event_data;
        int mapped = (vol->vol * 100) / 127;
        AudioDriver::instance().set_volume(mapped);
        break;
    }
    case ESP_BT_AUDIO_EVENT_VOL_RELATIVE: {
        ESP_LOGI(TAG, "Relative volume event");
        break;
    }
    case ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED: {
        auto *dev = (esp_bt_audio_event_device_discovered_t *)event_data;
        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 dev->addr[0], dev->addr[1], dev->addr[2],
                 dev->addr[3], dev->addr[4], dev->addr[5]);
        /* Cache the name so we can show it when the device connects */
        _cache_name(dev->addr, dev->name);
        ESP_LOGI(TAG, "Discovered: %s (%s)", dev->name, addr_str);
        break;
    }
    case ESP_BT_AUDIO_EVENT_DISCOVERY_STATE_CHG: {
        auto *st = (esp_bt_audio_event_discovery_st_t *)event_data;
        ESP_LOGI(TAG, "Discovery: %s", st->discovering ? "STARTED" : "STOPPED");
        break;
    }
    default:
        ESP_LOGD(TAG, "Unhandled event %d", event);
        break;
    }
}

/* -- Public API -------------------------------------------- */
esp_err_t BtAudioDriver::init()
{
    if (_initialized.load(std::memory_order_acquire)) return ESP_OK;

    ESP_LOGI(TAG, "Initializing Bluetooth Audio...");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BT controller init failed"); return ret; }
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BT controller enable failed"); return ret; }

    esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
    snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "ESP-S31-Korvo");

    ret = _init_gmf_pool();
    if (ret != ESP_OK) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }

    esp_bt_audio_config_t bt_config = {
        .host_config = &host_cfg,
        .event_cb = _event_cb,
        .event_user_ctx = this,
        .classic = {
            .roles = ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK
                   | ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_TG,
        },
    };

    ret = esp_bt_audio_init(&bt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_audio init failed: %s", esp_err_to_name(ret));
        _deinit_gmf_pool();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }

    esp_bt_audio_classic_set_scan_mode(true, true);

    esp_bt_audio_playback_reg_notifications(
        ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE |
        ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE |
        ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_END);

    esp_bt_audio_playback_request_metadata(
        ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE |
        ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST |
        ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM);

    _initialized.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Bluetooth Audio initialized (A2DP Sink + AVRCP Target)");
    return ESP_OK;
}

void BtAudioDriver::deinit()
{
    if (!_initialized.load(std::memory_order_acquire)) return;
    _initialized.store(false, std::memory_order_release);
    _connected.store(false, std::memory_order_release);
    _streaming.store(false, std::memory_order_release);

    esp_bt_audio_deinit();
    _deinit_gmf_pool();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "Bluetooth Audio deinitialized");
}

esp_err_t BtAudioDriver::start_discovery() { return esp_bt_audio_classic_discovery_start(); }
esp_err_t BtAudioDriver::stop_discovery()  { return esp_bt_audio_classic_discovery_stop(); }

esp_err_t BtAudioDriver::connect(const char *addr_str)
{
    esp_bd_addr_t addr;
    if (sscanf(addr_str, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, addr);
}

esp_err_t BtAudioDriver::disconnect()
{
    esp_bd_addr_t addr;
    if (sscanf(_device_addr, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]) == 6) {
        return esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, addr);
    }
    return ESP_ERR_INVALID_STATE;
}
