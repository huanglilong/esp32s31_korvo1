/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_AUDIO_FRAME_H_
#define UORB_TOPIC_AUDIO_FRAME_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_AUDIO_FRAME 4

#define AUDIO_FRAME_FORMAT_STR "audio_frame:uint64_t timestamp;uint32_t frame_index;uint16_t sample_rate;uint8_t channel;uint8_t bits_per_sample;uint16_t aac_size;uint8_t[1024] aac_data;uint8_t[6] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct audio_frame_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 frame_index;  ///< @brief
    uint16_t                 sample_rate;  ///< @brief
    uint8_t                  channel;  ///< @brief
    uint8_t                  bits_per_sample;  ///< @brief
    uint16_t                 aac_size;  ///< @brief
    uint8_t                  aac_data[1024];  ///< @brief
} audio_frame_s;

#define AUDIO_FRAME_SIZE sizeof(audio_frame_s)

// NOLINTNEXTLINE
static constexpr size_t audio_frame_SIZE_CONST { AUDIO_FRAME_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define AUDIO_FRAME_SIZE_NO_PADDING (sizeof(audio_frame_s) - 6)

// NOLINTNEXTLINE
static constexpr size_t audio_frame_SIZE_NO_PADDING_CONST { AUDIO_FRAME_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_AUDIO_FRAME_H_ */
