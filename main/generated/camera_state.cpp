/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "camera_state.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(camera_state, camera_state_s, 1, "camera_state:uint64_t timestamp;bool running;uint8_t[7] _padding0;", 7);
