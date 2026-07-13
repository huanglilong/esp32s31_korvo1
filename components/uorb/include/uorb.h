/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * uORB for FreeRTOS — Lightweight publish/subscribe message bus.
 * Ported from PX4 uORB API semantics, using FreeRTOS queues for
 * thread-safe inter-task communication.
 *
 * Each topic has a fixed message type (C struct) and a configurable
 * queue depth per subscriber. Subscribers that are slow will
 * drop messages (depth=1 always gets latest; depth>1 buffers).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Topic metadata                                                     */
/* ------------------------------------------------------------------ */

/**
 * Topic metadata — describes a uORB topic.
 * Defined with ORB_TOPIC_DEFINE() in a .c file;
 * referenced with ORB_ID() from other files.
 */
typedef struct orb_metadata {
    const char *o_name;             /**< Unique topic name */
    const char *o_format;           /**< ULog format string (e.g. "topic:uint64_t field;float field2;") */
    size_t      o_size;             /**< Size of the message struct (bytes) */
    size_t      o_size_no_padding;  /**< Size without trailing _padding (for ULog writer). Matches PX4 convention. */
    uint8_t     o_depth;            /**< Queue depth per subscriber (1 = latest-only) */
} orb_metadata_t;

typedef const orb_metadata_t *orb_id_t;

/* ------------------------------------------------------------------ */
/*  Handle types                                                       */
/* ------------------------------------------------------------------ */

/** Publisher handle.  < 0 means invalid. */
typedef int orb_advert_t;

/** Invalid publisher handle constant. */
#define ORB_ADVERT_INVALID (-1)

/** Subscriber handle.  < 0 means invalid. */
typedef int orb_sub_t;

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
/* ------------------------------------------------------------------ */

/** Maximum number of distinct topics. */
#define ORB_MAX_TOPICS      32
/** Maximum subscribers per topic. */
#define ORB_MAX_SUBSCRIBERS 8

/* ------------------------------------------------------------------ */
/*  Topic definition macros                                            */
/* ------------------------------------------------------------------ */

/**
 * Define a uORB topic (place in a .c file, e.g. topics.c).
 *
 * @param name              Topic identifier (used with ORB_ID())
 * @param msg_type          C struct type of the message
 * @param depth             Queue depth per subscriber (1 = latest-only, discards old)
 * @param format_str        ULog format string (e.g. "topic:uint64_t field;float field2;")
 * @param padding_end_size  Trailing _padding bytes in the struct (for o_size_no_padding)
 */
#define ORB_TOPIC_DEFINE(name, msg_type, depth, format_str, padding_end_size) \
    extern const orb_metadata_t g_orb_meta_##name                          \
        __attribute__((used)) = {                                          \
        .o_name             = #name,                                       \
        .o_format           = (format_str),                                \
        .o_size             = sizeof(msg_type),                            \
        .o_size_no_padding  = sizeof(msg_type) - (padding_end_size),       \
        .o_depth            = (depth),                                     \
    }

/**
 * Obtain a pointer to a topic's metadata (for use with the uORB API).
 */
#define ORB_ID(name) (&g_orb_meta_##name)

/**
 * Forward-declare a topic's metadata (use in headers).
 */
#define ORB_TOPIC_DECLARE(name)                                            \
    extern const orb_metadata_t g_orb_meta_##name

/* ------------------------------------------------------------------ */
/*  Core API                                                           */
/* ------------------------------------------------------------------ */

/**
 * Advertise as a publisher of a topic.
 * Must be called at least once before the first orb_publish().
 * Calling multiple times is safe (only the first call matters).
 *
 * @param meta  Topic metadata (use ORB_ID(name))
 * @return >= 0 publisher handle on success; < 0 on error.
 */
orb_advert_t orb_advertise(orb_id_t meta);

/**
 * Subscribe to a topic.
 * Each subscriber gets an independent queue.
 *
 * @param meta  Topic metadata (use ORB_ID(name))
 * @return >= 0 subscriber handle on success; < 0 on error.
 */
orb_sub_t orb_subscribe(orb_id_t meta);

/**
 * Unsubscribe from a topic and free the subscriber's queue.
 *
 * @param handle  Subscriber handle from orb_subscribe().
 * @return 0 on success; < 0 on error.
 */
int orb_unsubscribe(orb_sub_t handle);

/**
 * Publish data to a topic.
 * Every subscriber receives a copy of the message.
 *
 * - If o_depth == 1: the subscriber's queue is overwritten with
 *   the latest message (readers never see stale data).
 * - If o_depth  > 1: the message is enqueued; a full queue causes
 *   the newest message to be dropped (non-blocking).
 *
 * @param meta    Topic metadata.
 * @param handle  Publisher handle (from orb_advertise); unused currently.
 * @param data    Pointer to the message to publish.
 * @return 0 on success; < 0 on error.
 */
int orb_publish(orb_id_t meta, orb_advert_t handle, const void *data);

/**
 * Copy the latest message from a subscription (blocking).
 *
 * Blocks indefinitely until new data is available.  For a non-blocking
 * check, call orb_check() first.
 *
 * @param meta    Topic metadata.
 * @param handle  Subscriber handle.
 * @param buffer  Destination buffer (must be >= o_size bytes).
 * @return 0 on success; < 0 on error.
 */
int orb_copy(orb_id_t meta, orb_sub_t handle, void *buffer);

/**
 * Check whether a subscription has new data (non-blocking).
 *
 * @param handle   Subscriber handle.
 * @param updated  [out] true if new data is available.
 * @return 0 on success; < 0 on error.
 */
int orb_check(orb_sub_t handle, bool *updated);

/**
 * Initialize the uORB subsystem (must be called once before any other API).
 *
 * Creates the global mutex eagerly, eliminating the race condition
 * that would occur if two threads simultaneously called any uORB
 * function and both attempted to lazily create the mutex.
 */
void orb_init(void);

#ifdef __cplusplus
}
#endif
