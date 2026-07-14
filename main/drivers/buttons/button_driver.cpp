/*
 * ButtonDriver — board button management via BSP (espressif/esp32_s31_korvo_1).
 *
 * Uses BSP's bsp_iot_button_create() to initialize the 4-button ADC array.
 * Hardware: 4 buttons on ADC resistor ladder (GPIO42, ADC_CHANNEL_0).
 * Singleton, thread-safe.
 */

#include "button_driver.hpp"
#include "esp_log.h"
#include "bsp/esp32_s31_korvo_1.h"

static const char *TAG = "ButtonDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
ButtonDriver& ButtonDriver::instance() {
    static ButtonDriver s;
    return s;
}

ButtonDriver::ButtonDriver() :
    _mutex(nullptr),
    _initialized(false),
    _button_count(0)
{
    memset(_buttons, 0, sizeof(_buttons));
    _mutex = xSemaphoreCreateMutex();
}

ButtonDriver::~ButtonDriver() {
    deinit();
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

/*============================================================================
 * Init / Deinit
 *============================================================================*/
int ButtonDriver::init() {
    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "Buttons already initialized");
        xSemaphoreGive(_mutex);
        return 0;
    }

    ESP_LOGI(TAG, "Initializing buttons via BSP (ADC array on GPIO42)...");

    esp_err_t ret = bsp_iot_button_create(_buttons, &_button_count,
                                           BSP_BUTTON_COUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP button create failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(_mutex);
        return -1;
    }

    _initialized.store(true, std::memory_order_relaxed);
    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Buttons initialized: %d buttons (SET/MODE/VOLP/VOLM)", _button_count);
    return 0;
}

void ButtonDriver::deinit() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_initialized.load(std::memory_order_relaxed)) {
        xSemaphoreGive(_mutex);
        return;
    }

    for (int i = 0; i < _button_count; i++) {
        if (_buttons[i]) {
            iot_button_delete(_buttons[i]);
            _buttons[i] = nullptr;
        }
    }
    _button_count = 0;
    _initialized.store(false, std::memory_order_relaxed);

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Buttons deinitialized");
}

/*============================================================================
 * Button access
 *============================================================================*/
button_handle_t ButtonDriver::get_button(bsp_button_index_t index) const {
    if (index >= BSP_BUTTON_COUNT) return nullptr;

    if (!_mutex) return nullptr;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    button_handle_t btn = _buttons[index];
    xSemaphoreGive(_mutex);
    return btn;
}

int ButtonDriver::register_callback(bsp_button_index_t index,
                                     button_event_t event,
                                     void *cb, void *usr_data) {
    if (index >= BSP_BUTTON_COUNT) return -1;

    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    button_handle_t btn = _buttons[index];
    if (!btn) {
        xSemaphoreGive(_mutex);
        return -1;
    }

    esp_err_t ret = iot_button_register_cb(btn, event, nullptr,
                                            (button_cb_t)cb, usr_data);
    xSemaphoreGive(_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callback for button %d, event %d",
                 (int)index, (int)event);
        return -1;
    }
    return 0;
}
