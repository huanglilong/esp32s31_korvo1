/*
 * DisplayDriver — LCD + LVGL display management via BSP (espressif/esp32_s31_korvo_1).
 *
 * Uses BSP's bsp_display_start() for all-in-one LCD + LVGL + Touch init.
 * Hardware: ESP32-S3-LCD-EV-Board-SUB3 (4.3" RGB LCD, 800x480).
 * Singleton, thread-safe.
 * Display is optional — gracefully skips if LCD subboard not connected.
 */

#include "display_driver.hpp"
#include "esp_log.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "bsp/display.h"

#if __has_include("lvgl.h")
#include "lvgl.h"
#include "esp_lvgl_port.h"
#endif

static const char *TAG = "DisplayDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
DisplayDriver& DisplayDriver::instance() {
    static DisplayDriver s;
    return s;
}

DisplayDriver::DisplayDriver() :
    _mutex(nullptr),
    _initialized(false),
    _disp(nullptr),
    _width(800),
    _height(480)
{
    _mutex = xSemaphoreCreateMutex();
}

DisplayDriver::~DisplayDriver() {
    deinit();
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

/*============================================================================
 * Init / Deinit
 *============================================================================*/
int DisplayDriver::init() {
    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "Display already initialized");
        xSemaphoreGive(_mutex);
        return 0;
    }

    ESP_LOGI(TAG, "Initializing display via BSP (RGB LCD 800x480 + GT1151 touch)...");

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    /* All-in-one: LCD + LVGL + Touch via BSP */
    _disp = bsp_display_start();
    if (!_disp) {
        ESP_LOGW(TAG, "Display init failed — LCD subboard may not be connected");
        xSemaphoreGive(_mutex);
        return -1;
    }

    _width = BSP_LCD_H_RES;
    _height = BSP_LCD_V_RES;
#else
    ESP_LOGW(TAG, "LVGL not configured (BSP_CONFIG_NO_GRAPHIC_LIB=1)");
    xSemaphoreGive(_mutex);
    return -1;
#endif

    _initialized.store(true, std::memory_order_relaxed);

    /* Create a simple splash screen to verify display works */
    if (_disp) {
        DisplayDriver::lock(1000);
        lv_obj_t *scr = lv_display_get_screen_active(_disp);
        if (scr) {
            lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);

            lv_obj_t *label = lv_label_create(scr);
            lv_label_set_text(label, "ESP32-S31\nKorvo-1");
            lv_obj_set_style_text_color(label, lv_color_hex(0x00d4aa), LV_PART_MAIN);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_center(label);

            /* Status subtitle */
            lv_obj_t *status = lv_label_create(scr);
            lv_label_set_text(status, "Boot OK — WiFi AP Mode");
            lv_obj_set_style_text_color(status, lv_color_hex(0x888888), LV_PART_MAIN);
            lv_obj_set_style_text_font(status, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -20);

            /* IP address */
            lv_obj_t *ip = lv_label_create(scr);
            lv_label_set_text(ip, "http://192.168.4.1:8080");
            lv_obj_set_style_text_color(ip, lv_color_hex(0x5555ff), LV_PART_MAIN);
            lv_obj_set_style_text_font(ip, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_align(ip, LV_ALIGN_BOTTOM_MID, 0, -5);

            /* Force a render now */
            lv_refr_now(_disp);
        }
        DisplayDriver::unlock();
    }

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Display initialized: %dx%d (RGB565, LVGL)", _width, _height);
    return 0;
}

void DisplayDriver::deinit() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_initialized.load(std::memory_order_relaxed)) {
        xSemaphoreGive(_mutex);
        return;
    }

    /* BSP display handles are managed by bsp_display_delete() */
    bsp_display_delete();
    _disp = nullptr;
    _initialized.store(false, std::memory_order_relaxed);

    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Display deinitialized");
}

/*============================================================================
 * LVGL integration
 *============================================================================*/
bool DisplayDriver::lock(uint32_t timeout_ms) {
    if (!_initialized.load(std::memory_order_relaxed)) return false;
    return bsp_display_lock(timeout_ms);
}

void DisplayDriver::unlock() {
    if (_initialized.load(std::memory_order_relaxed)) {
        bsp_display_unlock();
    }
}

void DisplayDriver::rotate(lv_disp_rotation_t rotation) {
    if (_initialized.load(std::memory_order_relaxed) && _disp) {
        bsp_display_rotate(_disp, rotation);
    }
}