#pragma once

/*
 * CameraDriver — manages DVP camera (OV3660) mutual exclusion via uORB.
 *
 * Provides camera_available() check and camera claim/release API.
 * Coordinates exclusive access to the DVP camera hardware.
 *
 * Uses uORB camera_state topic for cross-module notification.
 * All public methods are thread-safe (protected by _mutex).
 *
 * Claim ownership:
 *   Each claim() caller provides a caller_id (e.g., module name).
 *   Re-claiming with the same caller_id succeeds (reentrant).
 *   Claiming with a different caller_id fails (mutual exclusion).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uorb.h"

class CameraDriver {
public:
    static CameraDriver& instance();

    /** Check if camera hardware is available (not claimed by another module). */
    bool available() const;

    /** Claim camera hardware. Publishes camera_state.running=true.
     *  @param caller_id  Identifier for the claiming module.
     *                    Re-claiming with the same caller_id succeeds (reentrant).
     *  @return true if successfully claimed, false if already in use */
    bool claim(const char *caller_id = "unknown");

    /** Release camera hardware. Publishes camera_state.running=false. */
    void release(const char *caller_id = "unknown");

    /** @return true if camera is currently claimed */
    bool isClaimed() const;

    /** @return the caller_id of the current claimer, or nullptr */
    const char* claimOwner() const;

    /* Delete copy/move */
    CameraDriver(const CameraDriver&) = delete;
    CameraDriver& operator=(const CameraDriver&) = delete;

private:
    CameraDriver();
    ~CameraDriver();

    bool _available_locked() const;

    orb_advert_t    _pub;
    mutable orb_sub_t _sub;
    bool            _claimed;
    const char     *_owner_id;
    SemaphoreHandle_t _mutex;
};