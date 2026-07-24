/*
 * CameraApp — Camera streaming to LCD display + ULog frame recording.
 *
 * Captures DVP camera frames (OV3660) via V4L2 (/dev/video0) and
 * displays them on the LCD using an LVGL canvas.  Uses the example's
 * app_video helper for V4L2 device management.
 *
 * When recording is enabled, also JPEG-encodes frames and publishes
 * camera_frame uORB topics for ULog writer to record to .ulg files.
 *
 * ESP32-S31 hardware notes:
 *   - PPA (Pixel Processing Accelerator) is available for HW scaling + fill
 *   - No MIPI CSI, uses DVP 8-bit parallel
 *   - Camera sensor: OV3660, DVP 8-bit parallel, 20 MHz XCLK
 *   - Camera I2C (SCCB) shares GPIO0/1 with ES8389
 *   - HW JPEG codec available for efficient JPEG encoding
 */

#include "camera_app.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_init.h"
#include "drivers/display/display_driver.hpp"
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
 *  app_video components (copied from BSP example for self-contained build)
 *  These are the same functions found in the display_camera_video example.
 *  They use V4L2 ioctls to manage the camera stream.
 * ════════════════════════════════════════════════════════════════════ */

/* Forward declarations — app_video is linked separately */
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
    _canvas(nullptr),
    _lvgl_format(LV_COLOR_FORMAT_RGB565),
    _ppa_client(nullptr),
    _ppa_out_buf(nullptr),
    _frame_count(0),
    _fps(0.0f),
    _fps_last_count(0),
    _fps_last_time(0)
{
    _mutex = xSemaphoreCreateMutex();
    _canvas_bufs[0] = nullptr;
    _canvas_bufs[1] = nullptr;
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

    _frame_width = 0;   /* Will be set from V4L2 format query */
    _frame_height = 0;

    ESP_LOGI(TAG, "Video device opened: %s (%s)",
             BSP_CAMERA_DEVICE,
             app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565 ? "RGB565" :
             app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565X ? "RGB565X" : "unknown");

    /* Map V4L2 pixel format to LVGL color format.
     * Camera outputs RGB565X (big-endian/byte-swapped) → LV_COLOR_FORMAT_RGB565_SWAPPED.
     * If camera outputs regular RGB565 → LV_COLOR_FORMAT_RGB565. */
    _lvgl_format = LV_COLOR_FORMAT_RGB565;
    if (app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565X) {
        _lvgl_format = LV_COLOR_FORMAT_RGB565_SWAPPED;
        ESP_LOGI(TAG, "Using LV_COLOR_FORMAT_RGB565_SWAPPED (camera outputs RGB565X)");
    } else if (app_video_get_pixelformat() == V4L2_PIX_FMT_RGB565) {
        _lvgl_format = LV_COLOR_FORMAT_RGB565;
        ESP_LOGI(TAG, "Using LV_COLOR_FORMAT_RGB565");
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

    /* Step 4: Initialize PPA hardware accelerator for scaling (if available).
     * When PPA is enabled, _ppa_out_buf serves as both PPA output AND LVGL canvas buffer.
     * When PPA is unavailable, we allocate _canvas_bufs for CPU fallback. */
    const size_t cache_line_size = 64;
    uint32_t canvas_size = ALIGN_UP(
        BSP_LCD_H_RES * BSP_LCD_V_RES * 2,  /* 800x480 * 2 bytes (RGB565) */
        cache_line_size
    );

    uint8_t *canvas_init_buf = nullptr;  /* Buffer to use for LVGL canvas creation */

#if SOC_PPA_SUPPORTED
    {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
        };
        esp_err_t ppa_ret = ppa_register_client(&ppa_cfg, (ppa_client_handle_t *)&_ppa_client);
        if (ppa_ret == ESP_OK) {
            /* Allocate PPA output buffer — also serves as LVGL canvas buffer */
            _ppa_out_buf = (uint8_t *)heap_caps_aligned_calloc(
                cache_line_size, 1, canvas_size, MALLOC_CAP_SPIRAM);
            if (_ppa_out_buf) {
                ESP_LOGI(TAG, "PPA SRM client registered, output buffer: %" PRIu32 " bytes", canvas_size);
                canvas_init_buf = _ppa_out_buf;
            } else {
                ESP_LOGW(TAG, "PPA buffer alloc failed, falling back to CPU copy");
                ppa_unregister_client((ppa_client_handle_t)_ppa_client);
                _ppa_client = nullptr;
            }
        } else {
            ESP_LOGW(TAG, "PPA SRM client reg failed (%s), using CPU path",
                     esp_err_to_name(ppa_ret));
            _ppa_client = nullptr;
        }
    }
#endif

    /* Allocate CPU fallback canvas buffers only if PPA is not available */
    if (!canvas_init_buf) {
        for (int i = 0; i < CAMERA_APP_NUM_V4L2_BUFS; i++) {
            _canvas_bufs[i] = (uint8_t *)heap_caps_aligned_calloc(
                cache_line_size, 1, canvas_size, MALLOC_CAP_SPIRAM);
            if (!_canvas_bufs[i]) {
                ESP_LOGE(TAG, "Failed to allocate canvas buffer %d", i);
                for (int j = 0; j < i; j++) {
                    heap_caps_free(_canvas_bufs[j]);
                    _canvas_bufs[j] = nullptr;
                }
                close(_video_fd);
                _video_fd = -1;
                xSemaphoreGive(_mutex);
                return -1;
            }
        }
        canvas_init_buf = _canvas_bufs[0];
        ESP_LOGI(TAG, "Canvas buffers allocated: %" PRIu32 " bytes x %d (CPU path)",
                 canvas_size, CAMERA_APP_NUM_V4L2_BUFS);
    }

    /* Step 5: Create LVGL canvas for camera display */
    {
        DisplayDriver& disp = DisplayDriver::instance();
        if (!disp.available()) {
            ESP_LOGE(TAG, "Display not initialized");
            if (_ppa_out_buf) {
                heap_caps_free(_ppa_out_buf);
                _ppa_out_buf = nullptr;
            }
            for (int i = 0; i < CAMERA_APP_NUM_V4L2_BUFS; i++) {
                if (_canvas_bufs[i]) { heap_caps_free(_canvas_bufs[i]); _canvas_bufs[i] = nullptr; }
            }
            close(_video_fd);
            _video_fd = -1;
            xSemaphoreGive(_mutex);
            return -1;
        }

        disp.lock(2000);
        _canvas = lv_canvas_create(lv_scr_act());
        if (_canvas) {
            lv_canvas_set_buffer(_canvas, canvas_init_buf,
                                 BSP_LCD_H_RES, BSP_LCD_V_RES, _lvgl_format);
            lv_obj_center(_canvas);
            lv_canvas_fill_bg(_canvas, lv_color_hex(0x202020), LV_OPA_COVER);
        }
        disp.unlock();

        if (!_canvas) {
            ESP_LOGE(TAG, "Failed to create LVGL canvas");
            xSemaphoreGive(_mutex);
            return -1;
        }
        ESP_LOGI(TAG, "LVGL canvas created (%dx%d) %s",
                 BSP_LCD_H_RES, BSP_LCD_V_RES,
                 _ppa_client ? "(PPA HW scaled)" : "(CPU centering)");
    }

    /* Step 6: Initialize HW JPEG encoder for ULog frame recording */
    ESP_LOGI(TAG, "Step 6: Initializing HW JPEG encoder...");
    if (!_init_jpeg_encoder()) {
        ESP_LOGW(TAG, "HW JPEG encoder init failed — camera ULog recording disabled");
        /* Non-fatal: LCD preview still works, just no ULog recording */
    }

    /* Step 7: Set up V4L2 buffers and register frame callback */
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
    ESP_LOGI(TAG, "CameraApp initialized");
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
    ESP_LOGI(TAG, "Camera streaming started");
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

    /* Wait for stream task to exit via app_video (signals its own internal semaphore) */
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

    ESP_LOGI(TAG, "Camera streaming stopped");
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

    for (int i = 0; i < CAMERA_APP_NUM_V4L2_BUFS; i++) {
        if (_canvas_bufs[i]) {
            heap_caps_free(_canvas_bufs[i]);
            _canvas_bufs[i] = nullptr;
        }
    }

    if (_canvas) {
        DisplayDriver& disp = DisplayDriver::instance();
        if (disp.available()) {
            disp.lock(1000);
            lv_obj_delete(_canvas);
            disp.unlock();
        }
        _canvas = nullptr;
    }

    /* Free PPA resources */
#if SOC_PPA_SUPPORTED
    if (_ppa_client) {
        ppa_unregister_client((ppa_client_handle_t)_ppa_client);
        _ppa_client = nullptr;
    }
    if (_ppa_out_buf) {
        heap_caps_free(_ppa_out_buf);
        _ppa_out_buf = nullptr;
    }
#endif

    /* Free CPU fallback canvas buffers */
    for (int i = 0; i < CAMERA_APP_NUM_V4L2_BUFS; i++) {
        if (_canvas_bufs[i]) {
            heap_caps_free(_canvas_bufs[i]);
            _canvas_bufs[i] = nullptr;
        }
    }

    /* Deinitialize HW JPEG encoder */
    _deinit_jpeg_encoder();

    _initialized.store(false, std::memory_order_relaxed);

    /* Clear uORB publisher handles (topics were already unpublished by last publish) */
    _pub_state = ORB_ADVERT_INVALID;
    _pub_fps = ORB_ADVERT_INVALID;

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "CameraApp deinitialized");
}

/* ════════════════════════════════════════════════════════════════════
 *  Frame callback — called from app_video stream task
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::_frame_callback(uint8_t *camera_buf, uint8_t camera_buf_index,
                                uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                size_t /*camera_buf_len*/)
{
    CameraApp& self = CameraApp::instance();

    /* Update frame dimensions (may change on first frame) */
    if (self._frame_width == 0) {
        self._frame_width = camera_buf_hes;
        self._frame_height = camera_buf_ves;
    }

    /* FPS throttle: skip frames to reduce CPU load (Core 0).
     * At natural ~11fps, each frame costs PPA SRM + LVGL canvas refresh
     * (~15-20ms CPU). Limiting to CAMERA_APP_TARGET_FPS reduces PPA/LVGL
     * overhead proportionally without affecting preview quality. */
    {
        int64_t now_us = esp_timer_get_time();
        int64_t frame_interval_us = 1000000LL / CAMERA_APP_TARGET_FPS;  /* 5fps → 200ms */
        if ((now_us - self._last_frame_us) < frame_interval_us) {
            return;  /* Skip this frame */
        }
        self._last_frame_us = now_us;
    }

    uint32_t canvas_w = BSP_LCD_H_RES;
    uint32_t canvas_h = BSP_LCD_V_RES;

    /* Choose canvas buffer for double-buffering */
    uint8_t *canvas_buf = self._canvas_bufs[camera_buf_index];

#if SOC_PPA_SUPPORTED
    /* PPA hardware path: scale 640x480 → 800x480 fullscreen + fill background.
     * SRM handles the scale+position; we fill the output buffer with black
     * via memset (fast) before the SRM operation. */
    if (self._ppa_client) {
        /* Fast fill: memset entire output buffer to black (RGB565 0x0000).
         * 800x480x2 = 768KB, memset is highly optimized (~2ms). */
        memset(self._ppa_out_buf, 0, canvas_w * canvas_h * 2);

        /* PPA SRM: scale camera frame into output buffer centered with aspect ratio */
        float scale_x = (float)canvas_w / camera_buf_hes;
        float scale_y = (float)canvas_h / camera_buf_ves;

        /* Use smaller scale to maintain aspect ratio */
        float scale = (scale_x < scale_y) ? scale_x : scale_y;
        uint32_t out_w = (uint32_t)(camera_buf_hes * scale) & ~1; /* Align to even */
        uint32_t out_h = (uint32_t)(camera_buf_ves * scale);

        /* Center the scaled image in the 800x480 canvas */
        uint32_t ox = (canvas_w - out_w) / 2;
        uint32_t oy = (canvas_h - out_h) / 2;

        /* Apply BSP camera rotation (OV3660 is mounted upside-down: 180°).
         * Rotation affects scale calculation: 90°/270° swap width/height. */
        ppa_srm_rotation_angle_t rotation = PPA_SRM_ROTATION_ANGLE_0;
        switch (BSP_CAMERA_ROTATION) {
        case 0:   rotation = PPA_SRM_ROTATION_ANGLE_0;   break;
        case 90:  rotation = PPA_SRM_ROTATION_ANGLE_90;  break;
        case 180: rotation = PPA_SRM_ROTATION_ANGLE_180; break;
        case 270: rotation = PPA_SRM_ROTATION_ANGLE_270; break;
        default:  rotation = PPA_SRM_ROTATION_ANGLE_0;   break;
        }

        ppa_srm_oper_config_t srm_cfg = {};
        srm_cfg.in.buffer = camera_buf;
        srm_cfg.in.pic_w = camera_buf_hes;
        srm_cfg.in.pic_h = camera_buf_ves;
        srm_cfg.in.block_w = camera_buf_hes;
        srm_cfg.in.block_h = camera_buf_ves;
        srm_cfg.in.block_offset_x = 0;
        srm_cfg.in.block_offset_y = 0;
        srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        srm_cfg.out.buffer = self._ppa_out_buf;
        srm_cfg.out.buffer_size = canvas_w * canvas_h * 2;
        srm_cfg.out.pic_w = canvas_w;
        srm_cfg.out.pic_h = canvas_h;
        srm_cfg.out.block_offset_x = ox;
        srm_cfg.out.block_offset_y = oy;
        srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        srm_cfg.rotation_angle = rotation;
        srm_cfg.scale_x = scale;
        srm_cfg.scale_y = scale;
        srm_cfg.rgb_swap = 0;
        srm_cfg.byte_swap = 0;
        srm_cfg.mode = PPA_TRANS_MODE_BLOCKING;
        ppa_do_scale_rotate_mirror((ppa_client_handle_t)self._ppa_client, &srm_cfg);

        /* Use PPA output buffer as the canvas buffer */
        canvas_buf = self._ppa_out_buf;
    }
    else
#endif
    {
        /* CPU fallback path: center + letterbox with black borders */
        int32_t offset_x = ((int32_t)canvas_w - (int32_t)camera_buf_hes) / 2;
        int32_t offset_y = ((int32_t)canvas_h - (int32_t)camera_buf_ves) / 2;

        uint32_t copy_w = camera_buf_hes;
        uint32_t copy_h = camera_buf_ves;
        if (offset_x < 0) { copy_w = canvas_w; offset_x = 0; }
        if (offset_y < 0) { copy_h = canvas_h; offset_y = 0; }
        (void)copy_w;  /* Used for bounds checking above */

        uint16_t *dst = (uint16_t *)canvas_buf;
        size_t total_pixels = canvas_w * canvas_h;
        for (size_t i = 0; i < total_pixels; i++) {
            dst[i] = 0x0000;
        }

        uint16_t *src = (uint16_t *)camera_buf;
        for (uint32_t y = 0; y < copy_h; y++) {
            uint32_t dst_y = (uint32_t)offset_y + y;
            uint32_t dst_x = (uint32_t)offset_x;
            uint16_t *dst_line = &dst[dst_y * canvas_w + dst_x];
            uint16_t *src_line = &src[y * camera_buf_hes];
            uint32_t line_copy = (camera_buf_hes > canvas_w) ? canvas_w : camera_buf_hes;
            if (dst_x + line_copy > canvas_w) line_copy = canvas_w - dst_x;
            for (uint32_t x = 0; x < line_copy; x++) {
                dst_line[x] = src_line[x];
            }
        }
    }

    /* Update LVGL canvas */
    DisplayDriver& disp = DisplayDriver::instance();
    if (disp.available()) {
        disp.lock(0);
        if (self._canvas) {
            lv_canvas_set_buffer(self._canvas, canvas_buf, canvas_w, canvas_h,
                                 self._lvgl_format);
            lv_obj_center(self._canvas);
            lv_obj_invalidate(self._canvas);
        }
        disp.unlock();
    }

    /* FPS tracking */
    uint32_t count = self._frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - self._fps_last_time;
    if (elapsed >= 1000000) { /* Update FPS every 1 second */
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
            fs.fps_total_bytes = 0; /* Not tracked per-frame in LCD preview path */
            orb_publish(ORB_ID(fps_stats), pub_fps, &fs);
        }

        self._fps_last_count = count;
        self._fps_last_time = now;

        ESP_LOGD(TAG, "FPS: %.1f (frames: %" PRIu32 ")", fps, count);
        if ((count / 10) % 6 == 0) { /* Log every ~60 frames (every ~5s at 11fps) */
            ESP_LOGI(TAG, "Camera streaming: %.1f fps, %" PRIu32 " frames, heap: %" PRIu32 " KB",
                     fps, count, (uint32_t)(esp_get_free_heap_size() / 1024));
        }
    }

    /* Publish camera_frame for ULog recording */
    self._publish_camera_frame(camera_buf, camera_buf_hes, camera_buf_ves);
}

/* ════════════════════════════════════════════════════════════════════
 *  Stream task (not used directly — app_video manages its own task)
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::_stream_task(void * /*arg*/) {
    /* Not used — app_video creates its own streaming task internally.
     * We use app_video_stream_task_start/stop for lifecycle management. */
}

/* ════════════════════════════════════════════════════════════════════
 *  Publish camera_frame uORB topic for ULog recording
 * ════════════════════════════════════════════════════════════════════ */
void CameraApp::_publish_camera_frame(uint8_t *rgb565_data, uint32_t width, uint32_t height)
{
    if (!rgb565_data || width == 0 || height == 0) return;

    /* JPEG-encode the RGB565 frame using HW encoder.
     * For ULog recording, we downscale to 320x240 first via PPA
     * to keep JPEG size < 15KB (640x480 produces >32KB even at quality 10). */
    uint32_t jpeg_size = 0;
    uint8_t *encode_input = rgb565_data;
    uint32_t encode_width = width;
    uint32_t encode_height = height;

    if (_jpeg_mutex) xSemaphoreTake(_jpeg_mutex, portMAX_DELAY);

    if (!_jpeg_encoder || !_jpeg_out_buf) {
        if (_jpeg_mutex) xSemaphoreGive(_jpeg_mutex);
        return;
    }

    /* Downscale via PPA if available, otherwise use original (may fail for large frames) */
    if (_ulog_ppa_client && _ulog_resize_buf && (width > CAMERA_APP_ULOG_WIDTH || height > CAMERA_APP_ULOG_HEIGHT)) {
#if SOC_PPA_SUPPORTED
        ppa_srm_oper_config_t srm_cfg = {};
        srm_cfg.in.buffer = rgb565_data;
        srm_cfg.in.pic_w = width;
        srm_cfg.in.pic_h = height;
        srm_cfg.in.block_w = width;
        srm_cfg.in.block_h = height;
        srm_cfg.in.block_offset_x = 0;
        srm_cfg.in.block_offset_y = 0;
        srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        srm_cfg.out.buffer = _ulog_resize_buf;
        srm_cfg.out.buffer_size = CAMERA_APP_ULOG_WIDTH * CAMERA_APP_ULOG_HEIGHT * 2;
        srm_cfg.out.pic_w = CAMERA_APP_ULOG_WIDTH;
        srm_cfg.out.pic_h = CAMERA_APP_ULOG_HEIGHT;
        srm_cfg.out.block_offset_x = 0;
        srm_cfg.out.block_offset_y = 0;
        srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        srm_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        float scale_x = (float)CAMERA_APP_ULOG_WIDTH / (float)width;
        float scale_y = (float)CAMERA_APP_ULOG_HEIGHT / (float)height;
        srm_cfg.scale_x = scale_x;
        srm_cfg.scale_y = scale_y;
        srm_cfg.rgb_swap = 0;
        srm_cfg.byte_swap = 0;
        srm_cfg.mode = PPA_TRANS_MODE_BLOCKING;

        /* Handle RGB565X (byte-swapped) input from camera */
        if (_lvgl_format == LV_COLOR_FORMAT_RGB565_SWAPPED) {
            srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            srm_cfg.rgb_swap = 1;  /* PPA will swap bytes */
        }

        esp_err_t ppa_ret = ppa_do_scale_rotate_mirror((ppa_client_handle_t)_ulog_ppa_client, &srm_cfg);
        if (ppa_ret == ESP_OK) {
            encode_input = _ulog_resize_buf;
            encode_width = CAMERA_APP_ULOG_WIDTH;
            encode_height = CAMERA_APP_ULOG_HEIGHT;
        } else {
            ESP_LOGW(TAG, "PPA downscale for ULog failed: %s", esp_err_to_name(ppa_ret));
        }
#endif
    }

    jpeg_encode_cfg_t enc_cfg = {
        .height = encode_height,
        .width = encode_width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = CAMERA_APP_JPEG_QUALITY,
        .pixel_reverse = false,
    };

    /* PPA output is native RGB565 (byte-swapped if rgb_swap was used).
     * If no PPA used and camera outputs RGB565X, set pixel_reverse. */
    if (encode_input == rgb565_data && _lvgl_format == LV_COLOR_FORMAT_RGB565_SWAPPED) {
        enc_cfg.pixel_reverse = true;
    }

    uint32_t inbuf_size = encode_width * encode_height * 2;  /* RGB565: 2 bytes per pixel */
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
        ESP_LOGI(TAG, "First JPEG encode OK: %ux%u → %u bytes",
                 (unsigned)encode_width, (unsigned)encode_height, (unsigned)jpeg_size);
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

    /* camera_frame_s is ~15KB — too large for app_video task stack.
     * Allocate from PSRAM heap to avoid stack overflow. */
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
 *  HW JPEG encoder init / deinit
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

    /* Register PPA SRM client for ULog downscale */
#if SOC_PPA_SUPPORTED
    {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
        };
        ret = ppa_register_client(&ppa_cfg, (ppa_client_handle_t *)&_ulog_ppa_client);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA SRM client for ULog failed (%s), will use CPU fallback", esp_err_to_name(ret));
            _ulog_ppa_client = nullptr;
        }
    }
#endif

    ESP_LOGI(TAG, "HW JPEG encoder initialized (output buf: %u bytes, ULog resize: %dx%d)",
             (unsigned)_jpeg_out_buf_size, CAMERA_APP_ULOG_WIDTH, CAMERA_APP_ULOG_HEIGHT);
    return true;
}

void CameraApp::_deinit_jpeg_encoder(void)
{
    if (_jpeg_mutex) xSemaphoreTake(_jpeg_mutex, portMAX_DELAY);

    if (_ulog_ppa_client) {
        ppa_unregister_client((ppa_client_handle_t)_ulog_ppa_client);
        _ulog_ppa_client = nullptr;
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
