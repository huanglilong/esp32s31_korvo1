/*
 * ESP-Brookesia integration boundary.
 *
 * This translation unit validates that the project's ESP-IDF 6.2 build
 * can consume the Brookesia master/v0.8 public layers. When
 * APP_BROOKESIA_ENABLE is toggled on, the runtime will initialize
 * the Brookesia HAL, ServiceManager, and services.
 *
 * HAL version dependency fixed: brookesia_hal_boards/adaptor previously
 * required esp_board_manager 0.5.* — corrected to >=0.6.0 in local clone.
 */

#include "sdkconfig.h"

#if CONFIG_APP_BROOKESIA_BUILD
#include "brookesia/gui_lvgl.hpp"
#include "brookesia/service_audio.hpp"
#include "brookesia/service_display.hpp"
#include "brookesia/service_video.hpp"
#include "brookesia/hal_adaptor.hpp"
#include "brookesia/service_manager.hpp"
#include "brookesia/runtime_manager.hpp"
#endif

bool brookesia_integration_compiled()
{
#if CONFIG_APP_BROOKESIA_BUILD
    return true;
#else
    return false;
#endif
}
