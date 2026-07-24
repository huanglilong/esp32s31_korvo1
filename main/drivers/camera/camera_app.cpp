/*
 * CameraApp — Continuous camera ULog frame recording.
 *
 * Captures DVP camera frames (OV3660) via V4L2 (/dev/video0),
 * JPEG-encodes them using HW JPEG codec, and publishes camera_frame
 * uORB topics for ULog writer to record to .ulg files on SD card.
 *
 * PPA pipeline: 640x480 YUV422 YUYV → 180° rotate + downscale → 320x240 YUV422
 * JPEG pipeline: 320x240 YUV422 → HW JPEG (quality 45, YUV422 subsampling) → ~4-10KB per frame
 *
 * ESP32-S31 hardware notes:
 *   - PPA (Pixel Processing Accelerator) for HW scaling + rotation + byte-swap
 *   - HW JPEG codec for efficient JPEG encoding
 *   - OV3660 mounted upside-down → BSP_CAMERA_ROTATION=180
 *   - OV3660 outputs YUV422 YUYV (no byte-swap needed, unlike RGB565X)
 *   - Camera I2C (SCCB) shares GPIO0/1 with ES8389
 */

#include "camera_app.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_init.h"
#include "topics.h"
#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif
#include "driver/jpeg_encode.h"
#include <cstring>
#include <cinttypes>
#include <new>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

extern "C" {
#include "app_video.h"
}

#define TAG "CameraApp"
#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

/* ════════════════════════════════════════════════════════════════════
 *  app_video forward declarations (linked separately)
 * ════════════════════════════════════════════════════════════════════ */

int app_video_open(const char *dev, video_fmt_t init_fmt);
esp_err_t app_video_set_bufs(int video_fd, uint32_t fb_num, const void **fb);
esp_err_t app_video_register_frame_operation_cb(app_video_frame_operation_cb_t cb);
esp_err_t app_video_stream_task_start(int video_fd, int core_id);
esp_err_t app_video_stream_task_stop(int video_fd);
esp_err_t app_video_wait_video_stop(void);
uint32_t app_video_get_pixelformat(void);

/* ════════════════════════════════════════════════════════════════════
 *  Singleton
 * ════════════════════════════════════════════════════════════════════ */
CameraApp& CameraApp::instance() {
    static CameraApp s;
    return s;
}

CameraApp::CameraApp() :
    _mutex(nullptr),
    _initialized(false),
    _streaming(false),
    _task_should_stop(false),
    _task_handle(nullptr),
    _video_fd(-1),
    _frame_width(0),
    _frame_height(0),
    _rgb565x_input(false),
    _frame_count(0),
    _fps(0.0f),
    _fps_last_count(0),
    _fps_last_time(0)
{
    _mutex = xSemaphoreCreateMutex();
    _jpeg_mutex = xSemaphoreCreateMutex();
}

CameraApp::~CameraApp() {
    deinit();
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    if (_jpeg_mutex) {
        vSemaphoreDelete(_jpeg_mutex);
        _jpeg_mutex = nullptr;
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Init
 * ════════════════════════════════════════════════════════════════════ */
int CameraApp::init() {
    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "CameraApp already initialized");
        xSemaphoreGive(_mutex);
        return 0;
    }

    /* Step 1: Init camera hardware via BSP */
    esp_err_t ret = bsp_camera_start(nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP camera init failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(_mutex);
        return -1;
    }

    /* Step 2: Open V4L2 device */
    _video_fd = app_video_open(BSP_CAMERA_DEVICE, APP_VIDEO_FMT_DRIVER_DEFAULT);
    if (_video_fd < 0) {
        xSemaphoreGive(_mutex);
        return -1;
    }

    _frame_width = 0;
    _frame_height = 0;

    ESP_LOGI(TAG, "Video device opened: %s (%s)",
             BSP_CAMERA_DEVICE,
             app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565 ? "RGB565" :
             app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565X ? "RGB565X" :
             app_video_get_pixelformat() == V4L2_PIX_FMT_YUYV ? "YUV422-YUYV" :
             app_video_get_pixelformat() == V4L2_PIX_FMT_YUV422P ? "YUV422P" :
             app_video_get_pixelformat() == V4L2_PIX_FMT_YUV420 ? "YUV420" : "unknown");

    /* Detect camera output format.
     * YUV422 YUYV: no byte-swap needed (unlike RGB565X).
     * RGB565X: PPA byte_swap=1 corrects byte order. */
    _rgb565x_input = (app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565X);
    if (_rgb565x_input) {
        ESP_LOGI(TAG, "Camera outputs RGB565X (byte-swapped) — PPA byte_swap will correct");
    } else if (app_video_get_pixelformat() == V4L2_PIX_FMT_YUYV ||
               app_video_get_pixelformat() == V4L2_PIX_FMT_YUV422P ||
               app_video_get_pixelformat() == V4L2_PIX_FMT_YUV420) {
        ESP_LOGI(TAG, "Camera outputs YUV format — no byte_swap needed");
    }

    /* Step 3: Query frame dimensions from V4L2 */
    {
        struct v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) == 0) {
            _frame_width = fmt.fmt.pix.width;
            _frame_height = fmt.fmt.pix.height;
            ESP_LOGI(TAG, "Camera frame: %" PRIu32 "x%" PRIu32, _frame_width, _frame_height);
        } else {
            ESP_LOGW(TAG, "Failed to query frame format, using defaults");
        }
    }

    /* Step 4: Initialize HW JPEG encoder + PPA client for ULog recording */
    ESP_LOGI(TAG, "Step 4: Initializing HW JPEG encoder + PPA...");
    if (!_init_jpeg_encoder()) {
        ESP_LOGE(TAG, "HW JPEG encoder init failed — cannot record to ULog");
        close(_video_fd);
        _video_fd = -1;
        xSemaphoreGive(_mutex);
        return -1;
    }

    /* Step 5: Set up V4L2 buffers and register frame callback */
    ret = app_video_set_bufs(_video_fd, CAMERA_APP_NUM_V4L2_BUFS, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set V4L2 bufs");
        xSemaphoreGive(_mutex);
        return -1;
    }

    ret = app_video_register_frame_operation_cb(_frame_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame callback");
        xSemaphoreGive(_mutex);
        return -1;
    }

    _initialized.store(true, std::memory_order_relaxed);

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "CameraApp initialized (ULog recording only, no LCD display)");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 *  Start / Stop
 * ════════════════════════════════════════════════════════════════════ */
int CameraApp::start() {
    if (!_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGE(TAG, "CameraApp not initialized");
        return -1;
    }
    if (_streaming.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "CameraApp already streaming");
        return 0;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    _task_should_stop = false;

    /* Publish camera_state.running = true */
    if (_pub_state < 0) {
        _pub_state = orb_advertise(ORB_ID(camera_state));
    }
    if (_pub_state >= 0) {
        camera_state_s cs = {};
        cs.timestamp = esp_timer_get_time();
        cs.running = true;
        orb_publish(ORB_ID(camera_state), _pub_state, &cs);
    }

    /* Reset FPS counters */
    _frame_count.store(0, std::memory_order_relaxed);
    _ulog_frame_count.store(0, std::memory_order_relaxed);
    _fps.store(0.0f, std::memory_order_relaxed);
    _fps_last_count = 0;
    _fps_last_time = esp_timer_get_time();

    /* Start V4L2 stream task */
    esp_err_t ret = app_video_stream_task_start(_video_fd, CAMERA_APP_TASK_CORE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream task");
        xSemaphoreGive(_mutex);
        return -1;
    }

    _streaming.store(true, std::memory_order_relaxed);

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Camera ULog recording started");
    return 0;
}

int CameraApp::stop() {
    if (!_streaming.load(std::memory_order_relaxed)) {
        return 0;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    app_video_stream_task_stop(_video_fd);
    _task_should_stop = true;
    xSemaphoreGive(_mutex);

    /* Wait for stream task to exit via app_video */
    app_video_wait_video_stop();

    _streaming.store(false, std::memory_order_relaxed);

    /* Publish camera_state.running = false */
    if (_pub_state < 0) {
        _pub_state = orb_advertise(ORB_ID(camera_state));
    }
    if (_pub_state >= 0) {
        camera_state_s cs = {};
        cs.timestamp = esp_timer_get_time();
        cs.running = false;
        orb_publish(ORB_ID(camera_state), _pub_state, &cs);
    }

    ESP_LOGI(TAG, "Camera ULog recording stopped (frames=%u, ulog_frames=%u)",
             (unsigned)_frame_count.load(std::memory_order_relaxed),
             (unsigned)_ulog_frame_count.load(std::memory_order_relaxed));
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 *  Deinit
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::deinit() {
    if (!_initialized.load(std::memory_order_relaxed)) return;

    stop();

    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_video_fd >= 0) {
        close(_video_fd);
        _video_fd = -1;
    }

    /* Deinitialize HW JPEG encoder + PPA client */
    _deinit_jpeg_encoder();

    _initialized.store(false, std::memory_order_relaxed);

    /* Clear uORB publisher handles */
    _pub_state = ORB_ADVERT_INVALID;
    _pub_fps = ORB_ADVERT_INVALID;
    _frame_pub = ORB_ADVERT_INVALID;

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "CameraApp deinitialized");
}

/* ════════════════════════════════════════════════════════════════════
 *  Frame callback — called from app_video stream task
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::_frame_callback(uint8_t *camera_buf, uint8_t /*camera_buf_index*/,
                                uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                size_t /*camera_buf_len*/)
{
    CameraApp& self = CameraApp::instance();

    /* Update frame dimensions (may change on first frame) */
    if (self._frame_width == 0) {
        self._frame_width = camera_buf_hes;
        self._frame_height = camera_buf_ves;
    }

    /* FPS throttle: skip frames to reduce CPU load */
    {
        int64_t now_us = esp_timer_get_time();
        int64_t frame_interval_us = 1000000LL / CAMERA_APP_TARGET_FPS;
        if ((now_us - self._last_frame_us) < frame_interval_us) {
            return;
        }
        self._last_frame_us = now_us;
    }

    /* Publish camera_frame for ULog recording */
    self._publish_camera_frame(camera_buf, camera_buf_hes, camera_buf_ves);

    /* FPS tracking */
    uint32_t count = self._frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - self._fps_last_time;
    if (elapsed >= 1000000) {
        uint32_t frames_in_window = count - self._fps_last_count;
        float fps = (float)frames_in_window * 1000000.0f / (float)elapsed;
        self._fps.store(fps, std::memory_order_relaxed);

        /* Publish fps_stats via uORB */
        auto pub_fps = self._pub_fps.load(std::memory_order_relaxed);
        if (pub_fps < 0) {
            pub_fps = orb_advertise(ORB_ID(fps_stats));
            self._pub_fps.store(pub_fps, std::memory_order_relaxed);
        }
        if (pub_fps >= 0) {
            fps_stats_s fs = {};
            fs.timestamp = now;
            fs.frame_count = count;
            fs.fps = fps;
            fs.fps_total_bytes = 0;
            orb_publish(ORB_ID(fps_stats), pub_fps, &fs);
        }

        self._fps_last_count = count;
        self._fps_last_time = now;

        ESP_LOGD(TAG, "FPS: %.1f (frames: %" PRIu32 ")", fps, count);
        if ((count / 10) % 6 == 0) {
            ESP_LOGI(TAG, "Camera recording: %.1f fps, %" PRIu32 " frames, ulog: %u, heap: %" PRIu32 " KB",
                     fps, count,
                     (unsigned)self._ulog_frame_count.load(std::memory_order_relaxed),
                     (uint32_t)(esp_get_free_heap_size() / 1024));
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Stream task (not used directly — app_video manages its own task)
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::_stream_task(void * /*arg*/) {
    /* Not used — app_video creates its own streaming task internally. */
}

/* ════════════════════════════════════════════════════════════════════
 *  Publish camera_frame uORB topic for ULog recording
 *
 *  PPA pipeline (single SRM operation):
 *    Input:  640x480 YUV422 YUYV (from OV3660)
 *    PPA:    rotate 180° + downscale to 320x240 → YUV422
 *    Output: 320x240 YUV422 → HW JPEG encode
 *
 *  180° rotation: OV3660 is mounted upside-down on Korvo-1 board.
 *  BSP_CAMERA_ROTATION=180 confirms this.
 * YUV422 has no byte-swap issue (unlike RGB565X), so byte_swap=0.
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::_publish_camera_frame(uint8_t *rgb565_data, uint32_t width, uint32_t height)
{
    if (!rgb565_data || width == 0 || height == 0) return;

    uint32_t jpeg_size = 0;
    uint8_t *encode_input = rgb565_data;
    uint32_t encode_width = width;
    uint32_t encode_height = height;

    if (_jpeg_mutex) xSemaphoreTake(_jpeg_mutex, portMAX_DELAY);

    if (!_jpeg_encoder || !_jpeg_out_buf) {
        if (_jpeg_mutex) xSemaphoreGive(_jpeg_mutex);
        return;
    }

    /* PPA: downscale + rotate 180° in one SRM operation.
     * YUV422 path: no byte_swap needed (YUYV is byte-ordered, not byte-swapped like RGB565X).
     * RGB565X fallback: byte_swap=1 corrects byte order within each 16-bit pixel.
     * Without PPA, we fall back to direct JPEG encode (may have wrong rotation). */
    if (_ppa_client && _ulog_resize_buf) {
#if SOC_PPA_SUPPORTED
        bool is_yuv_input = (app_video_get_pixelformat() == V4L2_PIX_FMT_YUYV ||
                             app_video_get_pixelformat() == V4L2_PIX_FMT_YUV422P ||
                             app_video_get_pixelformat() == V4L2_PIX_FMT_YUV420);

        ppa_srm_oper_config_t srm_cfg = {};
        srm_cfg.in.buffer = rgb565_data;
        srm_cfg.in.pic_w = width;
        srm_cfg.in.pic_h = height;
        srm_cfg.in.block_w = width;
        srm_cfg.in.block_h = height;
        srm_cfg.in.block_offset_x = 0;
        srm_cfg.in.block_offset_y = 0;

        srm_cfg.out.buffer = _ulog_resize_buf;
        srm_cfg.out.buffer_size = CAMERA_APP_ULOG_WIDTH * CAMERA_APP_ULOG_HEIGHT * 2;
        srm_cfg.out.pic_w = CAMERA_APP_ULOG_WIDTH;
        srm_cfg.out.pic_h = CAMERA_APP_ULOG_HEIGHT;
        srm_cfg.out.block_offset_x = 0;
        srm_cfg.out.block_offset_y = 0;

        if (is_yuv_input) {
            /* YUV422 YUYV input → PPA rotate + downscale → YUV422 output */
            srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_YUYV;
            srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_YUV422_YUYV;
            srm_cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
            srm_cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
            srm_cfg.out.yuv_range = PPA_COLOR_RANGE_LIMIT;
            srm_cfg.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        } else {
            /* RGB565X input → PPA byte-swap + rotate + downscale → RGB565 output */
            srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        }

        /* 180° rotation: OV3660 is mounted upside-down (BSP_CAMERA_ROTATION=180) */
        srm_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;

        /* Downscale to ULog resolution */
        float scale_x = (float)CAMERA_APP_ULOG_WIDTH / (float)width;
        float scale_y = (float)CAMERA_APP_ULOG_HEIGHT / (float)height;
        srm_cfg.scale_x = scale_x;
        srm_cfg.scale_y = scale_y;

        /* byte_swap: only needed for RGB565X input (corrects byte order within 16-bit pixel).
         * YUV422 YUYV has no byte-swap issue — byte_swap is only available for ARGB8888/RGB565. */
        srm_cfg.rgb_swap = 0;
        srm_cfg.byte_swap = _rgb565x_input ? 1 : 0;

        srm_cfg.mode = PPA_TRANS_MODE_BLOCKING;

        esp_err_t ppa_ret = ppa_do_scale_rotate_mirror((ppa_client_handle_t)_ppa_client, &srm_cfg);
        if (ppa_ret == ESP_OK) {
            encode_input = _ulog_resize_buf;
            encode_width = CAMERA_APP_ULOG_WIDTH;
            encode_height = CAMERA_APP_ULOG_HEIGHT;
        } else {
            ESP_LOGW(TAG, "PPA SRM failed: %s", esp_err_to_name(ppa_ret));
        }
#endif
    }

    /* JPEG encode: determine input format based on camera output.
     * YUV422 path: PPA output is YUV422 YUYV → JPEG encoder takes YUV422 directly,
     *   no RGB→YUV color space conversion needed (better compression, faster encoding).
     * RGB565 path: PPA output is native RGB565 (byte order corrected by PPA).
     *   pixel_reverse=false because PPA already fixed the byte order.
     *   Only set pixel_reverse=true if PPA was NOT used AND input is RGB565X. */
    bool is_yuv_input = (app_video_get_pixelformat() == V4L2_PIX_FMT_YUYV ||
                         app_video_get_pixelformat() == V4L2_PIX_FMT_YUV422P ||
                         app_video_get_pixelformat() == V4L2_PIX_FMT_YUV420);

    jpeg_encode_cfg_t enc_cfg = {};
    enc_cfg.height = encode_height;
    enc_cfg.width = encode_width;
    if (is_yuv_input) {
        enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_YUV422;
        enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    } else {
        enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    }
    enc_cfg.image_quality = CAMERA_APP_JPEG_QUALITY;
    enc_cfg.pixel_reverse = false;

    /* Fallback: if PPA was not used and camera outputs RGB565X,
     * pixel_reverse tells JPEG encoder to swap bytes internally. */
    if (!is_yuv_input && encode_input == rgb565_data && _rgb565x_input) {
        enc_cfg.pixel_reverse = true;
    }

    uint32_t inbuf_size = is_yuv_input
        ? (encode_width * encode_height * 2)   /* YUV422: 2 bytes/pixel */
        : (encode_width * encode_height * 2);  /* RGB565: 2 bytes/pixel */
    esp_err_t ret = jpeg_encoder_process(_jpeg_encoder, &enc_cfg,
                                          encode_input, inbuf_size,
                                          _jpeg_out_buf, _jpeg_out_buf_size,
                                          &jpeg_size);

    if (_jpeg_mutex) xSemaphoreGive(_jpeg_mutex);

    if (ret != ESP_OK || jpeg_size == 0) {
        static uint32_t enc_fail_count = 0;
        enc_fail_count++;
        if (enc_fail_count <= 3 || (enc_fail_count % 100) == 0) {
            ESP_LOGW(TAG, "JPEG encode failed: %s (count=%u size=%u)",
                     esp_err_to_name(ret), (unsigned)enc_fail_count, (unsigned)jpeg_size);
        }
        return;
    }

    /* Log first successful encode */
    static bool first_encode_logged = false;
    if (!first_encode_logged) {
        first_encode_logged = true;
        ESP_LOGI(TAG, "First JPEG encode OK: %ux%u → %u bytes (PPA: %s, byte_swap: %d)",
                 (unsigned)encode_width, (unsigned)encode_height, (unsigned)jpeg_size,
                 _ppa_client ? "yes" : "no", _rgb565x_input ? 1 : 0);
    }

    /* JPEG size must fit in camera_frame_s.jpeg_data[] */
    if (jpeg_size > sizeof(((camera_frame_s *)0)->jpeg_data)) {
        ESP_LOGW(TAG, "JPEG frame too large for ULog (%u > %u), skipping",
                 (unsigned)jpeg_size, (unsigned)sizeof(((camera_frame_s *)0)->jpeg_data));
        return;
    }

    /* Lazy-init publisher (atomic CAS prevents double-advertise) */
    if (_frame_pub.load(std::memory_order_relaxed) < 0) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(camera_frame));
        orb_advert_t expected = ORB_ADVERT_INVALID;
        _frame_pub.compare_exchange_strong(expected, new_pub,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }
    if (_frame_pub.load(std::memory_order_relaxed) < 0) return;

    /* camera_frame_s is ~15KB — allocate from PSRAM to avoid stack overflow */
    camera_frame_s *frame = (camera_frame_s *)heap_caps_malloc(sizeof(camera_frame_s), MALLOC_CAP_SPIRAM);
    if (!frame) {
        ESP_LOGW(TAG, "Failed to allocate camera_frame_s, skipping publish");
        return;
    }

    memset(frame, 0, sizeof(*frame));
    frame->timestamp   = (uint64_t)esp_timer_get_time();
    frame->frame_index = _frame_count.load(std::memory_order_relaxed);
    frame->width       = (uint16_t)encode_width;
    frame->height      = (uint16_t)encode_height;
    frame->format      = 0;  /* 0 = JPEG */
    frame->jpeg_size   = (uint16_t)jpeg_size;
    memcpy(frame->jpeg_data, _jpeg_out_buf, jpeg_size);

    orb_publish(ORB_ID(camera_frame), _frame_pub, frame);
    heap_caps_free(frame);
    _ulog_frame_count.fetch_add(1, std::memory_order_relaxed);
}

/* ════════════════════════════════════════════════════════════════════
 *  HW JPEG encoder + PPA client init / deinit
 * ════════════════════════════════════════════════════════════════════ */
bool CameraApp::_init_jpeg_encoder(void)
{
    if (_jpeg_encoder) return true;  /* Already initialized */

    if (!_jpeg_mutex) {
        _jpeg_mutex = xSemaphoreCreateMutex();
        if (!_jpeg_mutex) {
            ESP_LOGE(TAG, "Failed to create JPEG mutex");
            return false;
        }
    }

    /* Create JPEG encoder engine */
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
        .flags = {
            .allow_pd = 0,
        },
    };

    esp_err_t ret = jpeg_new_encoder_engine(&eng_cfg, &_jpeg_encoder);
    if (ret != ESP_OK || !_jpeg_encoder) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder engine: %s", esp_err_to_name(ret));
        _jpeg_encoder = nullptr;
        return false;
    }

    /* Allocate JPEG output buffer from PSRAM */
    _jpeg_out_buf_size = CAMERA_APP_JPEG_OUT_BUF_SIZE;
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated_size = 0;
    _jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(_jpeg_out_buf_size, &mem_cfg, &allocated_size);
    if (!_jpeg_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer (%u bytes)", (unsigned)_jpeg_out_buf_size);
        jpeg_del_encoder_engine(_jpeg_encoder);
        _jpeg_encoder = nullptr;
        return false;
    }
    _jpeg_out_buf_size = (uint32_t)allocated_size;

    /* Allocate PPA downscale buffer for ULog (320x240 RGB565 = 153600 bytes) */
    _ulog_resize_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1,
                        CAMERA_APP_ULOG_WIDTH * CAMERA_APP_ULOG_HEIGHT * 2,
                        MALLOC_CAP_SPIRAM);
    if (!_ulog_resize_buf) {
        ESP_LOGE(TAG, "Failed to allocate ULog resize buffer");
        jpeg_del_encoder_engine(_jpeg_encoder);
        _jpeg_encoder = nullptr;
        heap_caps_free(_jpeg_out_buf);
        _jpeg_out_buf = nullptr;
        return false;
    }

    /* Register PPA SRM client for ULog downscale + rotate + byte-swap */
#if SOC_PPA_SUPPORTED
    {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
        };
        ret = ppa_register_client(&ppa_cfg, (ppa_client_handle_t *)&_ppa_client);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA SRM client failed (%s) — no HW rotate/downscale", esp_err_to_name(ret));
            _ppa_client = nullptr;
        }
    }
#endif

    ESP_LOGI(TAG, "HW JPEG encoder + PPA initialized (output buf: %u bytes, ULog: %dx%d, rotate: 180°, byte_swap: %d, yuv_input: %d)",
             (unsigned)_jpeg_out_buf_size, CAMERA_APP_ULOG_WIDTH, CAMERA_APP_ULOG_HEIGHT,
             _rgb565x_input ? 1 : 0,
             (app_video_get_pixelformat() == V4L2_PIX_FMT_YUYV ||
              app_video_get_pixelformat() == V4L2_PIX_FMT_YUV422P ||
              app_video_get_pixelformat() == V4L2_PIX_FMT_YUV420) ? 1 : 0);
    return true;
}

void CameraApp::_deinit_jpeg_encoder(void)
{
    if (_jpeg_mutex) xSemaphoreTake(_jpeg_mutex, portMAX_DELAY);

    if (_ppa_client) {
#if SOC_PPA_SUPPORTED
        ppa_unregister_client((ppa_client_handle_t)_ppa_client);
#endif
        _ppa_client = nullptr;
    }

    if (_ulog_resize_buf) {
        heap_caps_free(_ulog_resize_buf);
        _ulog_resize_buf = nullptr;
    }

    if (_jpeg_out_buf) {
        heap_caps_free(_jpeg_out_buf);
        _jpeg_out_buf = nullptr;
        _jpeg_out_buf_size = 0;
    }

    if (_jpeg_encoder) {
        jpeg_del_encoder_engine(_jpeg_encoder);
        _jpeg_encoder = nullptr;
    }

    if (_jpeg_mutex) xSemaphoreGive(_jpeg_mutex);
}
