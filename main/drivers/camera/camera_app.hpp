#pragma once

/*
 * CameraApp — Camera streaming to LCD display + continuous ULog frame recording.
 *
 * Captures DVP camera frames (OV3660) via V4L2 (/dev/video0) and
 * displays them on the LCD using an LVGL canvas.  Uses the example's
 * app_video helper for V4L2 device management and BSP bsp_display_lock/unlock for LCD access.
 *
 * While streaming, continuously JPEG-encodes every frame and publishes
 * camera_frame uORB topics for ULog writer to record to .ulg files.
 * No separate enable/disable — recording is always active when streaming.
 *
 * Hardware flow:
 *   OV3660 → DVP parallel → ESP32-S31 → V4L2 → PPA (scale/rotate)
 *   → SPIRAM canvas → LVGL canvas → RGB LCD (800x480)
 *
 * ULog recording flow (parallel, same frame callback):
 *   V4L2 RGB565 frame → HW JPEG encode → camera_frame uORB → ULogWriter → SD card
 *
 * Mutual exclusion: uses CameraDriver claim/release via uORB camera_state.
 * Thread-safe via FreeRTOS mutex.
 *
 * ESP32-S31 has PPA (Pixel Processing Accelerator) for HW scaling,
 * and HW JPEG codec for efficient JPEG encoding.
 */

#include <atomic>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if __has_include("lvgl.h")
#include "lvgl.h"
#endif

#include "driver/jpeg_encode.h"

#include "uorb.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define CAMERA_APP_NUM_V4L2_BUFS   2     /* Double-buffering for V4L2 */
#define CAMERA_APP_TASK_STACK_SIZE (6 * 1024)
#define CAMERA_APP_TASK_PRIORITY   5
#define CAMERA_APP_TASK_CORE       0     /* Run on HP core (Core 0) */
#define CAMERA_APP_TARGET_FPS      5     /* Target frame rate: reduce CPU load (was ~11fps natural) */
#define CAMERA_APP_JPEG_QUALITY    30    /* JPEG quality for ULog recording (1-100) */
#define CAMERA_APP_JPEG_OUT_BUF_SIZE (16 * 1024)  /* 16KB JPEG output buffer */
#define CAMERA_APP_ULOG_WIDTH      320   /* Downscaled width for ULog JPEG encoding */
#define CAMERA_APP_ULOG_HEIGHT     240   /* Downscaled height for ULog JPEG encoding */

/* ── CameraApp class ────────────────────────────────────────────── */

class CameraApp {
public:
    static CameraApp& instance();

    /**
     * @brief  Initialize camera hardware and prepare for streaming.
     *         Must be called after DisplayDriver is initialized.
     *         - Initializes camera via BSP (bsp_camera_start)
     *         - Opens V4L2 device (/dev/video0)
     *         - Allocates SPIRAM canvas buffers
     *         - Creates LVGL canvas object
     *         - Initializes HW JPEG encoder for ULog recording
     *
     * @return 0 on success, -1 on failure
     */
    int init();

    /**
     * @brief  Start camera streaming to LCD.
     *         Spawns a FreeRTOS task that dequeues frames and updates the LCD.
     *         Camera frames are continuously JPEG-encoded and published to
     *         camera_frame uORB for ULog recording (no separate enable/disable).
     * @return 0 on success, -1 on failure
     */
    int start();

    /**
     * @brief  Stop camera streaming.
     *         Waits for the streaming task to cleanly exit.
     * @return 0 on success, -1 on failure
     */
    int stop();

    /** Deinitialize camera app. Stops streaming if active. */
    void deinit();

    /** @return true if camera app is initialized */
    bool available() const { return _initialized.load(std::memory_order_relaxed); }

    /** @return true if currently streaming */
    bool streaming() const { return _streaming.load(std::memory_order_relaxed); }

    /** @return current FPS (frames per second) */
    float fps() const {
        return _fps.load(std::memory_order_relaxed);
    }

    /** @return total frames rendered since start */
    uint32_t frameCount() const {
        return _frame_count.load(std::memory_order_relaxed);
    }

    /** @return total camera_frame uORB topics published since start */
    uint32_t ulogFrameCount() const {
        return _ulog_frame_count.load(std::memory_order_relaxed);
    }

    /** @return camera frame width in pixels */
    uint32_t frameWidth() const { return _frame_width; }

    /** @return camera frame height in pixels */
    uint32_t frameHeight() const { return _frame_height; }

    /* Delete copy/move */
    CameraApp(const CameraApp&) = delete;
    CameraApp& operator=(const CameraApp&) = delete;

private:
    CameraApp();
    ~CameraApp();

    /** V4L2 frame operation callback (called from video stream task) */
    static void _frame_callback(uint8_t *camera_buf, uint8_t camera_buf_index,
                                uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                size_t camera_buf_len);

    /** FreeRTOS task that runs the V4L2 stream loop */
    static void _stream_task(void *arg);

    /** Publish camera_frame uORB topic for ULog recording.
     *  Always called while streaming — no separate enable/disable. */
    void _publish_camera_frame(uint8_t *rgb565_data, uint32_t width, uint32_t height);

    /** Initialize HW JPEG encoder */
    bool _init_jpeg_encoder(void);

    /** Deinitialize HW JPEG encoder */
    void _deinit_jpeg_encoder(void);

    /* State */
    SemaphoreHandle_t     _mutex;
    std::atomic<bool>     _initialized;
    std::atomic<bool>     _streaming;
    bool                  _task_should_stop;   /* Protected by _mutex */
    TaskHandle_t          _task_handle;

    /* V4L2 */
    int                   _video_fd;

    /* Frame dimensions (from V4L2 VIDIOC_G_FMT) */
    uint32_t              _frame_width;
    uint32_t              _frame_height;

    /* LVGL */
    lv_obj_t             *_canvas;
    lv_color_format_t     _lvgl_format;  /* LVGL color format matching camera output */

    /* PPA hardware acceleration */
    void                 *_ppa_client;   /* ppa_client_handle_t for SRM operations */
    uint8_t              *_ppa_out_buf;  /* PPA output buffer (800x480 RGB565, SPIRAM) */

    /* Canvas buffers (SPIRAM) — one per V4L2 buffer for double-buffering */
    uint8_t              *_canvas_bufs[CAMERA_APP_NUM_V4L2_BUFS];

    /* FPS tracking */
    std::atomic<uint32_t> _frame_count;
    std::atomic<uint32_t> _ulog_frame_count{0};  /* camera_frame uORB publish count */
    std::atomic<float>    _fps;
    uint32_t              _fps_last_count;    /* Protected by _mutex */
    int64_t               _fps_last_time;     /* Protected by _mutex */
    int64_t               _last_frame_us{0};  /* Frame throttle timestamp */

    /* uORB */
    std::atomic<orb_advert_t> _pub_state{ORB_ADVERT_INVALID};
    std::atomic<orb_advert_t> _pub_fps{ORB_ADVERT_INVALID};
    std::atomic<orb_advert_t> _frame_pub{ORB_ADVERT_INVALID};  /* uORB publisher for camera_frame — atomic for lazy advertise CAS */

    /* HW JPEG encoder */
    jpeg_encoder_handle_t      _jpeg_encoder{nullptr};
    uint8_t                   *_jpeg_out_buf{nullptr};     /* JPEG output buffer (PSRAM) */
    uint32_t                   _jpeg_out_buf_size{0};
    SemaphoreHandle_t          _jpeg_mutex{nullptr};       /* Guards JPEG encoder access (shared between frame callback and deinit) */

    /* Downscaled buffer for ULog JPEG encoding (PPA → 320×240 RGB565) */
    uint8_t                   *_ulog_resize_buf{nullptr};   /* PPA output for ULog (320x240 RGB565, PSRAM) */
    void                      *_ulog_ppa_client{nullptr};   /* PPA SRM client for ULog downscale */
};
