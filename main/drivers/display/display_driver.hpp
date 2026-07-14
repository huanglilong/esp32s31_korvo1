#pragma once

#include <atomic>

/*
 * DisplayDriver — manages LCD and LVGL via BSP (espressif/esp32_s31_korvo_1).
 *
 * Uses BSP's bsp_display_start() for all-in-one LCD + LVGL + Touch init.
 * Hardware: ESP32-S3-LCD-EV-Board-SUB3 (4.3" RGB LCD, 800x480).
 *   - RGB interface: VSYNC=GPIO45, HSYNC=GPIO44, DE=GPIO43, PCLK=GPIO40
 *   - DISP=GPIO38, DATA[15:0]=GPIO[8:19,33:36]
 *   - Touch: GT1151 I2C touch controller (handled by BSP internally)
 *   - Backlight: NOT supported (no PWM backlight pin on this board)
 *
 * Thread-safe via bsp_display_lock/unlock.
 * The display is optional — init gracefully skips if LCD subboard not present.
 * Touch is initialized automatically by bsp_display_start().
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_types.h"

#if __has_include("lvgl.h")
#include "lvgl.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#endif

/* ── DisplayDriver class ───────────────────────────────────────── */

class DisplayDriver {
public:
    static DisplayDriver& instance();

    /**
     * @brief  Initialize display and LVGL via BSP. Thread-safe.
     *         Uses bsp_display_start() for all-in-one init.
     *         If LCD subboard is not connected, logs warning and returns success
     *         (display is optional).
     * @return 0 on success, -1 on failure
     */
    int init();

    /** Deinitialize display. Thread-safe. */
    void deinit();

    /** @return true if display is initialized */
    bool available() const { return _initialized.load(std::memory_order_relaxed); }

    /** @return display resolution width */
    int width() const { return _width; }

    /** @return display resolution height */
    int height() const { return _height; }

    /* LVGL integration */

    /** @return LVGL display handle, or nullptr */
    lv_display_t* lvgl_display() const { return _disp; }

    /**
     * @brief  Lock LVGL mutex for thread-safe access.
     *         Must be called before any LVGL API call from non-LVGL tasks.
     * @param  timeout_ms Max wait time in ms
     * @return true if lock acquired
     */
    bool lock(uint32_t timeout_ms = 100);

    /** Unlock LVGL mutex. Must be called after lock(). */
    void unlock();

    /** Rotate display.
     *  @param rotation LVGL rotation value */
    void rotate(lv_disp_rotation_t rotation);

    /* Delete copy/move */
    DisplayDriver(const DisplayDriver&) = delete;
    DisplayDriver& operator=(const DisplayDriver&) = delete;

private:
    DisplayDriver();
    ~DisplayDriver();

    SemaphoreHandle_t    _mutex;
    std::atomic<bool>    _initialized;
    lv_display_t        *_disp;
    int                  _width;
    int                  _height;
};
