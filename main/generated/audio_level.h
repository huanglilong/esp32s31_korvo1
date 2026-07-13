/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_AUDIO_LEVEL_H_
#define UORB_TOPIC_AUDIO_LEVEL_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_AUDIO_LEVEL 1

#define AUDIO_LEVEL_FORMAT_STR "audio_level:uint64_t timestamp;float level_left;float level_right;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct audio_level_s
{
    uint64_t                 timestamp;  ///< @brief
    float                    level_left;  ///< @brief
    float                    level_right;  ///< @brief
} audio_level_s;

#define AUDIO_LEVEL_SIZE sizeof(audio_level_s)

// NOLINTNEXTLINE
static constexpr size_t audio_level_SIZE_CONST { AUDIO_LEVEL_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define AUDIO_LEVEL_SIZE_NO_PADDING (sizeof(audio_level_s) - 0)

// NOLINTNEXTLINE
static constexpr size_t audio_level_SIZE_NO_PADDING_CONST { AUDIO_LEVEL_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_AUDIO_LEVEL_H_ */
