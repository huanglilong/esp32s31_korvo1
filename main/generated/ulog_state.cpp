/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "ulog_state.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(ulog_state, ulog_state_s, 1, "ulog_state:uint64_t timestamp;char[128] filepath;bool logging;uint8_t[7] _padding0;", 7);
