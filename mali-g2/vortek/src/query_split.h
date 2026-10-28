/* [mali-g2] Query commands out of the application's command pools (VORTEK_QUERY_SPLIT=1, winlator-0023).
 *
 * The Mali-G2 driver keeps ~420 bytes for every vkCmdBeginRendering recorded into command pool memory
 * that has held a query (cmd_memory.h), and keeps what it took even after the pool releases its memory:
 * released, trimmed or destroyed pools hand it to a cache of the device that is reused but never given
 * back to malloc (tools/probe/cmdmem in the private repository: after 37 MB leaked, a release or a
 * destroy frees 0.2 MB; the next 3000 leaking frames grow the heap by 0.2 MB, the 3000 after them by
 * 6.7 KB a frame). Releasing by an estimate (cmd_memory.h) therefore only bounds the peak, and on the
 * device the Vortek server still grew by 13 MB a minute with it on Duckov (2026-10-28, R17L).
 *
 * This takes the query commands that need no command buffer state, vkCmdWriteTimestamp(2),
 * vkCmdResetQueryPool and vkCmdCopyQueryPoolResults, out of primary command buffers when they come
 * before the recording's first other command ("lead": Duckov's DXVK records two timestamps at the start
 * of 14% of its command buffers and command buffers with nothing but two timestamps in 41%) or after its
 * last other command ("trail"), into a command buffer of a pool of the server's own, submitted right
 * before (lead) or after (trail) the application's in every vkQueueSubmit / vkQueueSubmit2. Submission
 * order is execution order across command buffers of a batch, so the timestamps, resets and copies
 * mean what they meant; the application's pool never holds a query and keeps nothing. Queries between
 * other commands, inside rendering, begun queries and queries in secondary command buffers stay where
 * they are (the estimate then covers them).
 *
 * Tables: per connection, single-threaded (the connection's requests run in order on its thread).
 * Open addressing on the handle, at most QS_PROBES probes; a full table leaves a pool or a command
 * buffer unsplit. A side command buffer's contents count only for the recording it was begun for
 * (`recording` == the application's buffer's recording number) and only while its side pool was not
 * reset since (`generation`). */
#ifndef DROIDDECK_QUERY_SPLIT_H
#define DROIDDECK_QUERY_SPLIT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define QS_POOL_BITS 10
#define QS_BUFFER_BITS 13
#define QS_PROBES 64
#define QS_EMPTY 0ull
#define QS_GONE UINT64_MAX

enum { QS_LEAD = 0, QS_TRAIL = 1 };

typedef struct QsPool {
    uint64_t key;         /* the application's VkCommandPool */
    uint64_t device;
    uint64_t side;        /* the server's VkCommandPool for the queries, 0 until needed */
    uint32_t family;
    uint32_t generation;  /* bumped by every reset of the application's pool (the side pool is reset with it) */
    bool failed;          /* creating the side pool failed: never split this pool */
} QsPool;

typedef struct QsBuffer {
    uint64_t key;          /* the application's VkCommandBuffer */
    uint32_t pool;         /* index in QuerySplit.pools */
    uint32_t recording;    /* bumped by every vkBeginCommandBuffer */
    uint64_t side[2];      /* QS_LEAD / QS_TRAIL command buffers from the side pool, 0 until needed */
    uint32_t sideRecording[2], sideGeneration[2];
    bool sideOpen[2];      /* begun and not yet ended */
    bool primary;
    bool seenOther;        /* the current recording has a command other than a movable query command */
    uint32_t usage;        /* VkCommandBufferUsageFlags of the current recording */
} QsBuffer;

typedef struct QuerySplit {
    QsPool pools[1u << QS_POOL_BITS];
    QsBuffer buffers[1u << QS_BUFFER_BITS];
    uint64_t moved[2], recordings[2], submitsExpanded, fullLogged;
    int log;
} QuerySplit;

static inline uint32_t qs_hash(uint64_t key, int bits) {
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

static inline QsPool* qs_pool(QuerySplit* q, uint64_t key) {
    if (!q || !key) return NULL;
    uint32_t mask = (1u << QS_POOL_BITS) - 1, i = qs_hash(key, QS_POOL_BITS);
    for (int probe = 0; probe < QS_PROBES; probe++, i = (i + 1) & mask) {
        if (q->pools[i].key == key) return &q->pools[i];
        if (q->pools[i].key == QS_EMPTY) break;
    }
    return NULL;
}

static inline QsBuffer* qs_buffer(QuerySplit* q, uint64_t key) {
    if (!q || !key) return NULL;
    uint32_t mask = (1u << QS_BUFFER_BITS) - 1, i = qs_hash(key, QS_BUFFER_BITS);
    for (int probe = 0; probe < QS_PROBES; probe++, i = (i + 1) & mask) {
        if (q->buffers[i].key == key) return &q->buffers[i];
        if (q->buffers[i].key == QS_EMPTY) break;
    }
    return NULL;
}

static inline QsPool* qs_pool_insert(QuerySplit* q, uint64_t key) {
    uint32_t mask = (1u << QS_POOL_BITS) - 1, i = qs_hash(key, QS_POOL_BITS);
    for (int probe = 0; probe < QS_PROBES; probe++, i = (i + 1) & mask)
        if (q->pools[i].key == QS_EMPTY || q->pools[i].key == QS_GONE) {
            QsPool* p = &q->pools[i];
            uint32_t generation = p->generation + 1; /* stale buffers of an earlier pool here stay stale */
            *p = (QsPool){0};
            p->key = key;
            p->generation = generation;
            return p;
        }
    return NULL;
}

static inline QsBuffer* qs_buffer_insert(QuerySplit* q, uint64_t key) {
    uint32_t mask = (1u << QS_BUFFER_BITS) - 1, i = qs_hash(key, QS_BUFFER_BITS);
    for (int probe = 0; probe < QS_PROBES; probe++, i = (i + 1) & mask)
        if (q->buffers[i].key == QS_EMPTY || q->buffers[i].key == QS_GONE) {
            QsBuffer* b = &q->buffers[i];
            *b = (QsBuffer){0};
            b->key = key;
            return b;
        }
    return NULL;
}

/* Whether a request code is a query command that needs no command buffer state (movable). */
static inline bool qs_movable(int requestCode, int writeTimestamp, int writeTimestamp2, int resetQueryPool, int copyQueryPoolResults) {
    return requestCode == writeTimestamp || requestCode == writeTimestamp2 || requestCode == resetQueryPool ||
           requestCode == copyQueryPoolResults;
}

/* Whether a side command buffer holds commands of the buffer's current recording, in its pool's current
 * generation (what a submit of the buffer must include). */
static inline bool qs_side_live(QuerySplit* q, QsBuffer* b, int which) {
    return b->side[which] && b->sideRecording[which] == b->recording && b->sideGeneration[which] == q->pools[b->pool].generation;
}

#endif
