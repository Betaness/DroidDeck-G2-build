/*
 * [mali-g2] Occlusion queries the driver keeps answering "not ready" (guards.c). MaliBridge's notes
 * (client/qvisible.c) record a Mali (an "S11 Ultra", Deep Rock Galactic under DXVK) answering
 * 512+ occlusion queries "not ready" for minutes after their submission had finished. Here a run of
 * "not ready" answers for one query, since its last reset, is counted once it lasts past a limit;
 * optionally such a query is then answered as visible, under the conditions of qs_visible_due
 * (behaviour re-implemented, not copied). No Vulkan calls: tests/guards_test.c includes this file
 * alone.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_HYBRIS_QUERY_STUCK_H
#define DROIDDECK_HYBRIS_QUERY_STUCK_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#define QS_LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define QS_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

typedef struct QsQuery {
    uint64_t resetNs; /* last reset, recorded in a command buffer or done on the host */
    uint64_t runNs;   /* first "not ready" of the current run, 0: none */
    uint64_t endNs;   /* last vkCmdEndQuery recorded (tracked for the visible answer only) */
    uint64_t endRec;  /* the recording it was in */
    uint32_t polls;   /* "not ready" answers in the current run */
    uint32_t crossed; /* the current run has lasted past the limit */
} QsQuery;

/* A "not ready" answer at `now`. Returns how long the run has lasted; *crossedNow is set the first
 * time it reaches `limitNs`. */
static inline uint64_t qs_not_ready(QsQuery* q, uint64_t now, uint64_t limitNs, bool* crossedNow) {
    *crossedNow = false;
    uint64_t run = QS_LOAD(&q->runNs);
    if (!run || run < QS_LOAD(&q->resetNs)) {
        QS_STORE(&q->runNs, now);
        QS_STORE(&q->polls, 1);
        QS_STORE(&q->crossed, 0);
        return 0;
    }
    __atomic_add_fetch(&q->polls, 1, __ATOMIC_RELAXED);
    uint64_t lasted = now > run ? now - run : 0;
    if (lasted >= limitNs && !__atomic_exchange_n(&q->crossed, 1, __ATOMIC_RELAXED)) *crossedNow = true;
    return lasted;
}

/* The run ends (the query was answered, or reset). Returns its length when it had passed the limit,
 * else 0. */
static inline uint64_t qs_end_run(QsQuery* q, uint64_t now) {
    uint64_t run = QS_LOAD(&q->runNs);
    if (!run) return 0;
    QS_STORE(&q->runNs, 0);
    if (!__atomic_exchange_n(&q->crossed, 0, __ATOMIC_RELAXED)) return 0;
    return now > run ? now - run : 1;
}

static inline uint64_t qs_reset(QsQuery* q, uint64_t now) {
    QS_STORE(&q->resetNs, now);
    return qs_end_run(q, now);
}

/* One query's result as visible: `samples` and, when asked for, availability 1. False when `size`
 * cannot hold it. */
static inline bool qs_write_visible(void* data, size_t size, VkQueryResultFlags flags, uint64_t samples) {
    bool wide = flags & VK_QUERY_RESULT_64_BIT, avail = flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    size_t one = wide ? 8 : 4, need = one * (avail ? 2 : 1);
    if (!data || size < need) return false;
    if (wide) {
        uint64_t v[2] = {samples, 1};
        memcpy(data, v, need);
    } else {
        uint32_t v[2] = {(uint32_t)samples, 1};
        memcpy(data, v, need);
    }
    return true;
}

/* Recordings, for the visible answer: each vkBeginCommandBuffer starts a numbered recording; a
 * command buffer maps to its current one, a recording to the primary's that executed it
 * (vkCmdExecuteCommands) and its last submission. Both tables are direct-mapped: an entry another
 * one replaced is gone, and a query whose recording is gone is never answered for. */
#define QS_RECS (1u << 16)
#define QS_CBS (1u << 12)

typedef struct QsRec {
    uint64_t rec;      /* 0: free */
    uint64_t parent;   /* a secondary's: the recording of the primary that last executed it */
    uint64_t submitNs; /* a primary's: its last submission */
    uint32_t visible;  /* a primary's: one of its queries was answered as visible */
} QsRec;

typedef struct QsTables {
    pthread_mutex_t lock;
    uint64_t counter;
    QsRec recs[QS_RECS];
    struct { uint64_t cb, rec; } cbs[QS_CBS];
} QsTables;

static inline uint32_t qs_cb_slot(uint64_t cb) {
    return (uint32_t)((cb * 0x9E3779B97F4A7C15ull) >> 52) & (QS_CBS - 1);
}

static inline QsRec* qs_rec_locked(QsTables* t, uint64_t rec) {
    QsRec* r = rec ? &t->recs[rec & (QS_RECS - 1)] : NULL;
    return r && r->rec == rec ? r : NULL;
}

static inline uint64_t qs_current_locked(QsTables* t, uint64_t cb) {
    uint32_t s = qs_cb_slot(cb);
    return t->cbs[s].cb == cb ? t->cbs[s].rec : 0;
}

static inline void qs_begin(QsTables* t, uint64_t cb) {
    pthread_mutex_lock(&t->lock);
    uint64_t rec = ++t->counter;
    uint32_t s = qs_cb_slot(cb);
    t->cbs[s].cb = cb;
    t->cbs[s].rec = rec;
    t->recs[rec & (QS_RECS - 1)] = (QsRec){.rec = rec};
    pthread_mutex_unlock(&t->lock);
}

static inline uint64_t qs_current(QsTables* t, uint64_t cb) {
    pthread_mutex_lock(&t->lock);
    uint64_t rec = qs_current_locked(t, cb);
    pthread_mutex_unlock(&t->lock);
    return rec;
}

static inline void qs_executed(QsTables* t, uint64_t primary, const uint64_t* secondaries, uint32_t count) {
    pthread_mutex_lock(&t->lock);
    uint64_t p = qs_current_locked(t, primary);
    for (uint32_t i = 0; p && i < count; i++) {
        QsRec* s = qs_rec_locked(t, qs_current_locked(t, secondaries[i]));
        if (s) s->parent = p;
    }
    pthread_mutex_unlock(&t->lock);
}

static inline void qs_submitted(QsTables* t, const uint64_t* cbs, uint32_t count, uint64_t now) {
    pthread_mutex_lock(&t->lock);
    for (uint32_t i = 0; i < count; i++) {
        QsRec* r = qs_rec_locked(t, qs_current_locked(t, cbs[i]));
        if (r) r->submitNs = now;
    }
    pthread_mutex_unlock(&t->lock);
}

/* Whether a query still "not ready" at `now` is answered as visible: its last End was recorded
 * after its last reset, in a primary recording submitted after that End and at least `limitNs` ago
 * (or in a secondary such a primary executed), and the query has been polled "not ready" for
 * `limitNs`, or another query of the same primary recording was answered as visible already (a
 * game waiting on hundreds of them one after another would wait `limitNs` for each). On true,
 * *primary is the primary recording's number for qs_mark_visible. */
static inline bool qs_visible_due(QsTables* t, const QsQuery* q, uint64_t now, uint64_t limitNs, uint64_t* primary) {
    uint64_t endRec = QS_LOAD(&q->endRec), endNs = QS_LOAD(&q->endNs), run = QS_LOAD(&q->runNs);
    if (!endRec || endNs < QS_LOAD(&q->resetNs) || !run) return false;
    bool due = false;
    pthread_mutex_lock(&t->lock);
    QsRec* e = qs_rec_locked(t, endRec);
    QsRec* p = !e ? NULL : e->submitNs ? e : qs_rec_locked(t, e->parent);
    if (p && p->submitNs >= endNs && now >= p->submitNs && now - p->submitNs >= limitNs &&
        (p->visible || (now >= run && now - run >= limitNs))) {
        due = true;
        *primary = p->rec;
    }
    pthread_mutex_unlock(&t->lock);
    return due;
}

static inline void qs_mark_visible(QsTables* t, uint64_t primary) {
    pthread_mutex_lock(&t->lock);
    QsRec* p = qs_rec_locked(t, primary);
    if (p) p->visible = 1;
    pthread_mutex_unlock(&t->lock);
}

#endif
