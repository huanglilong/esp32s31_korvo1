#pragma once

/*
 * CameraApp — Continuous camera ULog frame recording.
 *
 * Captures DVP camera frames (OV3660) via V4L2 (/dev/video0),
 * JPEG-encodes them using HW JPEG codec, and publishes camera_frame
 * uORB topics for ULog writer to record to .ulg files on SD card.
 *
 * Hardware flow:
 *   OV3660 → DVP parallel → ESP32-S31 → V4L2 → PPA (downscale + rotate 180°)
 *   → HW JPEG encode → camera_frame uORB → ULogWriter → SD card
 *
 * PPA performs: 640x480 RGB565X → byte-swap + 180° rotate + downscale → 320x240 RGB565
 * HW JPEG encodes: 320x240 RGB565 → JPEG (quality 30, ~5-12KB per frame)
 *
 * No LCD display — CameraApp is ULog recording only.
 * Mutual exclusion: uses CameraDriver claim/release via uORB camera_state.
 * Thread-safe via FreeRTOS mutex.
 */

#include <atomic>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/jpeg_encode.h"

#include "uorb.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define CAMERA_APP_NUM_V4L2_BUFS   2     /* Double-buffering for V4L2 */
#define CAMERA_APP_TASK_STACK_SIZE (6 * 1024)
#define CAMERA_APP_TASK_PRIORITY   5
#define CAMERA_APP_TASK_CORE       0     /* Run on HP core (Core 0) */
#define CAMERA_APP_TARGET_FPS      5     /* Target frame rate */
#define CAMERA_APP_JPEG_QUALITY    30    /* JPEG quality for ULog recording (1-100) */
#define CAMERA_APP_JPEG_OUT_BUF_SIZE (10 * 1024)  /* 10KB JPEG output buffer (320x240 q30 ≤10KB) */
#define CAMERA_APP_ULOG_WIDTH      320   /* Downscaled width for ULog JPEG encoding */
#define CAMERA_APP_ULOG_HEIGHT     240   /* Downscaled height for ULog JPEG encoding */

/* ── CameraApp class ────────────────────────────────────────────── */

class CameraApp {
public:
    static CameraApp& instance();

    /**
     * @brief  Initialize camera hardware and prepare for ULog recording.
     *         - Initializes camera via BSP (bsp_camera_start)
     *         - Opens V4L2 device (/dev/video0)
     *         - Initializes HW JPEG encoder + PPA for downscale/rotate
     *
     * @return 0 on success, -1 on failure
     */
    int init();

    /**
     * @brief  Start camera streaming and ULog recording.
     *         Spawns a V4L2 stream task; frames are JPEG-encoded and
     *         published to camera_frame uORB for ULog recording.
     * @return 0 on success, -1 on failure
     */
    int start();

    /**
     * @brief  Stop camera streaming and ULog recording.
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

    /** @return total frames processed since start */
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

    /** Initialize HW JPEG encoder + PPA client */
    bool _init_jpeg_encoder(void);

    /** Deinitialize HW JPEG encoder + PPA client */
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

    /* Camera outputs RGB565X (byte-swapped) — flag for PPA byte_swap */
    bool                  _rgb565x_input;

    /* FPS tracking */
    std::atomic<uint32_t> _frame_count;
    std::atomic<uint32_t> _ulog_frame_count{0};
    std::atomic<float>    _fps;
    uint32_t              _fps_last_count;    /* Protected by _mutex */
    int64_t               _fps_last_time;     /* Protected by _mutex */
    int64_t               _last_frame_us{0};  /* Frame throttle timestamp */

    /* uORB */
    std::atomic<orb_advert_t> _pub_state{ORB_ADVERT_INVALID};
    std::atomic<orb_advert_t> _pub_fps{ORB_ADVERT_INVALID};
    std::atomic<orb_advert_t> _frame_pub{ORB_ADVERT_INVALID};

    /* HW JPEG encoder */
    jpeg_encoder_handle_t      _jpeg_encoder{nullptr};
    uint8_t                   *_jpeg_out_buf{nullptr};     /* JPEG output buffer (PSRAM) */
    uint32_t                   _jpeg_out_buf_size{0};
    SemaphoreHandle_t          _jpeg_mutex{nullptr};       /* Guards JPEG encoder access */

    /* PPA for ULog: downscale + rotate 180° + byte-swap RGB565X→RGB565 */
    uint8_t                   *_ulog_resize_buf{nullptr};   /* PPA output (320x240 RGB565, PSRAM) */
    void                      *_ppa_client{nullptr};        /* PPA SRM client handle */
};
