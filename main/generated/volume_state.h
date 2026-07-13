/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_VOLUME_STATE_H_
#define UORB_TOPIC_VOLUME_STATE_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_VOLUME_STATE 1

#define VOLUME_STATE_FORMAT_STR "volume_state:uint64_t timestamp;int32_t volume;uint8_t[4] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct volume_state_s
{
    uint64_t                 timestamp;  ///< @brief
    int32_t                  volume;  ///< @brief
} volume_state_s;

#define VOLUME_STATE_SIZE sizeof(volume_state_s)

// NOLINTNEXTLINE
static constexpr size_t volume_state_SIZE_CONST { VOLUME_STATE_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define VOLUME_STATE_SIZE_NO_PADDING (sizeof(volume_state_s) - 4)

// NOLINTNEXTLINE
static constexpr size_t volume_state_SIZE_NO_PADDING_CONST { VOLUME_STATE_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_VOLUME_STATE_H_ */
