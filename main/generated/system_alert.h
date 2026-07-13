/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_SYSTEM_ALERT_H_
#define UORB_TOPIC_SYSTEM_ALERT_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_SYSTEM_ALERT 5

#define SYSTEM_ALERT_FORMAT_STR "system_alert:uint64_t timestamp;uint32_t current_value;uint32_t threshold;uint32_t task_cpu_pct;uint32_t free_internal;uint32_t free_psram;char[16] task_name;uint8_t alert_type;uint8_t severity;uint8_t[2] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct system_alert_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 current_value;  ///< @brief
    uint32_t                 threshold;  ///< @brief
    uint32_t                 task_cpu_pct;  ///< @brief
    uint32_t                 free_internal;  ///< @brief
    uint32_t                 free_psram;  ///< @brief
    char                     task_name[16];  ///< @brief
    uint8_t                  alert_type;  ///< @brief
    uint8_t                  severity;  ///< @brief
} system_alert_s;

#define SYSTEM_ALERT_SIZE sizeof(system_alert_s)

// NOLINTNEXTLINE
static constexpr size_t system_alert_SIZE_CONST { SYSTEM_ALERT_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define SYSTEM_ALERT_SIZE_NO_PADDING (sizeof(system_alert_s) - 2)

// NOLINTNEXTLINE
static constexpr size_t system_alert_SIZE_NO_PADDING_CONST { SYSTEM_ALERT_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_SYSTEM_ALERT_H_ */
