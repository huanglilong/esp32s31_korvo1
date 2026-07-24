/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_CAMERA_FRAME_H_
#define UORB_TOPIC_CAMERA_FRAME_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_CAMERA_FRAME 2

#define CAMERA_FRAME_FORMAT_STR "camera_frame:uint64_t timestamp;uint32_t frame_index;uint16_t width;uint16_t height;uint16_t jpeg_size;uint8_t format;uint8_t[10240] jpeg_data;uint8_t[5] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct camera_frame_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 frame_index;  ///< @brief
    uint16_t                 width;  ///< @brief
    uint16_t                 height;  ///< @brief
    uint16_t                 jpeg_size;  ///< @brief
    uint8_t                  format;  ///< @brief
    uint8_t                  jpeg_data[10240];  ///< @brief
} camera_frame_s;

#define CAMERA_FRAME_SIZE sizeof(camera_frame_s)

// NOLINTNEXTLINE
static constexpr size_t camera_frame_SIZE_CONST { CAMERA_FRAME_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define CAMERA_FRAME_SIZE_NO_PADDING (sizeof(camera_frame_s) - 5)

// NOLINTNEXTLINE
static constexpr size_t camera_frame_SIZE_NO_PADDING_CONST { CAMERA_FRAME_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_CAMERA_FRAME_H_ */
