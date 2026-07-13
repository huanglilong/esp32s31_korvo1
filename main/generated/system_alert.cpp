/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "system_alert.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(system_alert, system_alert_s, 5, "system_alert:uint64_t timestamp;uint32_t current_value;uint32_t threshold;uint32_t task_cpu_pct;uint32_t free_internal;uint32_t free_psram;char[16] task_name;uint8_t alert_type;uint8_t severity;uint8_t[2] _padding0;", 2);
