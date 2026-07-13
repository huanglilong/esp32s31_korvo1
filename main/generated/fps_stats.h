/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_FPS_STATS_H_
#define UORB_TOPIC_FPS_STATS_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_FPS_STATS 3

#define FPS_STATS_FORMAT_STR "fps_stats:uint64_t timestamp;uint32_t frame_count;uint32_t fps_total_bytes;float fps;uint8_t[4] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct fps_stats_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 frame_count;  ///< @brief
    uint32_t                 fps_total_bytes;  ///< @brief
    float                    fps;  ///< @brief
} fps_stats_s;

#define FPS_STATS_SIZE sizeof(fps_stats_s)

// NOLINTNEXTLINE
static constexpr size_t fps_stats_SIZE_CONST { FPS_STATS_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define FPS_STATS_SIZE_NO_PADDING (sizeof(fps_stats_s) - 4)

// NOLINTNEXTLINE
static constexpr size_t fps_stats_SIZE_NO_PADDING_CONST { FPS_STATS_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_FPS_STATS_H_ */
