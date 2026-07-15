#pragma once

/*
 * CameraApp — Camera streaming to LCD display.
 *
 * Captures DVP camera frames (OV3660) via V4L2 (/dev/video0) and
 * displays them on the LCD using an LVGL canvas.  Uses esp_video for
 * V4L2 device management and BSP bsp_display_lock/unlock for LCD access.
 *
 * Hardware flow:
 *   OV3660 → DVP parallel → ESP32-S31 → V4L2 → PPA (scale/rotate)
 *   → SPIRAM canvas → LVGL canvas → RGB LCD (800x480)
 *
 * Mutual exclusion: uses CameraDriver claim/release via uORB camera_state.
 * Thread-safe via FreeRTOS mutex.
 *
 * ESP32-S31 has no PPA (Pixel Processing Accelerator), so scaling and
 * rotation are not available — frames are displayed at native resolution
 * centered on the LCD.
 */

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if __has_include("lvgl.h")
#include "lvgl.h"
#endif

#include "uorb.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define CAMERA_APP_NUM_V4L2_BUFS   2     /* Double-buffering for V4L2 */
#define CAMERA_APP_TASK_STACK_SIZE (6 * 1024)
#define CAMERA_APP_TASK_PRIORITY   5
#define CAMERA_APP_TASK_CORE       0     /* Run on HP core (Core 0) */

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
     *
     * @return 0 on success, -1 on failure
     */
    int init();

    /**
     * @brief  Start camera streaming to LCD.
     *         Spawns a FreeRTOS task that dequeues frames and updates the LCD.
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

    /* State */
    SemaphoreHandle_t     _mutex;
    std::atomic<bool>     _initialized;
    std::atomic<bool>     _streaming;
    bool                  _task_should_stop;   /* Protected by _mutex */
    SemaphoreHandle_t     _task_stop_sem;       /* Signalled when task exits */
    TaskHandle_t          _task_handle;

    /* V4L2 */
    int                   _video_fd;

    /* Frame dimensions (from V4L2 VIDIOC_G_FMT) */
    uint32_t              _frame_width;
    uint32_t              _frame_height;

    /* LVGL */
    lv_obj_t             *_canvas;
    lv_color_format_t     _lvgl_format;  /* LVGL color format matching camera output */

    /* Canvas buffers (SPIRAM) — one per V4L2 buffer for double-buffering */
    uint8_t              *_canvas_bufs[CAMERA_APP_NUM_V4L2_BUFS];

    /* FPS tracking */
    std::atomic<uint32_t> _frame_count;
    std::atomic<float>    _fps;
    uint32_t              _fps_last_count;    /* Protected by _mutex */
    int64_t               _fps_last_time;     /* Protected by _mutex */

    /* uORB */
    orb_advert_t          _pub_state;
    orb_advert_t          _pub_fps;
};
