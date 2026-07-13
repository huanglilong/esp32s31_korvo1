/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_SYSTEM_STATS_H_
#define UORB_TOPIC_SYSTEM_STATS_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_SYSTEM_STATS 3

#define SYSTEM_STATS_FORMAT_STR "system_stats:uint64_t timestamp;uint32_t free_internal;uint32_t free_psram;uint32_t min_free_internal;uint32_t min_free_psram;uint32_t task_count;uint32_t total_cpu_pct;uint32_t core0_cpu_pct;uint32_t core1_cpu_pct;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct system_stats_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 free_internal;  ///< @brief
    uint32_t                 free_psram;  ///< @brief
    uint32_t                 min_free_internal;  ///< @brief
    uint32_t                 min_free_psram;  ///< @brief
    uint32_t                 task_count;  ///< @brief
    uint32_t                 total_cpu_pct;  ///< @brief
    uint32_t                 core0_cpu_pct;  ///< @brief
    uint32_t                 core1_cpu_pct;  ///< @brief
} system_stats_s;

#define SYSTEM_STATS_SIZE sizeof(system_stats_s)

// NOLINTNEXTLINE
static constexpr size_t system_stats_SIZE_CONST { SYSTEM_STATS_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define SYSTEM_STATS_SIZE_NO_PADDING (sizeof(system_stats_s) - 0)

// NOLINTNEXTLINE
static constexpr size_t system_stats_SIZE_NO_PADDING_CONST { SYSTEM_STATS_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_SYSTEM_STATS_H_ */
