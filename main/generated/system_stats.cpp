/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "system_stats.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(system_stats, system_stats_s, 3, "system_stats:uint64_t timestamp;uint32_t free_internal;uint32_t free_psram;uint32_t min_free_internal;uint32_t min_free_psram;uint32_t task_count;uint32_t total_cpu_pct;uint32_t core0_cpu_pct;uint32_t core1_cpu_pct;", 0);
