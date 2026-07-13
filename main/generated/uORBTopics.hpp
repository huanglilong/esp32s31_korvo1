/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#pragma once

#include <cstdint>
#include "uorb.h"

static constexpr size_t ORB_TOPICS_COUNT { 10 };

enum class ORB_ID : uint8_t {
    audio_level = 0,
    camera_frame = 1,
    camera_state = 2,
    fps_stats = 3,
    recording_state = 4,
    system_alert = 5,
    system_stats = 6,
    ulog_state = 7,
    volume_state = 8,
    wifi_state = 9,
    INVALID
};

extern const struct orb_metadata *const *orb_get_topics();
extern const struct orb_metadata *get_orb_meta(ORB_ID id);
