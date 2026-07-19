#pragma once

/**
 * @brief Start the Brookesia application runtime.
 *
 * Initializes the TaskScheduler, ServiceManager, HAL adaptor, Display service,
 * and LVGL GUI. Only does work when CONFIG_APP_BROOKESIA_ENABLE is set.
 *
 * @return true if the runtime was started successfully (or if Brookesia is disabled)
 * @return false if initialization failed
 */
bool brookesia_app_start();
