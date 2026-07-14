#pragma once

#include <atomic>

/*
 * ButtonDriver — manages board buttons via BSP (espressif/esp32_s31_korvo_1).
 *
 * Uses BSP's bsp_iot_button_create() to initialize the 4-button ADC array.
 * Button mapping (from BSP esp32_s31_korvo_1.json):
 *   SET  (BSP_BUTTON_SET)  — ADC ~1870 → GPIO42, function: Mode/Settings
 *   MODE (BSP_BUTTON_MODE) — ADC ~1340 → GPIO42, function: Play/Pause
 *   VOLP (BSP_BUTTON_VOLP) — ADC ~819  → GPIO42, function: Volume Up
 *   VOLM (BSP_BUTTON_VOLM) — ADC ~380  → GPIO42, function: Volume Down
 *
 * Hardware: 4 buttons on ADC resistor ladder (GPIO42, ADC_CHANNEL_0).
 *   - PLAY:  ~2.8V (13KΩ)
 *   - SET:   ~2.4V (6.8KΩ)
 *   - VOL-:  ~1.8V (3.3KΩ)
 *   - VOL+:  ~1.0V (1.3KΩ)
 *
 * Integrates with iot_button for debounce and long/short press detection.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iot_button.h"

/* Number of buttons on the board */
#define BSP_BUTTON_COUNT 4

/* Button index constants (match BSP enum values) */
typedef enum {
    BUTTON_SET  = 0,  /*!< SET/Mode button */
    BUTTON_MODE = 1,  /*!< MODE/Play button */
    BUTTON_VOLP = 2,  /*!< Volume Up button */
    BUTTON_VOLM = 3,  /*!< Volume Down button */
} bsp_button_index_t;

/* ── ButtonDriver class ────────────────────────────────────────── */

class ButtonDriver {
public:
    static ButtonDriver& instance();

    /**
     * @brief  Initialize buttons via BSP. Thread-safe.
     * @return 0 on success, -1 on failure
     */
    int init();

    /** Deinitialize buttons. Thread-safe. */
    void deinit();

    /** @return true if buttons are initialized */
    bool available() const { return _initialized.load(std::memory_order_relaxed); }

    /**
     * @brief  Get button handle by index.
     * @param  index Button index (BUTTON_SET/BUTTON_MODE/BUTTON_VOLP/BUTTON_VOLM)
     * @return Button handle, or nullptr if not initialized
     */
    button_handle_t get_button(bsp_button_index_t index) const;

    /**
     * @brief  Register callback for a button event.
     * @param  index   Button index
     * @param  event   Button event type (BUTTON_PRESS_DOWN, BUTTON_PRESS_UP, etc.)
     * @param  cb      Callback function
     * @param  usr_data User data passed to callback
     * @return 0 on success, -1 on failure
     */
    int register_callback(bsp_button_index_t index, button_event_t event,
                          void *cb, void *usr_data);

    /** @return array of all button handles (size = BSP_BUTTON_COUNT) */
    const button_handle_t* all_buttons() const { return _buttons; }

    /* Delete copy/move */
    ButtonDriver(const ButtonDriver&) = delete;
    ButtonDriver& operator=(const ButtonDriver&) = delete;

private:
    ButtonDriver();
    ~ButtonDriver();

    SemaphoreHandle_t           _mutex;
    std::atomic<bool>           _initialized;
    button_handle_t             _buttons[BSP_BUTTON_COUNT];
    int                         _button_count;
};
