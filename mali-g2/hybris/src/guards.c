/*
 * [mali-g2] Protections whose behaviour follows MaliBridge's (re-implemented, not copied; see the
 * notes' MALIBRIDGE-VS-HYBRIS-2026-11-12.md, E), each with its own switch:
 *
 *   - Heap cap (heap_cap.h), on by default: on a device with 12800 MB of RAM or less, device-local
 *     heaps are reported as half the RAM rounded down to 512 MB, 2048..4096 MB, and no more than was
 *     free (MemAvailable) at the process's first query, 2048 MB at least. A VK_EXT_memory_budget
 *     budget the driver fills is held under the reported size (r56p0 has no such extension).
 *     DROIDDECK_HYBRIS_HEAP_MAX_MB=<MB> sets the cap instead (0: none);
 *     DROIDDECK_HYBRIS_HEAP_FOLLOW_FREE=0 keeps the default cap whatever is free.
 *   - Wait retry (wait_retry.h), on by default: vkWaitForFences, vkWaitSemaphores(KHR),
 *     vkQueueWaitIdle and vkDeviceWaitIdle (icd.c), vkGetQueryPoolResults with WAIT_BIT, and this
 *     library's own fence waits (wsi.c). A result a wait may not return (VK_ERROR_INITIALIZATION_FAILED
 *     after an interrupted ppoll), or a VK_TIMEOUT before the caller's deadline, is waited on again
 *     until the deadline; without one for DROIDDECK_HYBRIS_WAIT_RETRY_S (default 30) seconds.
 *     DROIDDECK_HYBRIS_WAIT_RETRY=0 turns it off.
 *   - Stuck occlusion queries (query_stuck.h): an occlusion query answered "not ready" for
 *     DROIDDECK_HYBRIS_QUERY_STUCK_MS (default 500; 0: not watched) since the first such answer after
 *     its last reset is counted and logged ("hybris-icd: query stuck: ..."); the answers are not
 *     changed. DROIDDECK_HYBRIS_QUERY_VISIBLE=1 answers such a query as visible (1 sample) when its
 *     End was recorded in a primary submitted that long ago (qs_visible_due).
 *   - Crash guard (crash_guard.c), off by default: DROIDDECK_HYBRIS_CRASH_GUARD=1 contains a Mali
 *     fault inside vkCreateGraphicsPipelines / vkCreateComputePipelines (the call fails with
 *     VK_ERROR_UNKNOWN and null pipelines) and vkEndCommandBuffer (it returns VK_SUCCESS, and every
 *     queue submission after it fails with VK_ERROR_DEVICE_LOST, so the buffer never reaches the
 *     GPU: DXVK throws on a failed vkEndCommandBuffer, and stops submitting after a lost device).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include "icd.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crash_guard.h"
#include "heap_cap.h"
#include "query_stuck.h"
#include "wait_retry.h"

#define LOG(fmt, ...) fprintf(stderr, "hybris-icd: " fmt "\n", ##__VA_ARGS__)

uint64_t icd_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void napNs(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    nanosleep(&ts, NULL);
}

static void* findInChain(void* chain, VkStructureType type) {
    for (VkBaseOutStructure* s = chain; s; s = s->pNext)
        if (s->sType == type) return s;
    return NULL;
}

/* 1, 2, 3, then 10, 100, 1000, ... */
static bool worthALine(unsigned long n) {
    if (n <= 3) return true;
    while (n % 10 == 0) n /= 10;
    return n == 1;
}

/* The driver's functions, the same for every device (libhybris hands out the Android loader's
 * trampolines): the first one seen is kept. */
enum {
    N_WAIT_FENCES, N_WAIT_SEMAPHORES, N_WAIT_SEMAPHORES_KHR, N_GET_QUERY_POOL_RESULTS, N_CREATE_QUERY_POOL, N_DESTROY_QUERY_POOL,
    N_RESET_QUERY_POOL, N_RESET_QUERY_POOL_EXT, N_CMD_RESET_QUERY_POOL, N_CMD_END_QUERY, N_CMD_END_QUERY_INDEXED,
    N_CMD_EXECUTE_COMMANDS, N_COUNT
};
static PFN_vkVoidFunction nextFns[N_COUNT];
#define NEXT(index, type) ((type)__atomic_load_n(&nextFns[index], __ATOMIC_ACQUIRE))
#define KEY(handle) ((uint64_t)(uintptr_t)(handle))

/* `wrapper` when the driver has `name`: asked with a device, or kept from an earlier device. */
static PFN_vkVoidFunction wrapped(VkDevice handle, const char* name, int index, PFN_vkVoidFunction wrapper) {
    Device* device = handle ? icd_device(handle) : NULL;
    PFN_vkVoidFunction driver = device ? device->instance->GetDeviceProcAddr(handle, name) : NULL;
    if (device && !driver) return NULL;
    void* expected = NULL;
    if (driver) __atomic_compare_exchange_n((void**)&nextFns[index], &expected, (void*)driver, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return __atomic_load_n(&nextFns[index], __ATOMIC_ACQUIRE) ? wrapper : NULL;
}

/* ---- heap cap ---- */

static pthread_once_t heapOnce = PTHREAD_ONCE_INIT;
static uint64_t heapCapBytes;

static void readHeapCap(void) {
    char text[8192] = "";
    FILE* f = fopen("/proc/meminfo", "re");
    if (f) {
        text[fread(text, 1, sizeof(text) - 1, f)] = 0;
        fclose(f);
    }
    const char* set = getenv("DROIDDECK_HYBRIS_HEAP_MAX_MB");
    if (set && *set) {
        int64_t mb = heapcap_parse_mb(set);
        if (mb >= 0) {
            heapCapBytes = (uint64_t)mb << 20;
            if (mb) LOG("heap cap: device-local heaps reported as %lld MB at most (DROIDDECK_HYBRIS_HEAP_MAX_MB)", (long long)mb);
            else LOG("heap cap: off (DROIDDECK_HYBRIS_HEAP_MAX_MB=0)");
            return;
        }
        LOG("heap cap: DROIDDECK_HYBRIS_HEAP_MAX_MB=%s is not a number of MB, ignored", set);
    }
    uint64_t ramMb = heapcap_meminfo_kb(text, "MemTotal") >> 10;
    uint32_t cap = heapcap_default_mb(ramMb);
    if (!cap) {
        LOG("heap cap: off, %llu MB of RAM (more than %u, or unknown); DROIDDECK_HYBRIS_HEAP_MAX_MB=<MB> sets one",
            (unsigned long long)ramMb, HEAPCAP_RAM_MAX_MB);
        return;
    }
    const char* follow = getenv("DROIDDECK_HYBRIS_HEAP_FOLLOW_FREE");
    uint64_t freeMb = heapcap_meminfo_kb(text, "MemAvailable") >> 10;
    uint32_t mb = follow && !strcmp(follow, "0") ? cap : heapcap_follow_free(cap, freeMb);
    heapCapBytes = (uint64_t)mb << 20;
    LOG("heap cap: %llu MB of RAM (%u or less), %llu MB free: device-local heaps reported as %u MB at most "
        "(DROIDDECK_HYBRIS_HEAP_MAX_MB=0 turns this off)", (unsigned long long)ramMb, HEAPCAP_RAM_MAX_MB, (unsigned long long)freeMb, mb);
}

static void capHeaps(VkPhysicalDeviceMemoryProperties* memory, VkPhysicalDeviceMemoryBudgetPropertiesEXT* budget) {
    pthread_once(&heapOnce, readHeapCap);
    VkDeviceSize before[VK_MAX_MEMORY_HEAPS] = {0};
    for (uint32_t h = 0; h < memory->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) before[h] = memory->memoryHeaps[h].size;
    uint32_t changed = heapcap_apply(memory, budget, heapCapBytes);
    static uint32_t logged;
    uint32_t fresh = changed & ~__atomic_fetch_or(&logged, changed, __ATOMIC_RELAXED);
    for (uint32_t h = 0; fresh && h < VK_MAX_MEMORY_HEAPS; h++)
        if (fresh & (1u << h))
            LOG("heap cap: device-local heap %u reported as %llu MB, not %llu MB", h, (unsigned long long)(memory->memoryHeaps[h].size >> 20),
                (unsigned long long)(before[h] >> 20));
}

static void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties* memory) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    instance->GetPhysicalDeviceMemoryProperties(physical, memory);
    capHeaps(memory, NULL);
}

static void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2* memory) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    if (instance->GetPhysicalDeviceMemoryProperties2) instance->GetPhysicalDeviceMemoryProperties2(physical, memory);
    else instance->GetPhysicalDeviceMemoryProperties(physical, &memory->memoryProperties);
    capHeaps(&memory->memoryProperties, findInChain(memory->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT));
}

/* ---- wait retry ---- */

static pthread_once_t waitOnce = PTHREAD_ONCE_INIT;
static bool waitRetry;
static uint64_t waitCapNs = WR_DEFAULT_CAP_NS;

static void readWaitConfig(void) {
    const char* on = getenv("DROIDDECK_HYBRIS_WAIT_RETRY");
    waitRetry = !(on && *on == '0');
    const char* cap = getenv("DROIDDECK_HYBRIS_WAIT_RETRY_S");
    if (cap && *cap >= '0' && *cap <= '9') waitCapNs = strtoull(cap, NULL, 10) * WR_SEC;
}

bool guards_wait_retry_on(void) {
    pthread_once(&waitOnce, readWaitConfig);
    return waitRetry;
}

static const char* const waitNames[GUARDS_WAIT_KINDS] = {"vkWaitForFences", "vkWaitSemaphores", "vkQueueWaitIdle", "vkDeviceWaitIdle",
                                                         "vkGetQueryPoolResults (WAIT_BIT)"};
static unsigned long waitTally[GUARDS_WAIT_KINDS];

VkResult guards_retry_wait(int kind, VkResult (*wait)(void*, uint64_t), VkResult (*poll)(void*), void* ctx, uint64_t timeout,
                           uint64_t start, VkResult first) {
    uint64_t took = icd_now_ns() - start;
    if (!guards_wait_retry_on() || !wr_needed(first, took, timeout)) return first;
    WaitOp op = {wait, poll, ctx, timeout, start, waitCapNs, icd_now_ns, napNs};
    WaitOutcome o = wr_retry(&op, first);
    unsigned long n = __atomic_add_fetch(&waitTally[kind], 1, __ATOMIC_RELAXED);
    if (worthALine(n) || o.keptFailing) {
        char limit[48];
        if (timeout == UINT64_MAX) snprintf(limit, sizeof(limit), "no timeout");
        else snprintf(limit, sizeof(limit), "timeout %.3f s", timeout / 1e9);
        LOG("wait retry: %s: the driver answered %d after %.3f s (%s); %u more wait(s) in %.3f s, the caller gets %d%s "
            "(%lu such wait(s) so far; DROIDDECK_HYBRIS_WAIT_RETRY=0 turns this off)", waitNames[kind], first, took / 1e9, limit, o.tries,
            o.retriedNs / 1e9, o.result, o.keptFailing ? ", the driver's own result: it kept failing" : "", n);
    }
    return o.result;
}

typedef struct {
    PFN_vkWaitForFences next;
    VkDevice handle;
    uint32_t count;
    const VkFence* fences;
    VkBool32 all;
} FenceWait;

static VkResult fenceWait(void* c, uint64_t timeout) {
    FenceWait* w = c;
    return w->next(w->handle, w->count, w->fences, w->all, timeout);
}

static VkResult fencePoll(void* c) {
    return fenceWait(c, 0);
}

static VkResult waitFences(PFN_vkWaitForFences next, VkDevice handle, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    uint64_t start = icd_now_ns();
    VkResult r = next(handle, count, fences, all, timeout);
    if (r == VK_SUCCESS) return r;
    FenceWait w = {next, handle, count, fences, all};
    return guards_retry_wait(GUARDS_WAIT_FENCE, fenceWait, fencePoll, &w, timeout, start, r);
}

VkResult icd_wait_fences(Device* device, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    return waitFences(device->WaitForFences, device->handle, count, fences, all, timeout);
}

static VkResult VKAPI_CALL WaitForFences(VkDevice handle, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    return waitFences(NEXT(N_WAIT_FENCES, PFN_vkWaitForFences), handle, count, fences, all, timeout);
}

typedef struct {
    PFN_vkWaitSemaphores next;
    VkDevice handle;
    const VkSemaphoreWaitInfo* info;
} SemaphoreWait;

static VkResult semaphoreWait(void* c, uint64_t timeout) {
    SemaphoreWait* w = c;
    return w->next(w->handle, w->info, timeout);
}

static VkResult semaphorePoll(void* c) {
    return semaphoreWait(c, 0);
}

static VkResult waitSemaphores(PFN_vkWaitSemaphores next, VkDevice handle, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    uint64_t start = icd_now_ns();
    VkResult r = next(handle, info, timeout);
    if (r == VK_SUCCESS) return r;
    SemaphoreWait w = {next, handle, info};
    return guards_retry_wait(GUARDS_WAIT_SEMAPHORE, semaphoreWait, semaphorePoll, &w, timeout, start, r);
}

static VkResult VKAPI_CALL WaitSemaphores(VkDevice handle, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    return waitSemaphores(NEXT(N_WAIT_SEMAPHORES, PFN_vkWaitSemaphores), handle, info, timeout);
}

static VkResult VKAPI_CALL WaitSemaphoresKHR(VkDevice handle, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    return waitSemaphores(NEXT(N_WAIT_SEMAPHORES_KHR, PFN_vkWaitSemaphores), handle, info, timeout);
}

/* ---- stuck occlusion queries ---- */

typedef struct QueryPool {
    uint64_t handle;
    uint32_t count;
    struct QueryPool* next;
    QsQuery queries[];
} QueryPool;

#define POOL_BUCKETS 256
static pthread_once_t queryOnce = PTHREAD_ONCE_INIT;
static uint64_t stuckLimitNs;
static bool queryVisible;
static QsTables* recordings; /* for the visible answer only */
static pthread_mutex_t poolLock = PTHREAD_MUTEX_INITIALIZER;
static QueryPool* pools[POOL_BUCKETS];
static uint32_t poolCount;
static struct {
    unsigned long stuck, ready, reset, visible, summarized;
    uint64_t longestNs;
} queryStats;

static void readQueryConfig(void) {
    const char* ms = getenv("DROIDDECK_HYBRIS_QUERY_STUCK_MS");
    uint64_t limit = ms && *ms >= '0' && *ms <= '9' ? strtoull(ms, NULL, 10) : 500;
    if (limit && limit < 100) limit = 100;
    stuckLimitNs = limit * WR_MS;
    const char* visible = getenv("DROIDDECK_HYBRIS_QUERY_VISIBLE");
    if (stuckLimitNs && visible && *visible && *visible != '0') {
        QsTables* t = calloc(1, sizeof(*t));
        if (t) {
            pthread_mutex_init(&t->lock, NULL);
            recordings = t;
            queryVisible = true;
        }
    }
    if (queryVisible)
        LOG("query stuck: an occlusion query answered not ready for %llu ms, ended in a primary submitted that long ago, is answered "
            "as visible (DROIDDECK_HYBRIS_QUERY_VISIBLE=1)", (unsigned long long)limit);
}

static bool queryStuckOn(void) {
    pthread_once(&queryOnce, readQueryConfig);
    return stuckLimitNs != 0;
}

bool guards_query_visible_on(void) {
    return queryStuckOn() && queryVisible;
}

static uint32_t poolBucket(uint64_t handle) {
    return (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> 56) & (POOL_BUCKETS - 1);
}

/* The pool stays valid after the lock: the application may not destroy it while using it. */
static QueryPool* poolOf(VkQueryPool pool) {
    if (!pool || !__atomic_load_n(&poolCount, __ATOMIC_RELAXED)) return NULL;
    uint64_t handle = (uint64_t)pool;
    pthread_mutex_lock(&poolLock);
    QueryPool* p = pools[poolBucket(handle)];
    while (p && p->handle != handle) p = p->next;
    pthread_mutex_unlock(&poolLock);
    return p;
}

static void querySummary(void) {
    unsigned long stuck = __atomic_load_n(&queryStats.stuck, __ATOMIC_RELAXED);
    if (!stuck || __atomic_exchange_n(&queryStats.summarized, stuck, __ATOMIC_RELAXED) == stuck) return;
    LOG("query stuck: %lu occlusion query run(s) answered not ready for %llu ms or more: %lu became available (the longest after "
        "%llu ms), %lu were reset first, %lu answers were visible ones", stuck, (unsigned long long)(stuckLimitNs / WR_MS),
        __atomic_load_n(&queryStats.ready, __ATOMIC_RELAXED), (unsigned long long)(__atomic_load_n(&queryStats.longestNs, __ATOMIC_RELAXED) / WR_MS),
        __atomic_load_n(&queryStats.reset, __ATOMIC_RELAXED), __atomic_load_n(&queryStats.visible, __ATOMIC_RELAXED));
}

static void runEnded(uint64_t pool, uint32_t query, uint64_t lasted, bool reset) {
    if (!lasted) return;
    unsigned long n = __atomic_add_fetch(reset ? &queryStats.reset : &queryStats.ready, 1, __ATOMIC_RELAXED);
    uint64_t longest = __atomic_load_n(&queryStats.longestNs, __ATOMIC_RELAXED);
    while (!reset && lasted > longest &&
           !__atomic_compare_exchange_n(&queryStats.longestNs, &longest, lasted, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (worthALine(n))
        LOG("query stuck: occlusion query %u of pool %#llx %s after %llu ms not ready", query, (unsigned long long)pool,
            reset ? "was reset" : "became available", (unsigned long long)(lasted / WR_MS));
}

static VkResult VKAPI_CALL CreateQueryPool(VkDevice handle, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator,
                                           VkQueryPool* pool) {
    VkResult result = NEXT(N_CREATE_QUERY_POOL, PFN_vkCreateQueryPool)(handle, info, allocator, pool);
    if (result != VK_SUCCESS || info->queryType != VK_QUERY_TYPE_OCCLUSION || !info->queryCount) return result;
    QueryPool* p = calloc(1, sizeof(*p) + (size_t)info->queryCount * sizeof(QsQuery));
    if (!p) return result;
    p->handle = (uint64_t)*pool;
    p->count = info->queryCount;
    pthread_mutex_lock(&poolLock);
    p->next = pools[poolBucket(p->handle)];
    pools[poolBucket(p->handle)] = p;
    __atomic_add_fetch(&poolCount, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&poolLock);
    return result;
}

static void VKAPI_CALL DestroyQueryPool(VkDevice handle, VkQueryPool pool, const VkAllocationCallbacks* allocator) {
    QueryPool* gone = NULL;
    if (pool) {
        pthread_mutex_lock(&poolLock);
        for (QueryPool** link = &pools[poolBucket((uint64_t)pool)]; *link; link = &(*link)->next) {
            if ((*link)->handle != (uint64_t)pool) continue;
            gone = *link;
            *link = gone->next;
            __atomic_sub_fetch(&poolCount, 1, __ATOMIC_RELAXED);
            break;
        }
        pthread_mutex_unlock(&poolLock);
    }
    free(gone);
    NEXT(N_DESTROY_QUERY_POOL, PFN_vkDestroyQueryPool)(handle, pool, allocator);
}

static void queriesReset(VkQueryPool pool, uint32_t first, uint32_t count) {
    QueryPool* p = poolOf(pool);
    if (!p || first >= p->count) return;
    uint32_t end = count > p->count - first ? p->count : first + count;
    uint64_t now = icd_now_ns();
    for (uint32_t q = first; q < end; q++) runEnded((uint64_t)pool, q, qs_reset(&p->queries[q], now), true);
}

static void VKAPI_CALL ResetQueryPool(VkDevice handle, VkQueryPool pool, uint32_t first, uint32_t count) {
    queriesReset(pool, first, count);
    NEXT(N_RESET_QUERY_POOL, PFN_vkResetQueryPool)(handle, pool, first, count);
}

static void VKAPI_CALL ResetQueryPoolEXT(VkDevice handle, VkQueryPool pool, uint32_t first, uint32_t count) {
    queriesReset(pool, first, count);
    NEXT(N_RESET_QUERY_POOL_EXT, PFN_vkResetQueryPool)(handle, pool, first, count);
}

typedef struct {
    PFN_vkGetQueryPoolResults next;
    VkDevice handle;
    VkQueryPool pool;
    uint32_t first, count;
    size_t size;
    void* data;
    VkDeviceSize stride;
    VkQueryResultFlags flags;
} QueryWait;

static VkResult queryWait(void* c, uint64_t timeout) {
    QueryWait* w = c;
    return w->next(w->handle, w->pool, w->first, w->count, w->size, w->data, w->stride, w->flags);
}

static VkResult queryPoll(void* c) {
    QueryWait* w = c;
    return w->next(w->handle, w->pool, w->first, w->count, w->size, w->data, w->stride, w->flags & ~VK_QUERY_RESULT_WAIT_BIT);
}

/* A "not ready" answer for query `first` (the one a poll of several is counted against): counted,
 * and answered as visible when that is due. */
static VkResult notReady(QueryPool* p, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void* data,
                         VkQueryResultFlags flags) {
    QsQuery* q = &p->queries[first];
    uint64_t now = icd_now_ns();
    bool crossed;
    uint64_t lasted = qs_not_ready(q, now, stuckLimitNs, &crossed);
    if (crossed) {
        unsigned long n = __atomic_add_fetch(&queryStats.stuck, 1, __ATOMIC_RELAXED);
        if (worthALine(n))
            LOG("query stuck: occlusion query %u of pool %#llx answered not ready for %llu ms (%u polls; %lu such run(s) so far%s)", first,
                (unsigned long long)(uint64_t)pool, (unsigned long long)(lasted / WR_MS), __atomic_load_n(&q->polls, __ATOMIC_RELAXED), n,
                queryVisible ? "" : "; DROIDDECK_HYBRIS_QUERY_VISIBLE=1 answers such queries as visible");
    }
    uint64_t primary;
    if (!queryVisible || count != 1 || (flags & VK_QUERY_RESULT_WITH_STATUS_BIT_KHR) ||
        !qs_visible_due(recordings, q, now, stuckLimitNs, &primary) || !qs_write_visible(data, size, flags, 1))
        return VK_NOT_READY;
    qs_end_run(q, now);
    qs_mark_visible(recordings, primary);
    unsigned long n = __atomic_add_fetch(&queryStats.visible, 1, __ATOMIC_RELAXED);
    if (worthALine(n))
        LOG("query stuck: occlusion query %u of pool %#llx answered as visible after %llu ms not ready (%lu so far)", first,
            (unsigned long long)(uint64_t)pool, (unsigned long long)(lasted / WR_MS), n);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL GetQueryPoolResults(VkDevice handle, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void* data,
                                               VkDeviceSize stride, VkQueryResultFlags flags) {
    PFN_vkGetQueryPoolResults next = NEXT(N_GET_QUERY_POOL_RESULTS, PFN_vkGetQueryPoolResults);
    bool wait = flags & VK_QUERY_RESULT_WAIT_BIT;
    uint64_t start = wait ? icd_now_ns() : 0;
    VkResult r = next(handle, pool, first, count, size, data, stride, flags);
    if (wait && !wr_legal(r)) {
        QueryWait w = {next, handle, pool, first, count, size, data, stride, flags};
        r = guards_retry_wait(GUARDS_WAIT_QUERY, queryWait, queryPoll, &w, UINT64_MAX, start, r);
    }
    if ((r != VK_SUCCESS && r != VK_NOT_READY) || (flags & VK_QUERY_RESULT_PARTIAL_BIT) || !stuckLimitNs) return r;
    QueryPool* p = poolOf(pool);
    if (!p || first >= p->count) return r;
    if (r == VK_NOT_READY) return wait ? r : notReady(p, pool, first, count, size, data, flags);
    uint32_t end = count > p->count - first ? p->count : first + count;
    uint64_t now = 0;
    for (uint32_t q = first; q < end; q++) {
        if (!__atomic_load_n(&p->queries[q].runNs, __ATOMIC_RELAXED)) continue;
        if (!now) now = icd_now_ns();
        runEnded((uint64_t)pool, q, qs_end_run(&p->queries[q], now), false);
    }
    return r;
}

static void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t first, uint32_t count) {
    queriesReset(pool, first, count);
    NEXT(N_CMD_RESET_QUERY_POOL, PFN_vkCmdResetQueryPool)(commandBuffer, pool, first, count);
}

static void queryEnded(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t query) {
    QueryPool* p = poolOf(pool);
    if (!p || query >= p->count) return;
    QS_STORE(&p->queries[query].endRec, qs_current(recordings, KEY(commandBuffer)));
    QS_STORE(&p->queries[query].endNs, icd_now_ns());
}

static void VKAPI_CALL CmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t query) {
    queryEnded(commandBuffer, pool, query);
    NEXT(N_CMD_END_QUERY, PFN_vkCmdEndQuery)(commandBuffer, pool, query);
}

static void VKAPI_CALL CmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t query, uint32_t index) {
    queryEnded(commandBuffer, pool, query);
    NEXT(N_CMD_END_QUERY_INDEXED, PFN_vkCmdEndQueryIndexedEXT)(commandBuffer, pool, query, index);
}

static void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t count, const VkCommandBuffer* buffers) {
    if (buffers && sizeof(VkCommandBuffer) == sizeof(uint64_t)) qs_executed(recordings, KEY(commandBuffer), (const uint64_t*)buffers, count);
    NEXT(N_CMD_EXECUTE_COMMANDS, PFN_vkCmdExecuteCommands)(commandBuffer, count, buffers);
}

void guards_command_buffer_begun(VkCommandBuffer commandBuffer) {
    if (guards_query_visible_on()) qs_begin(recordings, KEY(commandBuffer));
}

void guards_submitted(uint32_t count, const VkSubmitInfo* submits) {
    if (!guards_query_visible_on() || sizeof(VkCommandBuffer) != sizeof(uint64_t)) return;
    uint64_t now = icd_now_ns();
    for (uint32_t i = 0; i < count; i++)
        if (submits[i].pCommandBuffers)
            qs_submitted(recordings, (const uint64_t*)submits[i].pCommandBuffers, submits[i].commandBufferCount, now);
}

void guards_submitted2(uint32_t count, const VkSubmitInfo2* submits) {
    if (!guards_query_visible_on()) return;
    uint64_t now = icd_now_ns();
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t j = 0; submits[i].pCommandBufferInfos && j < submits[i].commandBufferInfoCount; j++) {
            uint64_t cb = KEY(submits[i].pCommandBufferInfos[j].commandBuffer);
            qs_submitted(recordings, &cb, 1, now);
        }
}

/* ---- crash guard ---- */

/* Process-wide: the containment does not know which device a command buffer belongs to. */
static int lost;

bool guards_device_lost(void) {
    return __atomic_load_n(&lost, __ATOMIC_ACQUIRE);
}

bool guards_want_submits(void) {
    return crash_guard_on() || guards_query_visible_on();
}

typedef struct {
    Device* device;
    VkDevice handle;
    VkPipelineCache cache;
    uint32_t count;
    const void* infos;
    const VkAllocationCallbacks* allocator;
    VkPipeline* pipelines;
} PipelineCall;

static int graphicsCall(void* c) {
    PipelineCall* p = c;
    return p->device->CreateGraphicsPipelines(p->handle, p->cache, p->count, p->infos, p->allocator, p->pipelines);
}

static int computeCall(void* c) {
    PipelineCall* p = c;
    return p->device->CreateComputePipelines(p->handle, p->cache, p->count, p->infos, p->allocator, p->pipelines);
}

static VkResult guardedPipelines(const char* what, int (*fn)(void*), PipelineCall* call) {
    bool faulted;
    VkResult result = (VkResult)crash_guard_call(what, fn, call, &faulted);
    if (!faulted) return result;
    for (uint32_t i = 0; call->pipelines && i < call->count; i++) call->pipelines[i] = VK_NULL_HANDLE;
    return VK_ERROR_UNKNOWN;
}

VkResult guards_create_graphics_pipelines(Device* device, VkDevice handle, VkPipelineCache cache, uint32_t count,
                                          const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                          VkPipeline* pipelines) {
    PipelineCall call = {device, handle, cache, count, infos, allocator, pipelines};
    return guardedPipelines("vkCreateGraphicsPipelines", graphicsCall, &call);
}

VkResult guards_create_compute_pipelines(Device* device, VkDevice handle, VkPipelineCache cache, uint32_t count,
                                         const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                         VkPipeline* pipelines) {
    PipelineCall call = {device, handle, cache, count, infos, allocator, pipelines};
    return guardedPipelines("vkCreateComputePipelines", computeCall, &call);
}

typedef struct {
    PFN_vkEndCommandBuffer next;
    VkCommandBuffer commandBuffer;
} EndCall;

static int endCall(void* c) {
    EndCall* e = c;
    return e->next(e->commandBuffer);
}

VkResult guards_end_command_buffer(PFN_vkEndCommandBuffer next, VkCommandBuffer commandBuffer) {
    EndCall call = {next, commandBuffer};
    bool faulted;
    VkResult result = (VkResult)crash_guard_call("vkEndCommandBuffer", endCall, &call, &faulted);
    if (!faulted) return result;
    if (!__atomic_exchange_n(&lost, 1, __ATOMIC_ACQ_REL))
        LOG("crash guard: queue submissions fail with VK_ERROR_DEVICE_LOST from now on (command buffer %p was not recorded)",
            (void*)commandBuffer);
    return VK_SUCCESS;
}

/* ---- lookup ---- */

PFN_vkVoidFunction guards_instance_proc(const char* name) {
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceMemoryProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") || !strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceMemoryProperties2;
    return NULL;
}

#define WRAP(fn, index, wrapper) \
    if (!strcmp(name, fn)) return wrapped(handle, name, index, (PFN_vkVoidFunction)(wrapper))

PFN_vkVoidFunction guards_device_proc(VkDevice handle, const char* name) {
    bool stuck = queryStuckOn();
    if (guards_wait_retry_on()) {
        WRAP("vkWaitForFences", N_WAIT_FENCES, WaitForFences);
        WRAP("vkWaitSemaphores", N_WAIT_SEMAPHORES, WaitSemaphores);
        WRAP("vkWaitSemaphoresKHR", N_WAIT_SEMAPHORES_KHR, WaitSemaphoresKHR);
    }
    if (guards_wait_retry_on() || stuck) WRAP("vkGetQueryPoolResults", N_GET_QUERY_POOL_RESULTS, GetQueryPoolResults);
    if (!stuck) return NULL;
    WRAP("vkCreateQueryPool", N_CREATE_QUERY_POOL, CreateQueryPool);
    WRAP("vkDestroyQueryPool", N_DESTROY_QUERY_POOL, DestroyQueryPool);
    WRAP("vkResetQueryPool", N_RESET_QUERY_POOL, ResetQueryPool);
    WRAP("vkResetQueryPoolEXT", N_RESET_QUERY_POOL_EXT, ResetQueryPoolEXT);
    WRAP("vkCmdResetQueryPool", N_CMD_RESET_QUERY_POOL, CmdResetQueryPool);
    if (!guards_query_visible_on()) return NULL;
    WRAP("vkCmdEndQuery", N_CMD_END_QUERY, CmdEndQuery);
    WRAP("vkCmdEndQueryIndexedEXT", N_CMD_END_QUERY_INDEXED, CmdEndQueryIndexedEXT);
    WRAP("vkCmdExecuteCommands", N_CMD_EXECUTE_COMMANDS, CmdExecuteCommands);
    return NULL;
}

void guards_device_destroyed(Device* device) {
    querySummary();
}
