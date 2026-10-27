/* [mali-g2] Unit test of ../src/cmd_memory.h (the estimate of the Mali's retained command memory), no GPU:
 *   cc -O2 -Wall -I../src -o cmd_memory_test cmd_memory_test.c -lpthread && ./cmd_memory_test
 * Prints CMDMEM-PASS or the first failed check. */
#include <stdio.h>
#define CMDMEM_LOG(fmt, ...) (logged++, (void)snprintf(NULL, 0, fmt, __VA_ARGS__))
static int logged;
#include "cmd_memory.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define KIB 1024ull
#define MIB (1024ull * 1024ull)

/* a recording into `cb`: `renderings` vkCmdBeginRendering, with a query or not */
static void record(CmdMem* m, uint64_t cb, uint32_t renderings, bool query) {
    cmdmem_begin(m, cb);
    cmdmem_recorded(m, cb, renderings, query, false);
    cmdmem_end(m, cb);
}

typedef struct { CmdMem* m; uint64_t base; int frames; } Worker;

/* one thread per pool, as DXVK records on several threads: allocate, record, reset, free */
static void* worker(void* param) {
    Worker* w = param;
    uint64_t pool = w->base, cbs[4] = {w->base + 1, w->base + 2, w->base + 3, w->base + 4};
    cmdmem_pool_created(w->m, 1, pool, false);
    cmdmem_buffers_allocated(w->m, pool, 4, cbs);
    for (int f = 0; f < w->frames; f++) {
        cmdmem_pool_reset(w->m, pool);
        for (int c = 0; c < 4; c++) record(w->m, cbs[c], 16, c == 3);
        if (f % 1000 == 999) { /* churn: buffers freed and allocated again */
            cmdmem_buffers_freed(w->m, 4, cbs);
            cmdmem_buffers_allocated(w->m, pool, 4, cbs);
        }
    }
    cmdmem_pool_destroyed(w->m, pool);
    return NULL;
}

int main(void) {
    CHECK(cmdmem_parse_bytes("16m", 0) == 16 * MIB);
    CHECK(cmdmem_parse_bytes("512k", 0) == 512 * KIB);
    CHECK(cmdmem_parse_bytes("1G", 0) == 1024 * MIB);
    CHECK(cmdmem_parse_bytes("4096", 0) == 4096);
    CHECK(cmdmem_parse_bytes("", 7) == 7 && cmdmem_parse_bytes(NULL, 7) == 7 && cmdmem_parse_bytes("x", 7) == 7);
    CHECK(cmdmem_parse_bytes("16MiB", 0) == 16 * MIB && cmdmem_parse_bytes("64kb", 0) == 64 * KIB && cmdmem_parse_bytes("1.5m", 0) == 3 * MIB / 2);
    /* typos, negative, non-finite and absurd values: the fallback, not something else */
    CHECK(cmdmem_parse_bytes("16x", 7) == 7 && cmdmem_parse_bytes("16 m", 7) == 7 && cmdmem_parse_bytes("-1m", 7) == 7);
    CHECK(cmdmem_parse_bytes("inf", 7) == 7 && cmdmem_parse_bytes("nan", 7) == 7 && cmdmem_parse_bytes("1e30", 7) == 7);
    CHECK(cmdmem_parse_every(NULL) == -1 && cmdmem_parse_every("") == -1 && cmdmem_parse_every("auto") == -1);
    CHECK(cmdmem_parse_every("32x") == -1 && cmdmem_parse_every("32") == 32 && cmdmem_parse_every("0") == 0);
    CHECK(cmdmem_parse_every("-1") == 0 && cmdmem_parse_every("1") == 1);
    CHECK(cmdmem_parse_bytes("1048576g", 7) == CMDMEM_MAX_BYTES && CMDMEM_DEFAULT_TOTAL(CMDMEM_MAX_BYTES) == CMDMEM_MAX_BYTES);

    /* per-pool limit: 1 MiB = 2497 renderings of 420 bytes */
    CmdMem* m = cmdmem_create(1 * MIB, 64 * MIB, 1);
    uint64_t pool = 0x1000, cb = 0x2000, other = 0x3000;
    cmdmem_pool_created(m, 1, pool, false);
    cmdmem_buffers_allocated(m, pool, 1, &cb);
    int resets = 0;
    while (!cmdmem_pool_reset(m, pool)) {
        record(m, cb, 16, true);
        resets++;
        if (resets > 100000) break;
    }
    CHECK(resets == (int)((1 * MIB + 16 * 420 - 1) / (16 * 420)));
    CHECK(__atomic_load_n(&m->retained, __ATOMIC_RELAXED) == 0);
    CHECK(cmdmem_pool(m, pool)->retained == 0);

    /* no query, or a query in another buffer: nothing kept, no release */
    cmdmem_buffers_allocated(m, pool, 1, &other);
    for (int i = 0; i < 10000; i++) {
        CHECK(!cmdmem_pool_reset(m, pool));
        record(m, cb, 16, false);
        record(m, other, 0, true);
    }
    CHECK(cmdmem_pool(m, pool)->retained == 0);

    /* a VkRenderPass with a query (a fresh pool): the backstop at every 1024th reset */
    cmdmem_pool_destroyed(m, pool);
    cmdmem_pool_created(m, 1, pool, false);
    cmdmem_buffers_allocated(m, pool, 1, &cb);
    int backstop = 0;
    for (int i = 1; i <= 3000; i++) {
        if (cmdmem_pool_reset(m, pool)) {
            CHECK(i % CMDMEM_BACKSTOP_RESETS == 0);
            backstop++;
        }
        cmdmem_begin(m, cb);
        cmdmem_recorded(m, cb, 0, true, true);
        cmdmem_end(m, cb);
    }
    CHECK(backstop == 2);
    cmdmem_pool_destroyed(m, pool);
    CHECK(!cmdmem_pool(m, pool) && !cmdmem_buffer(m, cb));
    cmdmem_destroy(m);

    /* total limit over four pools: releases free about a quarter each */
    m = cmdmem_create(64 * MIB, 1 * MIB, 1);
    uint64_t pools[4] = {0x10, 0x20, 0x30, 0x40}, cbs[4] = {0x11, 0x21, 0x31, 0x41};
    for (int p = 0; p < 4; p++) {
        cmdmem_pool_created(m, 1, pools[p], false);
        cmdmem_buffers_allocated(m, pools[p], 1, &cbs[p]);
    }
    int released = 0;
    uint64_t peak = 0;
    for (int f = 0; f < 40000; f++) {
        int p = f % 4;
        released += cmdmem_pool_reset(m, pools[p]);
        record(m, cbs[p], 16, true);
        uint64_t now = __atomic_load_n(&m->retained, __ATOMIC_RELAXED);
        if (now > peak) peak = now;
    }
    /* a pool releases for the total only with min(perPool, total) / 16 of its own, so the sum can go
     * a few frames over */
    CHECK(peak <= 1 * MIB + 4 * m->totalMin);
    /* each release frees about a quarter of the limit */
    printf("total limit: %d releases, peak %.0f KiB\n", released, peak / 1024.0);
    CHECK(released >= (int)(40000ull * 16 * 420 * 3 / MIB) && released <= (int)(40000ull * 16 * 420 * 6 / MIB));
    cmdmem_destroy(m);

    /* single command buffer resets and the implicit reset of vkBeginCommandBuffer */
    m = cmdmem_create(1 * MIB, 64 * MIB, 1);
    cmdmem_pool_created(m, 1, pool, true);
    uint64_t two[2] = {cb, other};
    cmdmem_buffers_allocated(m, pool, 2, two);
    int cbReleases = 0, beginReleases = 0;
    for (int f = 0; f < 10000; f++) {
        cbReleases += cmdmem_buffer_reset(m, cb);
        cmdmem_recorded(m, cb, 16, true, false);
        cmdmem_end(m, cb);
        beginReleases += cmdmem_begin(m, other); /* no explicit reset */
        cmdmem_recorded(m, other, 16, true, false);
        cmdmem_end(m, other);
    }
    CHECK(cbReleases + beginReleases >= 2 * (int)(10000ull * 16 * 420 / MIB) - 2);
    CHECK(cmdmem_pool(m, pool)->retained <= 1 * MIB + 2 * 16 * 420);
    /* a pool without RESET_COMMAND_BUFFER_BIT never releases at begin */
    uint64_t pool2 = 0x5000, cb2 = 0x5001;
    cmdmem_pool_created(m, 1, pool2, false);
    cmdmem_buffers_allocated(m, pool2, 1, &cb2);
    for (int f = 0; f < 10000; f++) {
        CHECK(!cmdmem_begin(m, cb2));
        cmdmem_recorded(m, cb2, 16, true, false);
        cmdmem_end(m, cb2);
    }
    /* freeing a buffer takes its share off; the device going away drops everything of it */
    uint64_t before = cmdmem_pool(m, pool2)->retained;
    CHECK(before == 10000ull * 16 * 420);
    cmdmem_buffers_freed(m, 1, &cb2);
    CHECK(cmdmem_pool(m, pool2)->retained == 0);
    cmdmem_device_destroyed(m, 1);
    CHECK(!cmdmem_pool(m, pool) && !cmdmem_pool(m, pool2) && !cmdmem_buffer(m, cb));
    CHECK(__atomic_load_n(&m->retained, __ATOMIC_RELAXED) == 0);
    cmdmem_destroy(m);

    /* a pool reset with flags 0 hands the buffers' memory to the pool: releasing single buffers (or
     * freeing them) afterwards gives nothing back on the device, so they do not release or count */
    m = cmdmem_create(1 * MIB, 64 * MIB, 1);
    cmdmem_pool_created(m, 1, pool, true);
    uint64_t three[3] = {0x6001, 0x6002, 0x6003};
    cmdmem_buffers_allocated(m, pool, 3, three);
    int poolReleases = 0, wasted = 0;
    uint64_t real = 0, realPeak = 0;
    for (int f = 0; f < 20000; f++) {
        if (cmdmem_pool_reset(m, pool)) {
            poolReleases++;
            real = 0;
        }
        for (int c = 0; c < 3; c++) {
            wasted += cmdmem_begin(m, three[c]);
            if (c == 2 && f % 3 == 0) wasted += cmdmem_buffer_reset(m, three[c]), cmdmem_begin(m, three[c]);
            cmdmem_recorded(m, three[c], c ? 1 : 40, true, false);
            cmdmem_end(m, three[c]);
            real += (c ? 1 : 40) * 420;
            if (real > realPeak) realPeak = real;
        }
    }
    CHECK(wasted == 0);
    CHECK(poolReleases >= (int)(20000ull * 42 * 420 / (MIB + 42 * 420)));
    CHECK(realPeak <= 1 * MIB + 2 * 42 * 420);
    CHECK(cmdmem_pool(m, pool)->retained == real && __atomic_load_n(&m->retained, __ATOMIC_RELAXED) == real);
    cmdmem_destroy(m);

    /* idle pools holding most of the total (DXVK's recycled command lists) do not make the active
     * pool release at every reset: it needs min(perPool, total) / 16 of its own */
    m = cmdmem_create(16 * MIB, 32 * MIB, 0);
    uint64_t idle[4] = {0x7100, 0x7200, 0x7300, 0x7400};
    for (int p = 0; p < 4; p++) {
        uint64_t c = idle[p] + 1;
        cmdmem_pool_created(m, 1, idle[p], false);
        cmdmem_buffers_allocated(m, idle[p], 1, &c);
    }
    for (int p = 0; p < 3; p++)
        for (int f = 0; f < 1900; f++) { /* 7.6 MiB each, below the per-pool limit */
            CHECK(!cmdmem_pool_reset(m, idle[p]));
            record(m, idle[p] + 1, 10, true);
        }
    CHECK(__atomic_load_n(&m->retained, __ATOMIC_RELAXED) == 3 * 1900 * 4200ull);
    /* more recordings bring the sum to just below the total, then they sit idle */
    for (int f = 0; __atomic_load_n(&m->retained, __ATOMIC_RELAXED) + 4200 < 32 * MIB && f < 100000; f++) {
        CHECK(!cmdmem_pool_reset(m, idle[f % 3]));
        record(m, idle[f % 3] + 1, 10, true);
    }
    int active = 0;
    for (int f = 0; f < 10000; f++) {
        active += cmdmem_pool_reset(m, idle[3]);
        record(m, idle[3] + 1, 1, true);
    }
    CHECK(active <= 10000 / (int)(1 * MIB / 420) + 1);
    CHECK(active >= 1);
    cmdmem_destroy(m);

    /* a full table falls back to every 32nd reset */
    m = cmdmem_create(1 * MIB, 64 * MIB, 0);
    int created = 0;
    for (uint64_t p = 1; p <= 4096 && !__atomic_load_n(&m->full, __ATOMIC_RELAXED); p++, created++) cmdmem_pool_created(m, 1, p << 4, false);
    CHECK(__atomic_load_n(&m->full, __ATOMIC_RELAXED));
    int fallback = 0;
    for (int i = 0; i < 320; i++) fallback += cmdmem_pool_reset(m, 0xdead0000);
    CHECK(fallback == 10);
    cmdmem_destroy(m);

    /* threads: lookups without the lock while others insert and remove */
    m = cmdmem_create(1 * MIB, 4 * MIB, 0);
    pthread_t threads[8];
    Worker workers[8];
    for (int t = 0; t < 8; t++) {
        workers[t] = (Worker){m, (uint64_t)(t + 1) << 20, 20000};
        pthread_create(&threads[t], NULL, worker, &workers[t]);
    }
    for (int t = 0; t < 8; t++) pthread_join(threads[t], NULL);
    CHECK(__atomic_load_n(&m->retained, __ATOMIC_RELAXED) == 0);
    CHECK(!__atomic_load_n(&m->full, __ATOMIC_RELAXED));
    CHECK(__atomic_load_n(&m->releases, __ATOMIC_RELAXED) > 0);
    cmdmem_destroy(m);

    if (failures) {
        printf("CMDMEM-FAIL (%d)\n", failures);
        return 1;
    }
    printf("CMDMEM-PASS\n");
    return 0;
}
