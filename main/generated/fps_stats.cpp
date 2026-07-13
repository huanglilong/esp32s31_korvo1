/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "fps_stats.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(fps_stats, fps_stats_s, 3, "fps_stats:uint64_t timestamp;uint32_t frame_count;uint32_t fps_total_bytes;float fps;uint8_t[4] _padding0;", 4);
