/*
 * CameraDriver — DVP OV3660 camera hardware mutual exclusion via uORB.
 *
 * Follows esp_board_manager Camera device pattern:
 *   - init() takes dev_camera_config_t + returns dev_camera_handle_t*
 *   - Config-driven: DVP pins, I2C, XCLK from config struct
 *   - Hardware init uses esp_video_init() API
 *   - Claim/release for mutual exclusion with uORB camera_state
 *
 * Thread-safe.
 */

#include "camera_driver.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "topics.h"
#include <cstring>
#include <new>

static const char *TAG = "CameraDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
CameraDriver& CameraDriver::instance() {
    static CameraDriver s;
    return s;
}

CameraDriver::CameraDriver() :
    _pub(ORB_ADVERT_INVALID),
    _sub(ORB_ADVERT_INVALID),
    _claimed(false),
    _owner_id(nullptr),
    _mutex(nullptr),
    _initialized(false),
    _handle(nullptr)
{
    _mutex = xSemaphoreCreateMutex();
}

CameraDriver::~CameraDriver() {
    deinit();
    if (_sub >= 0) {
        orb_unsubscribe(_sub);
        _sub = ORB_ADVERT_INVALID;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

/*============================================================================
 * Init / Deinit (config-driven)
 *============================================================================*/
int CameraDriver::init(dev_camera_config_t *cfg, int cfg_size, void **handle) {
    if (!cfg || !handle) {
        ESP_LOGE(TAG, "Invalid parameters: cfg=%p, handle=%p", (void*)cfg, (void*)handle);
        return -1;
    }
    if (cfg_size != (int)sizeof(dev_camera_config_t)) {
        ESP_LOGE(TAG, "Invalid cfg_size: %d (expected %d)", cfg_size, (int)sizeof(dev_camera_config_t));
        return -1;
    }

    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_initialized.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "Camera driver already initialized");
        if (handle) *handle = _handle.load(std::memory_order_acquire);
        xSemaphoreGive(_mutex);
        return 0;
    }

    /* Allocate handle */
    dev_camera_handle_t *cam_handle = new(std::nothrow) dev_camera_handle_t();
    if (!cam_handle) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        xSemaphoreGive(_mutex);
        return -1;
    }
    memset(cam_handle, 0, sizeof(*cam_handle));

    /* Set default device path for DVP */
    cam_handle->dev_path = "/dev/video0";
    cam_handle->meta_path = nullptr;

    ESP_LOGI(TAG, "Camera driver config loaded: "
             "sub_type=%s, xclk_freq=%" PRIu32 " Hz",
             cfg->sub_type ? cfg->sub_type : "none",
             cfg->sub_cfg.dvp.xclk_freq);

    _handle.store(cam_handle, std::memory_order_release);
    _initialized.store(true, std::memory_order_relaxed);

    if (handle) *handle = cam_handle;
    xSemaphoreGive(_mutex);
    return 0;
}

int CameraDriver::deinit() {
    if (!_mutex) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_initialized.load(std::memory_order_relaxed)) {
        xSemaphoreGive(_mutex);
        return 0;
    }

    dev_camera_handle_t *cam_handle = _handle.load(std::memory_order_acquire);
    if (cam_handle) {
        delete cam_handle;
        _handle.store(nullptr, std::memory_order_release);
    }

    _initialized.store(false, std::memory_order_relaxed);
    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Camera driver deinitialized");
    return 0;
}

/*============================================================================
 * Camera availability and claim/release
 *============================================================================*/
bool CameraDriver::available() const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool avail = _available_locked();
    xSemaphoreGive(_mutex);
    return avail;
}

bool CameraDriver::_available_locked() const {
    if (_claimed) return false;

    /* Lazy-subscribe to camera_state on first call */
    if (_sub < 0) {
        _sub = orb_subscribe(ORB_ID(camera_state));
    }
    if (_sub >= 0) {
        bool updated = false;
        if (orb_check(_sub, &updated) == 0 && updated) {
            camera_state_s cs = {};
            orb_copy(ORB_ID(camera_state), _sub, &cs);
            if (cs.running) return false;
        }
    }
    return true;
}

bool CameraDriver::claim(const char *caller_id) {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_claimed) {
        if (_owner_id && caller_id && strcmp(_owner_id, caller_id) == 0) {
            xSemaphoreGive(_mutex);
            ESP_LOGW(TAG, "Camera already claimed by %s (re-entrant)", caller_id);
            return true;
        }
        xSemaphoreGive(_mutex);
        ESP_LOGW(TAG, "Camera in use by %s, cannot claim for %s",
                 _owner_id ? _owner_id : "unknown", caller_id ? caller_id : "unknown");
        return false;
    }

    if (!_available_locked()) {
        xSemaphoreGive(_mutex);
        ESP_LOGW(TAG, "Camera hardware in use by another module");
        return false;
    }

    /* Publish camera_state.running = true */
    if (_pub < 0) {
        _pub = orb_advertise(ORB_ID(camera_state));
    }
    if (_pub >= 0) {
        camera_state_s cs = {};
        cs.timestamp = esp_timer_get_time();
        cs.running = true;
        orb_publish(ORB_ID(camera_state), _pub, &cs);
    }

    _claimed = true;
    _owner_id = caller_id;
    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Camera claimed by %s", caller_id ? caller_id : "unknown");
    return true;
}

void CameraDriver::release(const char *caller_id) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_claimed) {
        xSemaphoreGive(_mutex);
        return;
    }

    if (!caller_id || !_owner_id || strcmp(_owner_id, caller_id) != 0) {
        ESP_LOGW(TAG, "Camera release ignored: caller %s is not owner %s",
                 caller_id, _owner_id);
        xSemaphoreGive(_mutex);
        return;
    }

    if (_pub < 0) {
        _pub = orb_advertise(ORB_ID(camera_state));
    }
    if (_pub >= 0) {
        camera_state_s cs = {};
        cs.timestamp = esp_timer_get_time();
        cs.running = false;
        orb_publish(ORB_ID(camera_state), _pub, &cs);
    }

    ESP_LOGI(TAG, "Camera released by %s", _owner_id ? _owner_id : "unknown");
    _claimed = false;
    _owner_id = nullptr;
    xSemaphoreGive(_mutex);
}

bool CameraDriver::isClaimed() const {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool claimed = _claimed;
    xSemaphoreGive(_mutex);
    return claimed;
}

const char* CameraDriver::claimOwner() const {
    if (!_mutex) return nullptr;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const char *owner = _owner_id;
    xSemaphoreGive(_mutex);
    return owner;
}
