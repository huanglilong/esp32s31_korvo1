/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#pragma once

#include <cstdint>
#include "uorb.h"

static constexpr size_t ORB_TOPICS_COUNT { 11 };

enum class ORB_ID : uint8_t {
    audio_frame = 0,
    audio_level = 1,
    camera_frame = 2,
    camera_state = 3,
    fps_stats = 4,
    recording_state = 5,
    system_alert = 6,
    system_stats = 7,
    ulog_state = 8,
    volume_state = 9,
    wifi_state = 10,
    INVALID
};

extern const struct orb_metadata *const *orb_get_topics();
extern const struct orb_metadata *get_orb_meta(ORB_ID id);
