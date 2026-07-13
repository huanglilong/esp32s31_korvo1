/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "uORBTopics.hpp"

ORB_TOPIC_DECLARE(audio_level);
ORB_TOPIC_DECLARE(camera_frame);
ORB_TOPIC_DECLARE(camera_state);
ORB_TOPIC_DECLARE(fps_stats);
ORB_TOPIC_DECLARE(recording_state);
ORB_TOPIC_DECLARE(system_alert);
ORB_TOPIC_DECLARE(system_stats);
ORB_TOPIC_DECLARE(ulog_state);
ORB_TOPIC_DECLARE(volume_state);
ORB_TOPIC_DECLARE(wifi_state);

static const struct orb_metadata *const s_topics[ORB_TOPICS_COUNT] = {
    ORB_ID(audio_level),
    ORB_ID(camera_frame),
    ORB_ID(camera_state),
    ORB_ID(fps_stats),
    ORB_ID(recording_state),
    ORB_ID(system_alert),
    ORB_ID(system_stats),
    ORB_ID(ulog_state),
    ORB_ID(volume_state),
    ORB_ID(wifi_state),
};

const struct orb_metadata *const *orb_get_topics()
{
    return s_topics;
}

const struct orb_metadata *get_orb_meta(ORB_ID id)
{
    if (static_cast<uint8_t>(id) < ORB_TOPICS_COUNT) {
        return s_topics[static_cast<uint8_t>(id)];
    }
    return nullptr;
}
