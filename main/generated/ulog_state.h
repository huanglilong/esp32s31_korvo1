/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_ULOG_STATE_H_
#define UORB_TOPIC_ULOG_STATE_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_ULOG_STATE 1

#define ULOG_STATE_FORMAT_STR "ulog_state:uint64_t timestamp;char[128] filepath;bool logging;uint8_t[7] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct ulog_state_s
{
    uint64_t                 timestamp;  ///< @brief
    char                     filepath[128];  ///< @brief
    bool                     logging;  ///< @brief
} ulog_state_s;

#define ULOG_STATE_SIZE sizeof(ulog_state_s)

// NOLINTNEXTLINE
static constexpr size_t ulog_state_SIZE_CONST { ULOG_STATE_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define ULOG_STATE_SIZE_NO_PADDING (sizeof(ulog_state_s) - 7)

// NOLINTNEXTLINE
static constexpr size_t ulog_state_SIZE_NO_PADDING_CONST { ULOG_STATE_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_ULOG_STATE_H_ */
