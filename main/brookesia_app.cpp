/*
 * ESP-Brookesia application runtime.
 *
 * Initializes the Brookesia HAL, ServiceManager, Display service,
 * and LVGL GUI when APP_BROOKESIA_ENABLE is enabled.
 *
 * When disabled, all functions are no-ops — the legacy BSP-based
 * drivers remain the sole hardware owners.
 */

#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_APP_BROOKESIA_ENABLE
#include <memory>
#include <string>
#include <vector>

#include "brookesia/gui_lvgl.hpp"
#include "brookesia/hal_adaptor.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_helper.hpp"
#include "brookesia/service_manager.hpp"

using namespace esp_brookesia;
using DisplayHelper = service::helper::Display;
using LvglDisplaySource = gui::lvgl::DisplaySource;

static const char *TAG = "BrookesiaApp";
static std::shared_ptr<lib_utils::TaskScheduler> s_backend_scheduler;

static constexpr uint32_t DISPLAY_SERVICE_TIMEOUT_MS = 1000;

static bool start_display_service()
{
    ESP_LOGI(TAG, "Starting Brookesia Display service...");

    // Check if Display service is available
    if (!DisplayHelper::is_available()) {
        ESP_LOGE(TAG, "Display service is not available");
        return false;
    }

    // Bind to the Display service
    auto binding = service::ServiceManager::get_instance().bind(DisplayHelper::get_name().data());
    if (!binding.is_valid()) {
        ESP_LOGE(TAG, "Failed to bind Display service");
        return false;
    }

    // Get display outputs
    auto outputs_json = DisplayHelper::call_function_sync<boost::json::array>(
        DisplayHelper::FunctionId::GetOutputs,
        service::helper::Timeout(DISPLAY_SERVICE_TIMEOUT_MS)
    );
    if (!outputs_json.has_value()) {
        ESP_LOGE(TAG, "Failed to get Display outputs");
        return false;
    }

    std::vector<DisplayHelper::OutputInfo> outputs;
    if (!BROOKESIA_DESCRIBE_FROM_JSON(boost::json::value(outputs_json.value()), outputs)) {
        ESP_LOGE(TAG, "Failed to parse Display outputs");
        return false;
    }
    if (outputs.empty()) {
        ESP_LOGE(TAG, "No Display output available");
        return false;
    }

    const auto &main_output = outputs.front();
    ESP_LOGI(TAG, "Display output: %s (%ux%u)", main_output.name.c_str(),
             main_output.width, main_output.height);

    // Start LVGL display source
    gui::lvgl::DisplaySourceConfig lvgl_config{};
    lvgl_config.output_name = "";
    lvgl_config.task_core_id = 1;  // Core 1 for LVGL (Core 0 for WiFi/Camera)
    lvgl_config.task_stack_size = 40 * 1024;

    auto &lvgl_source = LvglDisplaySource::get_instance();
    if (!lvgl_source.start(lvgl_config)) {
        ESP_LOGE(TAG, "Failed to start LVGL Display source");
        return false;
    }

    // Activate LVGL as the active source
    auto result = DisplayHelper::call_function_sync(
        DisplayHelper::FunctionId::SetActiveSourceRole,
        std::string(),
        std::string(gui::lvgl::DISPLAY_SOURCE_ROLE),
        service::helper::Timeout(DISPLAY_SERVICE_TIMEOUT_MS)
    );
    if (!result.has_value()) {
        ESP_LOGE(TAG, "Failed to activate LVGL source");
        return false;
    }

    // Turn on backlight if supported
    if (main_output.backlight.has_value() && main_output.backlight->on_off_supported) {
        auto bl_result = DisplayHelper::call_function_async(
            DisplayHelper::FunctionId::SetBacklightOnOff,
            static_cast<double>(main_output.id), true
        );
        if (!bl_result) {
            ESP_LOGW(TAG, "Failed to turn on backlight");
        } else {
            ESP_LOGI(TAG, "Backlight turned on");
        }
    } else {
        ESP_LOGI(TAG, "Backlight on/off not supported by this output");
    }

    ESP_LOGI(TAG, "Brookesia Display service started successfully");
    return true;
}
#endif // CONFIG_APP_BROOKESIA_ENABLE

bool brookesia_app_start()
{
#if CONFIG_APP_BROOKESIA_ENABLE
    ESP_LOGI(TAG, "=== Brookesia App Runtime ===");

    // Create a task scheduler for backend operations
    s_backend_scheduler = std::make_shared<lib_utils::TaskScheduler>();
    if (!s_backend_scheduler) {
        ESP_LOGE(TAG, "Failed to create task scheduler");
        return false;
    }

    auto start_result = s_backend_scheduler->start({
        .worker_configs = {
            {
                .name = "BrookesiaWorker1",
                .core_id = 0,
                .priority = 1,
                .stack_size = 10 * 1024,
            },
            {
                .name = "BrookesiaWorker2",
                .core_id = 1,
                .priority = 1,
                .stack_size = 10 * 1024,
            },
        }
    });
    if (!start_result) {
        ESP_LOGE(TAG, "Failed to start task scheduler");
        return false;
    }

    // Start the ServiceManager — this probes/init/starts HAL devices and services
    auto &service_manager = service::ServiceManager::get_instance();
    if (!service_manager.start()) {
        ESP_LOGE(TAG, "Failed to start ServiceManager");
        return false;
    }
    ESP_LOGI(TAG, "ServiceManager started");

    // Start the Display service
    if (!start_display_service()) {
        ESP_LOGE(TAG, "Failed to start Display service — continuing without display");
        // Non-fatal — other services may still work
    }

    ESP_LOGI(TAG, "=== Brookesia App Runtime started ===");
    return true;
#else
    return false;
#endif // CONFIG_APP_BROOKESIA_ENABLE
}
