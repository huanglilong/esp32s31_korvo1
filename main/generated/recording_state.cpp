/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "recording_state.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(recording_state, recording_state_s, 1, "recording_state:uint64_t timestamp;uint32_t bytes_written;uint32_t elapsed_ms;bool active;uint8_t[7] _padding0;", 7);
