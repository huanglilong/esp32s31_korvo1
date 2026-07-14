#pragma once

#include <atomic>

/*
 * LedDriver — manages WS2812B RGB LED via BSP (espressif/esp32_s31_korvo_1).
 *
 * Uses BSP's bsp_led_indicator_create() to initialize the addressable RGB LED.
 * Hardware: WS2812B-compatible RGB LED on GPIO37 (via LBSS138LT1G level shifter).
 *
 * LED patterns:
 *   BSP_LED_ON   — Solid on
 *   BSP_LED_OFF  — Off
 *   Custom blink lists can be added for status indication (WiFi, audio, etc.)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_indicator.h"

/* Number of LEDs on the board (single WS2812B) */
#define BSP_LED_COUNT 1

/* ── LedDriver class ───────────────────────────────────────────── */

class LedDriver {
public:
    static LedDriver& instance();

    /**
     * @brief  Initialize LED via BSP. Thread-safe.
     * @return 0 on success, -1 on failure
     */
    int init();

    /** Deinitialize LED. Thread-safe. */
    void deinit();

    /** @return true if LED is initialized */
    bool available() const { return _initialized.load(std::memory_order_relaxed); }

    /** Turn LED on (solid) */
    void turn_on();

    /** Turn LED off */
    void turn_off();

    /** Toggle LED state */
    void toggle();

    /**
     * @brief  Start a blink pattern by index.
     * @param  pattern Blink list index (BSP_LED_ON, BSP_LED_OFF, or custom)
     */
    void set_pattern(int pattern);

    /** @return LED handle, or nullptr if not initialized */
    led_indicator_handle_t get_handle() const { return _led; }

    /* Delete copy/move */
    LedDriver(const LedDriver&) = delete;
    LedDriver& operator=(const LedDriver&) = delete;

private:
    LedDriver();
    ~LedDriver();

    SemaphoreHandle_t           _mutex;
    std::atomic<bool>           _initialized;
    std::atomic<bool>           _state;
    led_indicator_handle_t      _led;
    int                         _led_count;
};
