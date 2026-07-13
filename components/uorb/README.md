/**
 * @example uorb_usage_example.c
 *
 * Minimal example showing how to use uORB for inter-task communication.
 *
 * Publisher task:
 *   orb_advertise() once → orb_publish() in loop
 *
 * Subscriber task:
 *   orb_subscribe() once → orb_copy() in loop (blocking)
 *     or: orb_check() → orb_copy() (non-blocking poll)
 */

#include "uorb.h"
#include "topics.h"

/* ========================= PUBLISHER ========================= */

static orb_advert_t s_fps_pub;   /* stored globally or in class */

void publisher_init(void)
{
    /* One-time: advertise the topic */
    s_fps_pub = orb_advertise(ORB_ID(fps_stats));
}

void publisher_loop(void)
{
    struct fps_stats_s fps;
    fps.frame_count     = 1234;
    fps.fps_total_bytes = 65536;
    fps.fps             = 9.5f;

    /* Publish — all subscribers get a copy */
    orb_publish(ORB_ID(fps_stats), s_fps_pub, &fps);
}

/* ======================= SUBSCRIBER ========================= */

static orb_sub_t s_fps_sub;   /* stored globally or in class */

void subscriber_init(void)
{
    /* One-time: subscribe */
    s_fps_sub = orb_subscribe(ORB_ID(fps_stats));
}

void subscriber_loop_blocking(void)
{
    struct fps_stats_s fps;

    /* Block until new data arrives */
    orb_copy(ORB_ID(fps_stats), s_fps_sub, &fps);

    /* Use fps.frame_count, fps.fps ... */
}

void subscriber_loop_nonblocking(void)
{
    bool updated;
    if (orb_check(s_fps_sub, &updated) == 0 && updated) {
        struct fps_stats_s fps;
        orb_copy(ORB_ID(fps_stats), s_fps_sub, &fps);
        /* Use fps data ... */
    }
}

/* ===================== CLEANUP ========================= */

void subscriber_cleanup(void)
{
    orb_unsubscribe(s_fps_sub);
}
