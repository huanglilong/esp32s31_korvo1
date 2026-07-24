/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "camera_frame.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(camera_frame, camera_frame_s, 2, "camera_frame:uint64_t timestamp;uint32_t frame_index;uint16_t width;uint16_t height;uint16_t jpeg_size;uint8_t format;uint8_t[10240] jpeg_data;uint8_t[5] _padding0;", 5);
