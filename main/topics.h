#pragma once

#include "generated/wifi_state.h"
#include "generated/volume_state.h"
#include "generated/audio_level.h"
#include "generated/camera_state.h"
#include "generated/audio_frame.h"
#include "generated/camera_frame.h"
#include "generated/fps_stats.h"
#include "generated/recording_state.h"
#include "generated/ulog_state.h"
#include "generated/system_stats.h"
#include "generated/system_alert.h"
#include "generated/uORBTopics.hpp"

/* Forward-declare uORB topic metadata for all topics */
ORB_TOPIC_DECLARE(wifi_state);
ORB_TOPIC_DECLARE(volume_state);
ORB_TOPIC_DECLARE(audio_level);
ORB_TOPIC_DECLARE(camera_state);
ORB_TOPIC_DECLARE(audio_frame);
ORB_TOPIC_DECLARE(camera_frame);
ORB_TOPIC_DECLARE(fps_stats);
ORB_TOPIC_DECLARE(recording_state);
ORB_TOPIC_DECLARE(ulog_state);
ORB_TOPIC_DECLARE(system_stats);
ORB_TOPIC_DECLARE(system_alert);