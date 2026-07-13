/*
 * CameraDriver — DVP OV3660 camera hardware mutual exclusion via uORB.
 *
 * Provides claim/release for camera hardware, with uORB camera_state
 * topic for cross-module coordination. Thread-safe.
 */

#include "camera_driver.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "topics.h"
#include <cstring>

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
    _mutex(nullptr)
{
    _mutex = xSemaphoreCreateMutex();
}

CameraDriver::~CameraDriver() {
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