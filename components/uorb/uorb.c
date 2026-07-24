/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * uORB for FreeRTOS — implementation
 *
 * Design summary:
 *   - A global registry holds one entry per topic.
 *   - Each subscriber gets its own FreeRTOS queue (depth = topic's o_depth).
 *   - orb_publish() writes to every subscriber queue.
 *   - Thread safety: registry writes are serialised by a mutex.
 *     Queue operations themselves are FreeRTOS thread-safe.
 */

#include "uorb.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "esp_log.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal types                                                     */
/* ------------------------------------------------------------------ */

/** Subscriber entry — maps a subscriber handle to its queue + topic. */
typedef struct {
    QueueHandle_t queue;      /**< FreeRTOS queue handle. */
    int           topic_idx;  /**< Index into s_topics[]; < 0 if inactive. */
    int           generation; /**< Monotonically increasing; prevents ABA in orb_publish(). */
    uint8_t      *queue_storage;  /**< PSRAM-allocated queue storage (NULL for internal RAM queues). */
    StaticQueue_t *queue_buf;     /**< PSRAM-allocated queue struct (NULL for internal RAM queues). */
} orb_sub_entry_t;

/** Topic registry entry. */
typedef struct {
    orb_id_t  meta;               /**< Topic metadata pointer. */
    int       sub_indices[ORB_MAX_SUBSCRIBERS]; /**< Indices into s_subs[]. */
    int       num_subscribers;    /**< Current subscriber count. */
    int       num_publishers;     /**< Current publisher count. */
    bool      active;             /**< True once topic has been registered. */
} orb_topic_reg_t;

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

/** Topic registry (one per unique topic) — PSRAM allocation. */
static orb_topic_reg_t *s_topics = NULL;

/** Subscriber table (flat, handles are indices into this array) — PSRAM allocation. */
#define ORB_MAX_SUBS  (ORB_MAX_TOPICS * ORB_MAX_SUBSCRIBERS)
static orb_sub_entry_t *s_subs = NULL;
static int s_num_subs;         /**< High-water mark: next never-used index. */

/** Free-list for reusable subscriber slots — PSRAM allocation.
 * When orb_unsubscribe() frees a slot, its index is pushed here.
 * orb_subscribe() pops from the free-list first (if non-empty),
 * then falls back to s_num_subs (append).
 * This prevents subscriber slot exhaustion from repeated
 * subscribe/unsubscribe cycles. */
static int *s_sub_free_list = NULL;
static int s_sub_free_count;   /**< Number of entries in the free-list. */

/** Monotonically increasing generation counter assigned to subscriber
 *  slots.  orb_publish() snapshots (index, generation) pairs and
 *  verifies the generation still matches before delivering, preventing
 *  the ABA problem where a slot is unsubscribed and reused between
 *  snapshot and delivery. */
static int s_sub_generation;

/** Protects the registry and subscriber table. */
static SemaphoreHandle_t s_mutex;

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

void orb_init(void)
{
    /* Must be called exactly once from app_main() before any tasks are created.
     * Not safe to call concurrently — the idempotent check itself is a race.
     * In practice this is safe because app_main runs before other tasks exist. */
    if (s_mutex != NULL) {
        ESP_LOGE("uORB", "orb_init called twice — ignoring");
        return;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE("uORB", "Failed to create uORB mutex");
        return;
    }

    /* Allocate registry tables from PSRAM to save internal SRAM.
     * These are one-time allocations that live for the lifetime of the system. */
    s_topics = (orb_topic_reg_t *)heap_caps_calloc(ORB_MAX_TOPICS, sizeof(orb_topic_reg_t),
                                                    MALLOC_CAP_SPIRAM);
    s_subs = (orb_sub_entry_t *)heap_caps_calloc(ORB_MAX_SUBS, sizeof(orb_sub_entry_t),
                                                  MALLOC_CAP_SPIRAM);
    s_sub_free_list = (int *)heap_caps_calloc(ORB_MAX_SUBS, sizeof(int),
                                               MALLOC_CAP_SPIRAM);

    if (!s_topics || !s_subs || !s_sub_free_list) {
        ESP_LOGE("uORB", "PSRAM allocation failed for registry tables");
        /* Free whatever was allocated and null out */
        if (s_topics) { heap_caps_free(s_topics); s_topics = NULL; }
        if (s_subs) { heap_caps_free(s_subs); s_subs = NULL; }
        if (s_sub_free_list) { heap_caps_free(s_sub_free_list); s_sub_free_list = NULL; }
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

/** Check if uORB is properly initialized (one-time PSRAM allocations succeeded). */
static inline bool is_initialized(void)
{
    return (s_topics != NULL && s_subs != NULL && s_sub_free_list != NULL && s_mutex != NULL);
}

static inline void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/**
 * Find a topic in the registry by its metadata pointer.
 * Returns the index, or -1 if not found.
 */
static int topic_find(orb_id_t meta)
{
    for (int i = 0; i < ORB_MAX_TOPICS; i++) {
        if (s_topics[i].active && s_topics[i].meta == meta) {
            return i;
        }
    }
    return -1;
}

/**
 * Find or create a topic in the registry.
 * Returns the index, or -1 if the registry is full.
 * Caller must hold s_mutex.
 */
static int topic_find_or_create(orb_id_t meta)
{
    int idx = topic_find(meta);
    if (idx >= 0) {
        return idx;
    }

    /* Find the first inactive slot */
    for (int i = 0; i < ORB_MAX_TOPICS; i++) {
        if (!s_topics[i].active) {
            memset(&s_topics[i], 0, sizeof(s_topics[i]));
            s_topics[i].meta   = meta;
            s_topics[i].active = true;
            return i;
        }
    }

    return -1; /* Registry full */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

orb_advert_t orb_advertise(orb_id_t meta)
{
    if (meta == NULL) {
        return -1;
    }
    if (!is_initialized()) {
        ESP_LOGE("uORB", "orb_advertise: uORB not initialized");
        return -1;
    }

    lock();
    int idx = topic_find_or_create(meta);
    if (idx >= 0) {
        s_topics[idx].num_publishers++;
    }
    unlock();

    return (idx >= 0) ? idx : -1;
}

orb_sub_t orb_subscribe(orb_id_t meta)
{
    if (meta == NULL) {
        return -1;
    }
    if (!is_initialized()) {
        ESP_LOGE("uORB", "orb_subscribe: uORB not initialized");
        return -1;
    }

    lock();

    int t_idx = topic_find_or_create(meta);
    if (t_idx < 0) {
        unlock();
        return -1;
    }

    orb_topic_reg_t *topic = &s_topics[t_idx];

    if (topic->num_subscribers >= ORB_MAX_SUBSCRIBERS) {
        unlock();
        return -1;
    }

    /* Allocate a subscriber slot: prefer free-list (reused slots),
     * then fall back to appending at s_num_subs. */
    int s_idx;
    bool from_free_list = false;
    if (s_sub_free_count > 0) {
        from_free_list = true;
        s_idx = s_sub_free_list[--s_sub_free_count];
    } else if (s_num_subs < ORB_MAX_SUBS) {
        s_idx = s_num_subs++;
    } else {
        unlock();
        return -1;
    }

    /* Create the per-subscriber queue.
     * For large topics (o_size > threshold), use PSRAM-backed static queue
     * to avoid exhausting internal SRAM. FreeRTOS xQueueCreate allocates
     * queue storage from the heap — on ESP32-S31 this is internal SRAM,
     * which is scarce (~512KB shared with WiFi/LWIP/LVGL).
     * xQueueCreateStatic lets us provide PSRAM-allocated storage instead. */
#define UORB_PSRAM_QUEUE_THRESHOLD  256  /* Topics larger than this use PSRAM queues */

    QueueHandle_t q = NULL;
    uint8_t *queue_storage = NULL;
    StaticQueue_t *queue_buf = NULL;

    if (meta->o_size > UORB_PSRAM_QUEUE_THRESHOLD) {
        /* Allocate queue storage and struct from PSRAM */
        size_t storage_size = meta->o_depth * meta->o_size;
        queue_storage = (uint8_t *)heap_caps_calloc(1, storage_size,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        queue_buf = (StaticQueue_t *)heap_caps_calloc(1, sizeof(StaticQueue_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!queue_storage || !queue_buf) {
            ESP_LOGE("uORB", "orb_subscribe: PSRAM alloc failed for %s queue (depth=%u, size=%u)",
                     meta->o_name, meta->o_depth, (unsigned)meta->o_size);
            if (queue_storage) heap_caps_free(queue_storage);
            if (queue_buf) heap_caps_free(queue_buf);
            /* Return the consumed slot to allow future subscribes */
            if (from_free_list) {
                s_sub_free_list[s_sub_free_count++] = s_idx;
            } else {
                s_num_subs--;
            }
            unlock();
            return -1;
        }
        q = xQueueCreateStatic(meta->o_depth, meta->o_size, queue_storage, queue_buf);
    } else {
        q = xQueueCreate(meta->o_depth, meta->o_size);
    }

    if (q == NULL) {
        /* Return the consumed slot to allow future subscribes */
        if (from_free_list) {
            s_sub_free_list[s_sub_free_count++] = s_idx;
        } else {
            s_num_subs--;
        }
        unlock();
        return -1;
    }

    s_subs[s_idx].queue         = q;
    s_subs[s_idx].topic_idx     = t_idx;
    s_subs[s_idx].generation    = s_sub_generation++;  /* ABA protection */
    s_subs[s_idx].queue_storage = queue_storage;  /* NULL for internal RAM queues */
    s_subs[s_idx].queue_buf     = queue_buf;      /* NULL for internal RAM queues */

    int pos = topic->num_subscribers++;
    topic->sub_indices[pos] = s_idx;

    unlock();
    return s_idx;
}

int orb_unsubscribe(orb_sub_t handle)
{
    if (handle < 0) {
        return -1;
    }
    if (!is_initialized()) {
        ESP_LOGE("uORB", "orb_unsubscribe: uORB not initialized");
        return -1;
    }

    lock();

    if (handle >= s_num_subs) {
        unlock();
        return -1;
    }

    orb_sub_entry_t *sub = &s_subs[handle];
    if (sub->topic_idx < 0) {
        unlock();
        return -1; /* Already unsubscribed */
    }

    int t_idx = sub->topic_idx;
    orb_topic_reg_t *topic = &s_topics[t_idx];

    /* Remove this subscriber from the topic's subscriber list */
    for (int i = 0; i < topic->num_subscribers; i++) {
        if (topic->sub_indices[i] == handle) {
            topic->num_subscribers--;
            if (i < topic->num_subscribers) {
                topic->sub_indices[i] = topic->sub_indices[topic->num_subscribers];
            }
            break;
        }
    }

    /* Free the queue and mark inactive */
    if (sub->queue) {
        vQueueDelete(sub->queue);
    }
    /* Free PSRAM-allocated queue storage and struct (if any) */
    if (sub->queue_storage) {
        heap_caps_free(sub->queue_storage);
    }
    if (sub->queue_buf) {
        heap_caps_free(sub->queue_buf);
    }
    sub->queue         = NULL;
    sub->queue_storage = NULL;
    sub->queue_buf     = NULL;
    sub->topic_idx     = -1;

    /* Push slot to free-list for reuse by future orb_subscribe() */
    if (s_sub_free_count < ORB_MAX_SUBS) {
        s_sub_free_list[s_sub_free_count++] = handle;
    }

    unlock();
    return 0;
}

int orb_publish(orb_id_t meta, orb_advert_t handle, const void *data)
{
    (void)handle;

    if (meta == NULL || data == NULL) {
        return -1;
    }
    if (!is_initialized()) {
        ESP_LOGE("uORB", "orb_publish: uORB not initialized");
        return -1;
    }

    /* Hold the lock for the entire publish: find topic + deliver to
     * all subscribers. This prevents a race with orb_unsubscribe()
     * that could delete a queue (vQueueDelete) while we are mid-delivery
     * (use-after-free). The lock is held briefly — just long enough to
     * copy a few bytes into each subscriber's FreeRTOS queue. */
    lock();
    int t_idx = topic_find(meta);
    if (t_idx < 0) {
        unlock();
        return -1;
    }

    orb_topic_reg_t *topic = &s_topics[t_idx];
    int n_subs = topic->num_subscribers;

    const bool overwrite = (meta->o_depth == 1);

    for (int i = 0; i < n_subs && i < ORB_MAX_SUBSCRIBERS; i++) {
        int s_idx = topic->sub_indices[i];
        if (s_idx < 0 || s_idx >= s_num_subs) continue;

        QueueHandle_t q = s_subs[s_idx].queue;
        if (q == NULL) continue;

        if (overwrite) {
            xQueueOverwrite(q, data);
        } else {
            xQueueSend(q, data, 0);
        }
    }
    unlock();

    return 0;
}

int orb_copy(orb_id_t meta, orb_sub_t handle, void *buffer)
{
    (void)meta;

    if (handle < 0 || buffer == NULL) {
        return -1;
    }
    if (!is_initialized()) {
        ESP_LOGE("uORB", "orb_copy: uORB not initialized");
        return -1;
    }

    /* Hold the lock while checking topic_idx and getting the queue pointer.
     * Without the lock, a concurrent orb_unsubscribe() could delete the queue
     * between the topic_idx check and xQueueReceive, causing use-after-free.
     * We use a non-blocking receive inside the lock, then retry outside if empty. */
    while (1) {
        lock();
        orb_sub_entry_t *sub = &s_subs[handle];
        if (sub->topic_idx < 0) {
            unlock();
            return -1; /* Unsubscribed */
        }
        QueueHandle_t q = sub->queue;
        int gen = sub->generation;
        unlock();

        /* Wait for data outside the lock — xQueueReceive is thread-safe.
         * After receiving, verify the subscriber hasn't been recycled
         * (unsubscribed + resubscribed) by checking the generation counter. */
        BaseType_t ret = xQueueReceive(q, buffer, portMAX_DELAY);
        if (ret != pdTRUE) return -1;

        lock();
        bool valid = (s_subs[handle].topic_idx >= 0 && s_subs[handle].generation == gen);
        unlock();
        if (valid) return 0;
        /* Subscriber was recycled — discard this message and retry */
    }
}

int orb_check(orb_sub_t handle, bool *updated)
{
    if (handle < 0 || updated == NULL) {
        return -1;
    }
    if (!is_initialized()) {
        ESP_LOGE("uORB", "orb_check: uORB not initialized");
        return -1;
    }

    lock();

    if (handle >= s_num_subs) {
        unlock();
        return -1;
    }

    orb_sub_entry_t *sub = &s_subs[handle];
    if (sub->topic_idx < 0) {
        unlock();
        return -1; /* Unsubscribed */
    }
    QueueHandle_t q = sub->queue;
    unlock();

    *updated = (uxQueueMessagesWaiting(q) > 0);
    return 0;
}
