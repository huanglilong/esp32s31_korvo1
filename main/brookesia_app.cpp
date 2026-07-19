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
#include "esp_netif.h"
#include "esp_event.h"

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

static constexpr uint32_t SERVICE_TIMEOUT_MS = 1000;

static bool start_display_service()
{
    ESP_LOGI(TAG, "Starting Brookesia Display service...");

    if (!DisplayHelper::is_available()) {
        ESP_LOGE(TAG, "Display service is not available");
        return false;
    }

    auto binding = service::ServiceManager::get_instance().bind(DisplayHelper::get_name().data());
    if (!binding.is_valid()) {
        ESP_LOGE(TAG, "Failed to bind Display service");
        return false;
    }

    auto outputs_json = DisplayHelper::call_function_sync<boost::json::array>(
        DisplayHelper::FunctionId::GetOutputs,
        service::helper::Timeout(SERVICE_TIMEOUT_MS)
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

    gui::lvgl::DisplaySourceConfig lvgl_config{};
    lvgl_config.output_name = "";
    lvgl_config.task_core_id = 1;
    lvgl_config.task_stack_size = 40 * 1024;

    auto &lvgl_source = LvglDisplaySource::get_instance();
    if (!lvgl_source.start(lvgl_config)) {
        ESP_LOGE(TAG, "Failed to start LVGL Display source");
        return false;
    }

    auto result = DisplayHelper::call_function_sync(
        DisplayHelper::FunctionId::SetActiveSourceRole,
        std::string(),
        std::string(gui::lvgl::DISPLAY_SOURCE_ROLE),
        service::helper::Timeout(SERVICE_TIMEOUT_MS)
    );
    if (!result.has_value()) {
        ESP_LOGE(TAG, "Failed to activate LVGL source");
        return false;
    }

    if (main_output.backlight.has_value() && main_output.backlight->on_off_supported) {
        auto bl_result = DisplayHelper::call_function_async(
            DisplayHelper::FunctionId::SetBacklightOnOff,
            static_cast<double>(main_output.id), true
        );
        if (!bl_result) {
            ESP_LOGW(TAG, "Failed to turn on backlight");
        }
    }

    ESP_LOGI(TAG, "Brookesia Display service started");
    return true;
}
#endif // CONFIG_APP_BROOKESIA_ENABLE

bool brookesia_app_start()
{
#if CONFIG_APP_BROOKESIA_ENABLE
    ESP_LOGI(TAG, "=== Brookesia App Runtime ===");

    /* Initialize TCP/IP network stack before any HAL device (WiFi) uses it */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create default event loop (required by mdns, wifi_manager, etc.) */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Default event loop created");
    ESP_LOGI(TAG, "esp_netif initialized");

    s_backend_scheduler = std::make_shared<lib_utils::TaskScheduler>();
    if (!s_backend_scheduler) {
        ESP_LOGE(TAG, "Failed to create task scheduler");
        return false;
    }

    auto start_result = s_backend_scheduler->start({
        .worker_configs = {
            { .name = "BrookesiaWorker1", .core_id = 0, .priority = 1, .stack_size = 10 * 1024 },
            { .name = "BrookesiaWorker2", .core_id = 1, .priority = 1, .stack_size = 10 * 1024 },
        }
    });
    if (!start_result) {
        ESP_LOGE(TAG, "Failed to start task scheduler");
        return false;
    }

    auto &service_manager = service::ServiceManager::get_instance();
    if (!service_manager.start()) {
        ESP_LOGE(TAG, "Failed to start ServiceManager");
        return false;
    }
    ESP_LOGI(TAG, "ServiceManager started — Brookesia owns all HAL devices");

    if (!start_display_service()) {
        ESP_LOGW(TAG, "Display service unavailable — continuing without display");
    }

    ESP_LOGI(TAG, "=== Brookesia App Runtime started ===");
    return true;
#else
    return false;
#endif // CONFIG_APP_BROOKESIA_ENABLE
}
