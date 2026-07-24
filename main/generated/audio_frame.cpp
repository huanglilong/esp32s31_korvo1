/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "audio_frame.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(audio_frame, audio_frame_s, 4, "audio_frame:uint64_t timestamp;uint32_t frame_index;uint16_t sample_rate;uint8_t channel;uint8_t bits_per_sample;uint16_t aac_size;uint8_t[1024] aac_data;uint8_t[6] _padding0;", 6);
