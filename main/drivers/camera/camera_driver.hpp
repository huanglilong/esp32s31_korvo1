#pragma once

#include <atomic>

/*
 * CameraDriver — manages DVP camera (OV3660) mutual exclusion via uORB.
 *
 * Follows esp_board_manager Camera device pattern:
 *   - dev_camera_config_t: config struct with sub-type (dvp/csi/spi/usb_uvc)
 *   - dev_camera_sub_dvp_cfg: DVP-specific config (I2C, pins, XCLK)
 *   - dev_camera_handle_t: handle with dev_path and meta_path
 *
 * Provides camera claim/release API with mutual exclusion.
 * Coordinates exclusive access to the DVP camera hardware.
 *
 * Uses uORB camera_state topic for cross-module notification.
 * All public methods are thread-safe (protected by _mutex).
 *
 * Claim ownership:
 *   Each claim() caller provides a caller_id (e.g., module name).
 *   Re-claiming with the same caller_id succeeds (reentrant).
 *   Claiming with a different caller_id fails (mutual exclusion).
 *
 * Hardware init uses esp_video_init() API (same as esp_board_manager dev_camera_sub_dvp).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "uorb.h"

/* ── esp_board_manager-compatible config structures ────────────── */

#if __has_include("esp_cam_ctlr_dvp.h")
#include "esp_cam_ctlr_dvp.h"
#endif

/** DVP IO pin configuration (matches esp_board_manager esp_cam_ctlr_dvp_pin_config_t) */
typedef struct {
    int data_io[8];        /*!< D0-D7 data pins */
    int pclk_io;           /*!< Pixel clock pin */
    int hsync_io;          /*!< Horizontal sync pin */
    int vsync_io;          /*!< Vertical sync pin */
} camera_dvp_pin_config_t;

/** DVP sub configuration (matches esp_board_manager dev_camera_sub_dvp_cfg) */
typedef struct {
    const char             *i2c_name;   /*!< I2C bus name */
    uint32_t                i2c_freq;   /*!< I2C frequency (Hz) */
    gpio_num_t              reset_io;   /*!< Reset GPIO, -1 if not used */
    gpio_num_t              pwdn_io;    /*!< Power-down GPIO, -1 if not used */
    camera_dvp_pin_config_t dvp_io;     /*!< DVP pin configuration */
    uint32_t                xclk_freq;  /*!< XCLK frequency in Hz */
} dev_camera_sub_dvp_cfg_t;

/** Camera configuration (matches esp_board_manager dev_camera_config_t) */
typedef struct {
    const char *name;        /*!< Device name */
    const char *type;        /*!< Device type ("camera") */
    const char *sub_type;    /*!< Bus type ("dvp", "csi", "spi", "usb_uvc") */
    union {
        dev_camera_sub_dvp_cfg_t dvp;  /*!< DVP interface configuration */
    } sub_cfg;
} dev_camera_config_t;

/** Camera device handle (matches esp_board_manager dev_camera_handle_t) */
typedef struct {
    const char *dev_path;   /*!< Camera device path (e.g., "/dev/video0") */
    const char *meta_path;  /*!< Camera metadata path (e.g., "/dev/video11" for ISP) */
    void       *video_handle; /*!< Opaque video device handle */
} dev_camera_handle_t;

/* ── CameraDriver class ────────────────────────────────────────── */

class CameraDriver {
public:
    static CameraDriver& instance();

    /**
     * @brief  Initialize camera with given configuration.
     *         Follows esp_board_manager dev_camera_init() signature.
     *         Thread-safe.
     *
     * @param[in]  cfg      Pointer to dev_camera_config_t
     * @param[in]  cfg_size Size of config struct
     * @param[out] handle   Pointer to receive dev_camera_handle_t*
     * @return 0 on success, -1 on failure
     */
    int init(dev_camera_config_t *cfg, int cfg_size, void **handle);

    /** Deinitialize camera. Thread-safe. */
    int deinit();

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

    /** @return the device handles struct, or nullptr */
    const dev_camera_handle_t* handles() const {
        return _handle.load(std::memory_order_acquire);
    }

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
    std::atomic<bool> _initialized;
    std::atomic<dev_camera_handle_t*> _handle;
};