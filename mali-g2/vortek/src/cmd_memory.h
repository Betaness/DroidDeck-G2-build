/* [mali-g2] When to release the Mali's command buffer memory: an estimate of what the driver keeps per
 * command pool, shared by the Vortek server (winlator-0018) and the in-process path
 * (../../hybris/src/mali_fixes.c).
 *
 * The Mali-G2 driver (r56p0) keeps host memory for every vkCmdBeginRendering recorded into a command
 * buffer that also writes a query, until that command buffer is reset with RELEASE_RESOURCES (or its
 * pool is reset with RELEASE_RESOURCES, or freed, or destroyed); resets with flags 0, and the implicit
 * reset of vkBeginCommandBuffer, never give it back. tests/frame_leak_test.c measures 415-421 bytes per
 * vkCmdBeginRendering, independent of the draws in it and of the number and kind of queries
 * (vkCmdWriteTimestamp, vkCmdWriteTimestamp2, vkCmdBeginQuery occlusion, inside or outside the
 * rendering, reset with vkCmdResetQueryPool or vkResetQueryPool). Not affected: command buffers whose
 * only query command is vkCmdResetQueryPool, rendering in a different command buffer from the query,
 * and render pass instances of a VkRenderPass (vkCmdBeginRenderPass, vkCmdBeginRenderPass2).
 *
 * So per command buffer the current recording counts its vkCmdBeginRendering and whether it wrote a
 * query; at vkEndCommandBuffer, if it did both, renderings x CMDMEM_RENDERING_BYTES is added to the
 * buffer's and its pool's estimate. A reset releases when its pool's estimate reaches `perPool`
 * bytes, or when the estimate over all pools reaches `total` and this pool (command buffer) holds
 * some: a pool reset clears the pool (a generation number makes its buffers' shares stale), a command
 * buffer reset clears that buffer's share. A pool that never had a query with rendering never releases.
 * Backstop for leaks the estimate does not know about: a pool or command buffer whose recordings
 * combined a query with a vkCmdBeginRenderPass(2) releases at its CMDMEM_BACKSTOP_RESETS-th reset.
 *
 * Tables: open addressing on the handle, fixed size, at most CMDMEM_PROBES probes. Lookups take no
 * lock (keys are read atomically; an entry's fields are only touched under the external
 * synchronization Vulkan requires for the command pool: recording into, beginning, ending, resetting
 * and freeing its command buffers, resetting and destroying it); inserts and removals take `lock`.
 * The sum over pools is atomic. When a table is full, everything falls back to releasing every
 * CMDMEM_FALLBACK_EVERY-th reset (the previous fixed policy), logged once.
 *
 * The includer defines CMDMEM_LOG(fmt, ...) (one line, no newline). */
#ifndef DROIDDECK_CMD_MEMORY_H
#define DROIDDECK_CMD_MEMORY_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CMDMEM_RENDERING_BYTES 420u
#define CMDMEM_POOL_BITS 10
#define CMDMEM_BUFFER_BITS 13
#define CMDMEM_PROBES 64
#define CMDMEM_BACKSTOP_RESETS 1024u
#define CMDMEM_FALLBACK_EVERY 32u
#define CMDMEM_DEFAULT_PER_POOL (16ull << 20)
#define CMDMEM_DEFAULT_TOTAL(perPool) ((perPool) * 2)
#define CMDMEM_EMPTY 0ull
#define CMDMEM_GONE UINT64_MAX

typedef struct CmdMemPool {
    uint64_t key;        /* VkCommandPool; CMDMEM_EMPTY / CMDMEM_GONE */
    uint64_t device;
    uint64_t retained;   /* estimated bytes the driver keeps for the pool's command buffers */
    uint32_t generation; /* bumped by every release of the whole pool */
    uint32_t resets;     /* pool resets since the last release */
    bool resetBuffer;    /* created with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT */
    bool suspect;        /* a recording combined a query with a VkRenderPass instance (backstop) */
} CmdMemPool;

typedef struct CmdMemBuffer {
    uint64_t key;        /* VkCommandBuffer */
    uint64_t retained;   /* this buffer's part of its pool's estimate, valid for `generation` */
    uint32_t pool;       /* index in CmdMem.pools */
    uint32_t generation;
    uint32_t renderings; /* vkCmdBeginRendering in the current recording */
    uint32_t resets;     /* resets of this buffer since it last released (backstop) */
    bool query;          /* the current recording wrote a query */
    bool renderPass;     /* the current recording began a VkRenderPass instance */
    bool suspect;
} CmdMemBuffer;

typedef struct CmdMem {
    pthread_mutex_t lock; /* inserts and removals */
    uint64_t perPool, total;
    uint64_t retained;    /* atomic: sum of the pools' estimates */
    uint64_t releases, releasedBytes; /* atomic, for the log */
    uint32_t fallbackResets;          /* atomic */
    int full;                         /* atomic: a table overflowed, every pool releases every 32nd reset */
    int logEvery;                     /* log every release (1), the first ones and then every 64th (0) */
    CmdMemPool pools[1u << CMDMEM_POOL_BITS];
    CmdMemBuffer buffers[1u << CMDMEM_BUFFER_BITS];
} CmdMem;

/* "16m", "512k", "1048576"; NULL or empty: `fallback` */
static inline uint64_t cmdmem_parse_bytes(const char* text, uint64_t fallback) {
    if (!text || !*text) return fallback;
    char* end = NULL;
    double value = strtod(text, &end);
    if (end == text || value < 0) return fallback;
    switch (*end) {
    case 'g': case 'G': value *= 1024.0;  /* fall through */
    case 'm': case 'M': value *= 1024.0;  /* fall through */
    case 'k': case 'K': value *= 1024.0; break;
    default: break;
    }
    return (uint64_t)value;
}

static inline CmdMem* cmdmem_create(uint64_t perPool, uint64_t total, int logEvery) {
    CmdMem* m = calloc(1, sizeof(CmdMem));
    if (!m) return NULL;
    pthread_mutex_init(&m->lock, NULL);
    m->perPool = perPool;
    m->total = total;
    m->logEvery = logEvery;
    return m;
}

static inline void cmdmem_destroy(CmdMem* m) {
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    free(m);
}

static inline uint32_t cmdmem_hash(uint64_t key, int bits) {
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

static inline CmdMemPool* cmdmem_pool(CmdMem* m, uint64_t key) {
    uint32_t mask = (1u << CMDMEM_POOL_BITS) - 1, i = cmdmem_hash(key, CMDMEM_POOL_BITS);
    for (int probe = 0; probe < CMDMEM_PROBES; probe++, i = (i + 1) & mask) {
        uint64_t k = __atomic_load_n(&m->pools[i].key, __ATOMIC_ACQUIRE);
        if (k == key) return &m->pools[i];
        if (k == CMDMEM_EMPTY) break;
    }
    return NULL;
}

static inline CmdMemBuffer* cmdmem_buffer(CmdMem* m, uint64_t key) {
    uint32_t mask = (1u << CMDMEM_BUFFER_BITS) - 1, i = cmdmem_hash(key, CMDMEM_BUFFER_BITS);
    for (int probe = 0; probe < CMDMEM_PROBES; probe++, i = (i + 1) & mask) {
        uint64_t k = __atomic_load_n(&m->buffers[i].key, __ATOMIC_ACQUIRE);
        if (k == key) return &m->buffers[i];
        if (k == CMDMEM_EMPTY) break;
    }
    return NULL;
}

static inline void cmdmem_overflow(CmdMem* m, const char* what) {
    if (!__atomic_exchange_n(&m->full, 1, __ATOMIC_RELAXED))
        CMDMEM_LOG("command memory: %s table full, releasing every %uth reset from now on", what, CMDMEM_FALLBACK_EVERY);
}

/* A buffer's share of its pool's estimate. */
static inline uint64_t cmdmem_share(CmdMem* m, CmdMemBuffer* b) {
    return b->generation == m->pools[b->pool].generation ? b->retained : 0;
}

static inline void cmdmem_drop_buffer_locked(CmdMem* m, CmdMemBuffer* b) {
    CmdMemPool* p = &m->pools[b->pool];
    uint64_t share = cmdmem_share(m, b);
    if (share) {
        p->retained -= share;
        __atomic_sub_fetch(&m->retained, share, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&b->key, CMDMEM_GONE, __ATOMIC_RELEASE);
}

/* Removes a pool with its command buffers (vkDestroyCommandPool, or its device went away). */
static inline void cmdmem_drop_pool_locked(CmdMem* m, CmdMemPool* p) {
    uint32_t index = (uint32_t)(p - m->pools);
    for (uint32_t i = 0; i < (1u << CMDMEM_BUFFER_BITS); i++) {
        uint64_t k = m->buffers[i].key;
        if (k != CMDMEM_EMPTY && k != CMDMEM_GONE && m->buffers[i].pool == index)
            __atomic_store_n(&m->buffers[i].key, CMDMEM_GONE, __ATOMIC_RELEASE);
    }
    __atomic_sub_fetch(&m->retained, p->retained, __ATOMIC_RELAXED);
    p->retained = 0;
    __atomic_store_n(&p->key, CMDMEM_GONE, __ATOMIC_RELEASE);
}

static inline void cmdmem_pool_created(CmdMem* m, uint64_t device, uint64_t pool, bool resetBuffer) {
    if (!m || !pool) return;
    pthread_mutex_lock(&m->lock);
    CmdMemPool* stale = cmdmem_pool(m, pool);
    if (stale) cmdmem_drop_pool_locked(m, stale); /* a handle the driver reused */
    uint32_t mask = (1u << CMDMEM_POOL_BITS) - 1, i = cmdmem_hash(pool, CMDMEM_POOL_BITS);
    CmdMemPool* slot = NULL;
    for (int probe = 0; probe < CMDMEM_PROBES && !slot; probe++, i = (i + 1) & mask)
        if (m->pools[i].key == CMDMEM_EMPTY || m->pools[i].key == CMDMEM_GONE) slot = &m->pools[i];
    if (slot) {
        uint32_t generation = slot->generation + 1; /* stale buffers of an earlier pool here stay stale */
        memset(slot, 0, sizeof(*slot));
        slot->device = device;
        slot->generation = generation;
        slot->resetBuffer = resetBuffer;
        __atomic_store_n(&slot->key, pool, __ATOMIC_RELEASE);
    }
    else cmdmem_overflow(m, "command pool");
    pthread_mutex_unlock(&m->lock);
}

static inline void cmdmem_pool_destroyed(CmdMem* m, uint64_t pool) {
    if (!m || !pool) return;
    pthread_mutex_lock(&m->lock);
    CmdMemPool* p = cmdmem_pool(m, pool);
    if (p) cmdmem_drop_pool_locked(m, p);
    pthread_mutex_unlock(&m->lock);
}

static inline void cmdmem_device_destroyed(CmdMem* m, uint64_t device) {
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < (1u << CMDMEM_POOL_BITS); i++) {
        uint64_t k = m->pools[i].key;
        if (k != CMDMEM_EMPTY && k != CMDMEM_GONE && m->pools[i].device == device) cmdmem_drop_pool_locked(m, &m->pools[i]);
    }
    pthread_mutex_unlock(&m->lock);
}

static inline void cmdmem_buffers_allocated(CmdMem* m, uint64_t pool, uint32_t count, const uint64_t* buffers) {
    if (!m || !count) return;
    pthread_mutex_lock(&m->lock);
    CmdMemPool* p = cmdmem_pool(m, pool);
    if (!p) cmdmem_overflow(m, "command pool");
    for (uint32_t n = 0; p && n < count; n++) {
        uint64_t key = buffers[n];
        if (!key) continue;
        CmdMemBuffer* stale = cmdmem_buffer(m, key);
        if (stale) cmdmem_drop_buffer_locked(m, stale);
        uint32_t mask = (1u << CMDMEM_BUFFER_BITS) - 1, i = cmdmem_hash(key, CMDMEM_BUFFER_BITS);
        CmdMemBuffer* slot = NULL;
        for (int probe = 0; probe < CMDMEM_PROBES && !slot; probe++, i = (i + 1) & mask)
            if (m->buffers[i].key == CMDMEM_EMPTY || m->buffers[i].key == CMDMEM_GONE) slot = &m->buffers[i];
        if (!slot) {
            cmdmem_overflow(m, "command buffer");
            break;
        }
        memset(slot, 0, sizeof(*slot));
        slot->pool = (uint32_t)(p - m->pools);
        slot->generation = p->generation;
        __atomic_store_n(&slot->key, key, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&m->lock);
}

static inline void cmdmem_buffers_freed(CmdMem* m, uint32_t count, const uint64_t* buffers) {
    if (!m || !count) return;
    pthread_mutex_lock(&m->lock);
    for (uint32_t n = 0; n < count; n++) {
        CmdMemBuffer* b = buffers[n] ? cmdmem_buffer(m, buffers[n]) : NULL;
        if (b) cmdmem_drop_buffer_locked(m, b);
    }
    pthread_mutex_unlock(&m->lock);
}

/* Commands recorded (the Vortek server counts a replayed batch at once). */
static inline void cmdmem_recorded(CmdMem* m, uint64_t buffer, uint32_t renderings, bool query, bool renderPass) {
    CmdMemBuffer* b = m ? cmdmem_buffer(m, buffer) : NULL;
    if (!b) return;
    b->renderings += renderings;
    b->query |= query;
    b->renderPass |= renderPass;
}

static inline void cmdmem_log_release(CmdMem* m, const char* what, uint64_t handle, uint64_t bytes, uint32_t resets) {
    uint64_t n = __atomic_add_fetch(&m->releases, 1, __ATOMIC_RELAXED);
    uint64_t sum = __atomic_add_fetch(&m->releasedBytes, bytes, __ATOMIC_RELAXED);
    if (m->logEvery || n <= 4 || n % 64 == 0)
        CMDMEM_LOG("command memory: release #%llu %s 0x%llx after %u resets: %.1f KiB (all pools now %.1f KiB, %.1f MiB released so far)",
                   (unsigned long long)n, what, (unsigned long long)handle, resets, bytes / 1024.0,
                   __atomic_load_n(&m->retained, __ATOMIC_RELAXED) / 1024.0, sum / 1048576.0);
}

static inline bool cmdmem_fallback(CmdMem* m) {
    return __atomic_add_fetch(&m->fallbackResets, 1, __ATOMIC_RELAXED) % CMDMEM_FALLBACK_EVERY == 0;
}

static inline bool cmdmem_buffer_release_due(CmdMem* m, CmdMemBuffer* b) {
    CmdMemPool* p = &m->pools[b->pool];
    uint64_t share = cmdmem_share(m, b);
    if (share && (p->retained >= m->perPool || __atomic_load_n(&m->retained, __ATOMIC_RELAXED) >= m->total)) return true;
    return b->suspect && b->resets >= CMDMEM_BACKSTOP_RESETS;
}

static inline void cmdmem_buffer_released(CmdMem* m, CmdMemBuffer* b, uint64_t handle) {
    CmdMemPool* p = &m->pools[b->pool];
    uint64_t share = cmdmem_share(m, b);
    p->retained -= share;
    __atomic_sub_fetch(&m->retained, share, __ATOMIC_RELAXED);
    uint32_t resets = b->resets;
    b->retained = 0;
    b->generation = p->generation;
    b->resets = 0;
    b->suspect = false;
    cmdmem_log_release(m, "command buffer", handle, share, resets);
}

/* vkBeginCommandBuffer: whether the caller first resets `buffer` with RELEASE_RESOURCES (the implicit
 * reset would keep the memory; only for pools that allow single resets). The current recording starts
 * empty either way. */
static inline bool cmdmem_begin(CmdMem* m, uint64_t buffer) {
    CmdMemBuffer* b = m ? cmdmem_buffer(m, buffer) : NULL;
    if (!b) return false;
    b->renderings = 0;
    b->query = b->renderPass = false;
    if (!m->pools[b->pool].resetBuffer) return false;
    b->resets++;
    if (__atomic_load_n(&m->full, __ATOMIC_RELAXED)) return b->resets % CMDMEM_FALLBACK_EVERY == 0;
    if (!cmdmem_buffer_release_due(m, b)) return false;
    cmdmem_buffer_released(m, b, buffer);
    return true;
}

/* vkEndCommandBuffer: what the recording leaves in the driver. */
static inline void cmdmem_end(CmdMem* m, uint64_t buffer) {
    CmdMemBuffer* b = m ? cmdmem_buffer(m, buffer) : NULL;
    if (!b || !b->query) return;
    CmdMemPool* p = &m->pools[b->pool];
    if (b->renderPass) b->suspect = p->suspect = true;
    if (!b->renderings) return;
    uint64_t bytes = (uint64_t)b->renderings * CMDMEM_RENDERING_BYTES;
    if (b->generation != p->generation) {
        b->retained = 0;
        b->generation = p->generation;
    }
    b->retained += bytes;
    p->retained += bytes;
    __atomic_add_fetch(&m->retained, bytes, __ATOMIC_RELAXED);
    b->renderings = 0;
}

/* vkResetCommandPool: whether to add RELEASE_RESOURCES. */
static inline bool cmdmem_pool_reset(CmdMem* m, uint64_t pool) {
    CmdMemPool* p = cmdmem_pool(m, pool);
    if (!p) return cmdmem_fallback(m);
    p->resets++;
    bool release;
    if (__atomic_load_n(&m->full, __ATOMIC_RELAXED)) release = p->resets >= CMDMEM_FALLBACK_EVERY;
    else
        release = (p->retained && (p->retained >= m->perPool || __atomic_load_n(&m->retained, __ATOMIC_RELAXED) >= m->total)) ||
                  (p->suspect && p->resets >= CMDMEM_BACKSTOP_RESETS);
    if (!release) return false;
    uint64_t bytes = p->retained;
    uint32_t resets = p->resets;
    __atomic_sub_fetch(&m->retained, bytes, __ATOMIC_RELAXED);
    p->retained = 0;
    p->generation++;
    p->resets = 0;
    p->suspect = false;
    cmdmem_log_release(m, "command pool", pool, bytes, resets);
    return true;
}

/* vkResetCommandBuffer: whether to add RELEASE_RESOURCES. */
static inline bool cmdmem_buffer_reset(CmdMem* m, uint64_t buffer) {
    CmdMemBuffer* b = cmdmem_buffer(m, buffer);
    if (!b) return cmdmem_fallback(m);
    b->renderings = 0;
    b->query = b->renderPass = false;
    b->resets++;
    if (__atomic_load_n(&m->full, __ATOMIC_RELAXED)) return b->resets % CMDMEM_FALLBACK_EVERY == 0;
    if (!cmdmem_buffer_release_due(m, b)) return false;
    cmdmem_buffer_released(m, b, buffer);
    return true;
}

#endif
