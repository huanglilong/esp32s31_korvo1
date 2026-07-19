/*
 * ESP-Brookesia integration boundary.
 *
 * This translation unit intentionally does not initialize hardware yet. It
 * validates that the project's ESP-IDF 6.2 build can consume the Brookesia
 * master/v0.8 public layers before runtime ownership is migrated.
 */

#include "sdkconfig.h"

#if CONFIG_APP_BROOKESIA_BUILD
#include "brookesia/gui_lvgl.hpp"
#include "brookesia/service_audio/service_audio.hpp"
#endif

bool brookesia_integration_compiled()
{
#if CONFIG_APP_BROOKESIA_BUILD
    return true;
#else
    return false;
#endif
}
