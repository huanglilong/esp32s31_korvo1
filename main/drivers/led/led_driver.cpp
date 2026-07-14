/*
 * LedDriver — WS2812B RGB LED management via BSP (espressif/esp32_s31_korvo_1).
 *
 * Uses BSP's bsp_led_indicator_create() to initialize the addressable RGB LED.
 * Hardware: WS2812B-compatible RGB LED on GPIO37.
 * Singleton, thread-safe.
 */

#include "led_driver.hpp"
#include "esp_log.h"
#include "bsp/esp32_s31_korvo_1.h"

static const char *TAG = "LedDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
LedDriver& LedDriver::instance() {
    static LedDriver s;
    return s;
}

LedDriver::LedDriver() :
    _mutex(nullptr),
    _initialized(false),
    _state(false),
    _led(nullptr),
    _led_count(0)
{
    _mutex = xSemaphoreCreateMutex();
}

LedDriver::~LedDriver() {
    deinit();
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

/*============================================================================
 * Init / Deinit
 *============================================================================*/
int LedDriver::init() {
    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "LED already initialized");
        xSemaphoreGive(_mutex);
        return 0;
    }

    ESP_LOGI(TAG, "Initializing RGB LED via BSP (WS2812 on GPIO37)...");

    led_indicator_handle_t led_array[BSP_LED_COUNT];
    esp_err_t ret = bsp_led_indicator_create(led_array, &_led_count, BSP_LED_COUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP LED create failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(_mutex);
        return -1;
    }

    if (_led_count > 0) {
        _led = led_array[0];
    }

    _initialized.store(true, std::memory_order_relaxed);

    /* Turn LED off — call internal function to avoid deadlock on mutex */
    _state.store(false, std::memory_order_relaxed);
    if (_led) {
        led_indicator_start(_led, BSP_LED_OFF);
    }

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "RGB LED initialized (WS2812, GPIO37)");
    fflush(stdout);
    return 0;
}

void LedDriver::deinit() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_initialized.load(std::memory_order_relaxed)) {
        xSemaphoreGive(_mutex);
        return;
    }

    if (_led) {
        led_indicator_stop(_led, BSP_LED_OFF);
        _led = nullptr;
    }
    _initialized.store(false, std::memory_order_relaxed);

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "LED deinitialized");
}

/*============================================================================
 * LED control
 *============================================================================*/
void LedDriver::turn_on() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    _state.store(true, std::memory_order_relaxed);
    if (_led) {
        led_indicator_start(_led, BSP_LED_ON);
    }
    xSemaphoreGive(_mutex);
    ESP_LOGD(TAG, "LED ON");
}

void LedDriver::turn_off() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    _state.store(false, std::memory_order_relaxed);
    if (_led) {
        led_indicator_start(_led, BSP_LED_OFF);
    }
    xSemaphoreGive(_mutex);
    ESP_LOGD(TAG, "LED OFF");
}

void LedDriver::toggle() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    bool new_state = !_state.load(std::memory_order_relaxed);
    _state.store(new_state, std::memory_order_relaxed);
    if (_led) {
        led_indicator_start(_led, new_state ? BSP_LED_ON : BSP_LED_OFF);
    }
    xSemaphoreGive(_mutex);
}

void LedDriver::set_pattern(int pattern) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_led) {
        led_indicator_start(_led, pattern);
    }
    xSemaphoreGive(_mutex);
}
