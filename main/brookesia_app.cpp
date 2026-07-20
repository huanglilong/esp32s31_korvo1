/*
 * ESP-Brookesia application runtime.
 *
 * Initializes the Brookesia HAL, ServiceManager, Display service,
 * LVGL GUI, and System Super Phone UI when APP_BROOKESIA_ENABLE=y.
 *
 * When disabled, all functions are no-ops — the legacy BSP-based
 * drivers remain the sole hardware owners.
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"

#if CONFIG_APP_BROOKESIA_ENABLE
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "brookesia/gui_lvgl.hpp"
#include "brookesia/hal_adaptor.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_helper.hpp"
#include "brookesia/service_manager.hpp"
#include "brookesia/service_wifi.hpp"

#include "lvgl.h"
#include "esp_lv_adapter.h"

using namespace esp_brookesia;
using DisplayHelper = service::helper::Display;
using LvglDisplaySource = gui::lvgl::DisplaySource;

static const char *TAG = "BrookesiaApp";
static std::shared_ptr<lib_utils::TaskScheduler> s_backend_scheduler;

static constexpr uint32_t SERVICE_TIMEOUT_MS = 1000;

using WifiHelper = esp_brookesia::service::helper::Wifi;

static void create_test_ui()
{
    esp_lv_adapter_lock(-1);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x003366), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "ESP32-S31 Brookesia");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Display service active");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x88CCFF), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "800x480 RGB565");
    lv_obj_set_style_text_color(status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 40);

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "Test UI created on LVGL");
}

static std::optional<esp_brookesia::service::ServiceBinding> s_wifi_binding;

static bool start_wifi_service()
{
    ESP_LOGI(TAG, "Starting Brookesia WiFi service...");

    if (!WifiHelper::is_available()) {
        ESP_LOGE(TAG, "WiFi service is not available");
        return false;
    }

    /* Bind the WiFi service — keep the binding alive so the service
     * doesn't get unbound and stopped when the binding goes out of scope. */
    s_wifi_binding.emplace(
        service::ServiceManager::get_instance().bind(WifiHelper::get_name().data())
    );
    if (!s_wifi_binding->is_valid()) {
        ESP_LOGE(TAG, "Failed to bind WiFi service");
        s_wifi_binding.reset();
        return false;
    }

    /* Set SoftAP params with fixed channel so SoftAP can start immediately
     * without needing a scan for channel selection.
     * SSID will use device MAC suffix for uniqueness. */
    {
        boost::json::object softap_params;
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char ssid[24];
        snprintf(ssid, sizeof(ssid), "esp-s31-%02x%02x%02x", mac[3], mac[4], mac[5]);
        softap_params["ssid"] = std::string(ssid);
        softap_params["password"] = std::string("");
        softap_params["max_connection"] = 4;
        softap_params["channel"] = 1;  /* Fixed channel 1 — no scan needed */

        auto set_result = WifiHelper::call_function_sync(
            WifiHelper::FunctionId::SetSoftApParams,
            softap_params,
            service::helper::Timeout(5000)
        );
        if (!set_result.has_value()) {
            ESP_LOGW(TAG, "SetSoftApParams failed: %s", set_result.error().c_str());
        }
    }

    /* Trigger Init action */
    auto init_result = WifiHelper::call_function_sync(
        WifiHelper::FunctionId::TriggerGeneralAction,
        std::string("Init"),
        service::helper::Timeout(10000)
    );
    if (!init_result.has_value()) {
        ESP_LOGW(TAG, "WiFi Init action failed: %s", init_result.error().c_str());
        return false;
    }

    /* Wait for WiFi to reach "Inited" state */
    int retry = 0;
    while (retry < 20) {
        auto state_result = WifiHelper::call_function_sync<std::string>(
            WifiHelper::FunctionId::GetGeneralState,
            service::helper::Timeout(3000)
        );
        if (state_result.has_value() && state_result.value() == "Inited") {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    if (retry >= 20) {
        ESP_LOGW(TAG, "WiFi Init timed out");
        return false;
    }
    ESP_LOGI(TAG, "WiFi service initialized");

    /* Trigger Start action */
    auto start_result = WifiHelper::call_function_sync(
        WifiHelper::FunctionId::TriggerGeneralAction,
        std::string("Start"),
        service::helper::Timeout(10000)
    );
    if (!start_result.has_value()) {
        ESP_LOGW(TAG, "WiFi Start action failed: %s", start_result.error().c_str());
        return false;
    }

    /* Wait for WiFi to reach "Started" state */
    retry = 0;
    while (retry < 20) {
        auto state_result = WifiHelper::call_function_sync<std::string>(
            WifiHelper::FunctionId::GetGeneralState,
            service::helper::Timeout(3000)
        );
        if (state_result.has_value()) {
            const auto &state = state_result.value();
            if (state == "Started" || state == "Connected" || state == "Connecting") {
                ESP_LOGI(TAG, "WiFi service started (state=%s)", state.c_str());
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    if (retry >= 20) {
        ESP_LOGW(TAG, "WiFi Start timed out");
        return false;
    }

    /* If no stored credentials, start SoftAP for first-time setup.
     * Use TriggerSoftApStart instead of TriggerSoftApProvisionStart because
     * the provision flow has a bug: the intentional STA disconnect at provision
     * start triggers WIFI_EVENT_STA_DISCONNECTED, which is misidentified as a
     * provision failure, causing immediate cleanup. SoftAP start alone works
     * correctly — the AP is visible and the web config UI handles provisioning. */
    auto state_result = WifiHelper::call_function_sync<std::string>(
        WifiHelper::FunctionId::GetGeneralState,
        service::helper::Timeout(3000)
    );
    if (state_result.has_value() && state_result.value() == "Started") {
        auto ap_result = WifiHelper::call_function_sync<boost::json::object>(
            WifiHelper::FunctionId::GetConnectAp,
            service::helper::Timeout(3000)
        );
        bool has_credentials = false;
        if (ap_result.has_value()) {
            const auto &obj = ap_result.value();
            auto it = obj.find("ssid");
            has_credentials = (it != obj.end() && it->value().is_string() &&
                              std::string(it->value().as_string()).length() > 0);
        }

        if (!has_credentials) {
            ESP_LOGI(TAG, "No stored WiFi credentials — starting SoftAP");
            auto softap_result = WifiHelper::call_function_sync(
                WifiHelper::FunctionId::TriggerSoftApStart,
                service::helper::Timeout(15000)
            );
            if (!softap_result.has_value()) {
                ESP_LOGW(TAG, "SoftAP start failed: %s", softap_result.error().c_str());
            } else {
                ESP_LOGI(TAG, "SoftAP started — connect to WiFi hotspot to configure");
            }
        }
    }

    return true;
}

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

    /* Create UI content on LVGL */
    create_test_ui();

    ESP_LOGI(TAG, "Brookesia Display service started");
    return true;
}
#endif // CONFIG_APP_BROOKESIA_ENABLE

bool brookesia_app_start()
{
#if CONFIG_APP_BROOKESIA_ENABLE
    ESP_LOGI(TAG, "=== Brookesia App Runtime ===");

    /* esp_netif_init() and esp_event_loop_create_default() may already be
     * initialized by WifiService (legacy WiFi). ESP_ERROR_CHECK would abort
     * on ESP_ERR_INVALID_STATE — handle gracefully instead. */
    esp_err_t _netif_err = esp_netif_init();
    if (_netif_err == ESP_OK) {
        ESP_LOGI(TAG, "esp_netif initialized");
    } else if (_netif_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(_netif_err));
        return false;
    }

    esp_err_t _evt_err = esp_event_loop_create_default();
    if (_evt_err == ESP_OK) {
        ESP_LOGI(TAG, "Default event loop created");
    } else if (_evt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(_evt_err));
        return false;
    }

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

    if (!start_wifi_service()) {
        ESP_LOGW(TAG, "WiFi service unavailable — continuing without WiFi");
    }

    if (!start_display_service()) {
        ESP_LOGW(TAG, "Display service unavailable — continuing without display");
    }

    ESP_LOGI(TAG, "=== Brookesia App Runtime started ===");
    return true;
#else
    return false;
#endif // CONFIG_APP_BROOKESIA_ENABLE
}
