/*
 * Unit tests for guards.c's pure logic: heap_cap.h, wait_retry.h (fake clock and fake waits) and
 * query_stuck.h. No GPU, no Vulkan library (headers only):
 *   cc -O2 -Wall -Wextra -I../src -o out/tests/guards_test tests/guards_test.c -lpthread && out/tests/guards_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap_cap.h"
#include "query_stuck.h"
#include "wait_retry.h"

static int failures, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

/* ---- heap cap ---- */

static void testHeapDefault(void) {
    CHECK(heapcap_default_mb(0) == 0, "unknown RAM: no cap");
    CHECK(heapcap_default_mb(16000) == 0, "16 GB: no cap");
    CHECK(heapcap_default_mb(12801) == 0, "just over the threshold");
    CHECK(heapcap_default_mb(12800) == 4096, "at the threshold: %u", heapcap_default_mb(12800));
    CHECK(heapcap_default_mb(11377) == 4096, "this tablet (11377 MB): %u", heapcap_default_mb(11377));
    CHECK(heapcap_default_mb(7500) == 3584, "an 8 GB device: %u", heapcap_default_mb(7500));
    CHECK(heapcap_default_mb(5600) == 2560, "a 6 GB device: %u", heapcap_default_mb(5600));
    CHECK(heapcap_default_mb(3000) == 2048, "small: the floor: %u", heapcap_default_mb(3000));
}

static void testHeapFollowFree(void) {
    CHECK(heapcap_follow_free(0, 1000) == 0, "no cap stays none");
    CHECK(heapcap_follow_free(4096, 0) == 4096, "free unknown: the cap");
    CHECK(heapcap_follow_free(4096, 9000) == 4096, "plenty free: the cap");
    CHECK(heapcap_follow_free(4096, 3300) == 3072, "3300 MB free: %u", heapcap_follow_free(4096, 3300));
    CHECK(heapcap_follow_free(4096, 1200) == 2048, "little free: the floor: %u", heapcap_follow_free(4096, 1200));
    CHECK(heapcap_follow_free(2048, 1200) == 2048, "floor equal to the cap");
}

static void testHeapParse(void) {
    const char* meminfo = "MemTotal:       11650560 kB\nMemFree:          523412 kB\nMemAvailable:    3412000 kB\nBuffers: 1 kB\n";
    CHECK(heapcap_meminfo_kb(meminfo, "MemTotal") == 11650560, "MemTotal");
    CHECK(heapcap_meminfo_kb(meminfo, "MemAvailable") == 3412000, "MemAvailable");
    CHECK(heapcap_meminfo_kb(meminfo, "MemFree") == 523412, "MemFree");
    CHECK(heapcap_meminfo_kb(meminfo, "Mem") == 0, "a prefix is not a key");
    CHECK(heapcap_meminfo_kb(meminfo, "SwapTotal") == 0, "absent");
    CHECK(heapcap_meminfo_kb("", "MemTotal") == 0, "empty");
    CHECK(heapcap_parse_mb("4096") == 4096, "number");
    CHECK(heapcap_parse_mb("0") == 0, "zero");
    CHECK(heapcap_parse_mb("") == -1 && heapcap_parse_mb(NULL) == -1, "empty");
    CHECK(heapcap_parse_mb("-1") == -1 && heapcap_parse_mb("4g") == -1 && heapcap_parse_mb(" 4") == -1, "not plain numbers");
    CHECK(heapcap_parse_mb("99999999999") == -1, "too big");
}

static void testHeapApply(void) {
    VkPhysicalDeviceMemoryProperties m = {0};
    m.memoryHeapCount = 2;
    m.memoryHeaps[0].size = 11377ull << 20;
    m.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    m.memoryHeaps[1].size = 8192ull << 20; /* not device-local: left alone */
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {0};
    b.heapBudget[0] = 9000ull << 20;
    b.heapUsage[0] = 5000ull << 20;
    b.heapBudget[1] = 8000ull << 20;
    uint32_t changed = heapcap_apply(&m, &b, 4096ull << 20);
    CHECK(changed == 1, "only heap 0 changed: %#x", changed);
    CHECK(m.memoryHeaps[0].size == 4096ull << 20, "heap 0 capped");
    CHECK(m.memoryHeaps[1].size == 8192ull << 20, "heap 1 unchanged");
    CHECK(b.heapBudget[0] == 4096ull << 20, "budget held under the heap");
    CHECK(b.heapUsage[0] == 5000ull << 20, "usage stays the driver's");
    CHECK(b.heapBudget[1] == 8000ull << 20, "a budget under its heap stays");
    VkPhysicalDeviceMemoryProperties small = {0};
    small.memoryHeapCount = 1;
    small.memoryHeaps[0].size = 2048ull << 20;
    small.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    CHECK(heapcap_apply(&small, NULL, 4096ull << 20) == 0 && small.memoryHeaps[0].size == 2048ull << 20, "a smaller heap stays");
    CHECK(heapcap_apply(&small, NULL, 0) == 0, "no cap");
}

/* ---- wait retry ---- */

typedef struct {
    uint64_t now;
    /* each wait: what it returns, and how long it blocks (a VK_TIMEOUT always uses the whole timeout
     * when `full` is set) */
    VkResult results[64];
    uint64_t blocks[64];
    int count, next;
    bool full;
    VkResult poll; /* what each poll answers */
    int polls, waits;
    uint64_t lastTimeout;
    uint64_t napped;
} Fake;

static Fake fake;

static uint64_t fakeNow(void) {
    return fake.now;
}

static void fakeNap(uint64_t ns) {
    fake.napped += ns;
    fake.now += ns;
}

static VkResult fakeWait(void* ctx, uint64_t timeout) {
    fake.waits++;
    fake.lastTimeout = timeout;
    int i = fake.next < fake.count ? fake.next++ : fake.count - 1;
    VkResult r = fake.results[i];
    uint64_t block = fake.blocks[i];
    if (r == VK_TIMEOUT && fake.full) block = timeout;
    if (block > timeout && timeout != UINT64_MAX) block = timeout, r = VK_TIMEOUT;
    fake.now += block;
    return r;
}

static VkResult fakePoll(void* ctx) {
    fake.polls++;
    return fake.poll;
}

static void fakeReset(void) {
    memset(&fake, 0, sizeof(fake));
    fake.now = 1000 * WR_SEC;
    fake.poll = VK_TIMEOUT;
}

static WaitOp op(uint64_t timeout, bool poll) {
    WaitOp w = {fakeWait, poll ? fakePoll : NULL, NULL, timeout, fake.now, WR_DEFAULT_CAP_NS, fakeNow, fakeNap};
    return w;
}

static void testWaitLegal(void) {
    CHECK(wr_legal(VK_SUCCESS) && wr_legal(VK_TIMEOUT) && wr_legal(VK_NOT_READY), "statuses");
    CHECK(wr_legal(VK_ERROR_DEVICE_LOST) && wr_legal(VK_ERROR_OUT_OF_HOST_MEMORY) && wr_legal(VK_ERROR_OUT_OF_DEVICE_MEMORY), "errors");
    CHECK(!wr_legal(VK_ERROR_INITIALIZATION_FAILED) && !wr_legal(VK_ERROR_UNKNOWN), "-3 and unknown");
    CHECK(wr_needed(VK_ERROR_INITIALIZATION_FAILED, 0, 0), "-3 on a poll");
    CHECK(!wr_needed(VK_SUCCESS, 0, UINT64_MAX) && !wr_needed(VK_ERROR_DEVICE_LOST, 0, UINT64_MAX), "legal answers");
    CHECK(wr_needed(VK_TIMEOUT, 10 * WR_MS, UINT64_MAX), "timeout without a timeout");
    CHECK(wr_needed(VK_TIMEOUT, 100 * WR_MS, 5 * WR_SEC), "early timeout");
    CHECK(!wr_needed(VK_TIMEOUT, 5 * WR_SEC, 5 * WR_SEC), "timeout at the deadline");
    CHECK(!wr_needed(VK_TIMEOUT, 5 * WR_SEC - WR_MS, 5 * WR_SEC), "within the slack");
    CHECK(!wr_needed(VK_TIMEOUT, 0, 0) && !wr_needed(VK_TIMEOUT, 0, WR_MS), "polls and tiny timeouts");
}

/* The DXVK case: a 5 s timeline wait interrupted after 2 s with -3, done at 3.5 s. */
static void testInterruptedThenDone(void) {
    fakeReset();
    WaitOp w = op(5 * WR_SEC, true);
    fake.now += 2 * WR_SEC;
    fake.results[0] = VK_SUCCESS;
    fake.blocks[0] = 1500 * WR_MS;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_SUCCESS, "result %d", o.result);
    CHECK(o.tries == 1 && fake.polls == 1, "tries %u polls %d", o.tries, fake.polls);
    CHECK(fake.lastTimeout == 3 * WR_SEC, "the rest of the caller's timeout: %llu", (unsigned long long)fake.lastTimeout);
    CHECK(!o.keptFailing, "not failing");
}

static void testSignalledMeanwhile(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, true);
    fake.poll = VK_SUCCESS;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_SUCCESS && o.tries == 0 && fake.waits == 0, "the poll ends it: %d %u", o.result, o.tries);
}

static void testLegalErrorFromPoll(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, true);
    fake.poll = VK_ERROR_DEVICE_LOST;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_ERROR_DEVICE_LOST && fake.waits == 0, "a lost device from the poll: %d", o.result);
}

/* The driver keeps failing at once: the caller's deadline ends it with VK_TIMEOUT, with naps. */
static void testDeadline(void) {
    fakeReset();
    WaitOp w = op(100 * WR_MS, true);
    fake.results[0] = VK_ERROR_INITIALIZATION_FAILED;
    fake.blocks[0] = 0;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_TIMEOUT, "deadline: %d", o.result);
    CHECK(fake.now == w.start + 100 * WR_MS, "ends at the deadline: %llu", (unsigned long long)(fake.now - w.start));
    CHECK(o.tries < 20, "not spun on: %u tries", o.tries);
    CHECK(fake.napped > 90 * WR_MS, "napped %llu", (unsigned long long)fake.napped);
    CHECK(!o.keptFailing, "VK_TIMEOUT is legal");
}

/* Without a deadline, a driver that keeps failing: its error after the cap. */
static void testCap(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, true);
    w.capNs = 2 * WR_SEC;
    fake.results[0] = VK_ERROR_INITIALIZATION_FAILED;
    fake.blocks[0] = 300 * WR_MS;
    fake.count = 1;
    uint64_t began = fake.now;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_ERROR_INITIALIZATION_FAILED && o.keptFailing, "the driver's error: %d", o.result);
    CHECK(fake.now - began >= 2 * WR_SEC && fake.now - began < 2 * WR_SEC + 400 * WR_MS, "about the cap: %llu",
          (unsigned long long)(fake.now - began));
    CHECK(o.tries >= 6 && o.tries <= 8, "tries %u", o.tries);
    CHECK(fake.napped == 0, "no naps when each try blocks");
}

/* An early VK_TIMEOUT with no deadline: waited on until done. */
static void testEarlyTimeout(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, true);
    fake.results[0] = VK_TIMEOUT;
    fake.blocks[0] = 1 * WR_SEC;
    fake.results[1] = VK_SUCCESS;
    fake.blocks[1] = 200 * WR_MS;
    fake.count = 2;
    WaitOutcome o = wr_retry(&w, VK_TIMEOUT);
    CHECK(o.result == VK_SUCCESS && o.tries == 2, "done: %d after %u", o.result, o.tries);
}

/* Early VK_TIMEOUTs only, no deadline: VK_TIMEOUT after the cap, not an error. */
static void testEarlyTimeoutCap(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, true);
    w.capNs = 3 * WR_SEC;
    fake.results[0] = VK_TIMEOUT;
    fake.blocks[0] = 500 * WR_MS;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_TIMEOUT);
    CHECK(o.result == VK_TIMEOUT && !o.keptFailing, "VK_TIMEOUT at the cap: %d", o.result);
}

/* A finite timeout that the driver ends early: the rest of it is waited, then VK_TIMEOUT. */
static void testEarlyTimeoutFinite(void) {
    fakeReset();
    WaitOp w = op(5 * WR_SEC, true);
    fake.now += 1 * WR_SEC;
    fake.results[0] = VK_TIMEOUT;
    fake.full = true;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_TIMEOUT);
    CHECK(o.result == VK_TIMEOUT && o.tries == 1, "%d after %u", o.result, o.tries);
    CHECK(fake.now == w.start + 5 * WR_SEC, "at the deadline");
}

/* Idle waits have no poll: a -3 then success. */
static void testNoPoll(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, false);
    fake.results[0] = VK_ERROR_INITIALIZATION_FAILED;
    fake.blocks[0] = 0;
    fake.results[1] = VK_ERROR_INITIALIZATION_FAILED;
    fake.blocks[1] = 0;
    fake.results[2] = VK_SUCCESS;
    fake.blocks[2] = 5 * WR_MS;
    fake.count = 3;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_SUCCESS && o.tries == 3 && fake.polls == 0, "%d after %u", o.result, o.tries);
    CHECK(fake.napped == WR_NAP_FIRST_NS * 3, "naps 0.5 + 1 ms: %llu", (unsigned long long)fake.napped);
}

/* A legal error from a retry goes to the caller. */
static void testLegalErrorFromRetry(void) {
    fakeReset();
    WaitOp w = op(UINT64_MAX, true);
    fake.results[0] = VK_ERROR_DEVICE_LOST;
    fake.blocks[0] = 10 * WR_MS;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(o.result == VK_ERROR_DEVICE_LOST && !o.keptFailing && o.tries == 1, "%d", o.result);
}

/* The naps double up to 20 ms and never pass the deadline. */
static void testNapGrowth(void) {
    fakeReset();
    WaitOp w = op(200 * WR_MS, false);
    fake.results[0] = VK_ERROR_INITIALIZATION_FAILED;
    fake.blocks[0] = 0;
    fake.count = 1;
    WaitOutcome o = wr_retry(&w, VK_ERROR_INITIALIZATION_FAILED);
    /* 0.5 + 1 + 2 + 4 + 8 + 16 = 31.5 ms, then 20 ms each: 9 more to reach 200 ms */
    CHECK(o.result == VK_TIMEOUT, "%d", o.result);
    CHECK(o.tries == 15, "tries %u", o.tries);
    CHECK(fake.now == w.start + 200 * WR_MS, "at the deadline: %llu", (unsigned long long)(fake.now - w.start));
}

/* ---- stuck queries ---- */

static void testQueryRuns(void) {
    QsQuery q = {0};
    bool crossed;
    uint64_t t = 1000 * WR_SEC, limit = 500 * WR_MS;
    CHECK(qs_not_ready(&q, t, limit, &crossed) == 0 && !crossed, "a run starts");
    CHECK(qs_not_ready(&q, t + 100 * WR_MS, limit, &crossed) == 100 * WR_MS && !crossed, "100 ms");
    CHECK(qs_not_ready(&q, t + 500 * WR_MS, limit, &crossed) == 500 * WR_MS && crossed, "crossed at 500 ms");
    CHECK(qs_not_ready(&q, t + 700 * WR_MS, limit, &crossed) == 700 * WR_MS && !crossed, "crossed once");
    CHECK(q.polls == 4, "polls %u", q.polls);
    CHECK(qs_end_run(&q, t + 800 * WR_MS) == 800 * WR_MS, "ended after 800 ms");
    CHECK(qs_end_run(&q, t + 900 * WR_MS) == 0, "no run");
    /* a short run ends unremarked */
    qs_not_ready(&q, t + 1 * WR_SEC, limit, &crossed);
    CHECK(qs_end_run(&q, t + 1 * WR_SEC + 10 * WR_MS) == 0, "short run");
    /* a reset ends a long run and the next "not ready" starts a new one */
    qs_not_ready(&q, t + 2 * WR_SEC, limit, &crossed);
    qs_not_ready(&q, t + 3 * WR_SEC, limit, &crossed);
    CHECK(crossed, "crossed");
    CHECK(qs_reset(&q, t + 3 * WR_SEC + 1) == WR_SEC + 1, "reset ends it");
    CHECK(qs_not_ready(&q, t + 4 * WR_SEC, limit, &crossed) == 0 && !crossed, "a new run after the reset");
    /* a reset recorded while a run goes on, without a call that ends the run: the run restarts */
    QsQuery r = {0};
    qs_not_ready(&r, t, limit, &crossed);
    QS_STORE(&r.resetNs, t + 100 * WR_MS);
    CHECK(qs_not_ready(&r, t + 600 * WR_MS, limit, &crossed) == 0 && !crossed, "run older than the reset restarts");
}

static void testQueryWriteVisible(void) {
    uint32_t d32[2] = {7, 7};
    CHECK(qs_write_visible(d32, 4, 0, 1) && d32[0] == 1 && d32[1] == 7, "32-bit, no availability");
    CHECK(qs_write_visible(d32, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 1) && d32[0] == 1 && d32[1] == 1, "32-bit with availability");
    CHECK(!qs_write_visible(d32, 4, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 1), "too small");
    uint64_t d64[2] = {7, 7};
    CHECK(qs_write_visible(d64, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 1) && d64[0] == 1 && d64[1] == 1, "64");
    CHECK(!qs_write_visible(d64, 12, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 1), "64 too small");
    CHECK(!qs_write_visible(NULL, 16, 0, 1), "no data");
}

static void testQueryVisibleDue(void) {
    QsTables* t = calloc(1, sizeof(*t));
    pthread_mutex_init(&t->lock, NULL);
    uint64_t limit = 500 * WR_MS, t0 = 1000 * WR_SEC, primary = 0;
    const uint64_t cbPrimary = 0x1000, cbSecondary = 0x2000, cbOther = 0x3000;
    QsQuery q = {0};
    bool crossed;
    qs_reset(&q, t0);
    qs_begin(t, cbPrimary);
    QS_STORE(&q.endRec, qs_current(t, cbPrimary));
    QS_STORE(&q.endNs, t0 + 1 * WR_MS);
    /* not submitted: never */
    qs_not_ready(&q, t0 + 2 * WR_MS, limit, &crossed);
    CHECK(!qs_visible_due(t, &q, t0 + 2 * WR_SEC, limit, &primary), "never submitted");
    qs_submitted(t, &cbPrimary, 1, t0 + 10 * WR_MS);
    CHECK(!qs_visible_due(t, &q, t0 + 400 * WR_MS, limit, &primary), "submitted less than the limit ago");
    CHECK(qs_visible_due(t, &q, t0 + 600 * WR_MS, limit, &primary), "due");
    CHECK(primary == qs_current(t, cbPrimary), "its primary");
    /* polled not ready only briefly: not due unless a sibling was answered */
    QsQuery s = {0};
    qs_reset(&s, t0);
    QS_STORE(&s.endRec, qs_current(t, cbPrimary));
    QS_STORE(&s.endNs, t0 + 2 * WR_MS);
    qs_not_ready(&s, t0 + 900 * WR_MS, limit, &crossed);
    CHECK(!qs_visible_due(t, &s, t0 + 950 * WR_MS, limit, &primary), "a fresh run");
    qs_mark_visible(t, primary);
    CHECK(qs_visible_due(t, &s, t0 + 950 * WR_MS, limit, &primary), "a sibling was answered");
    /* reset after its End: never */
    qs_reset(&s, t0 + 960 * WR_MS);
    qs_not_ready(&s, t0 + 970 * WR_MS, limit, &crossed);
    CHECK(!qs_visible_due(t, &s, t0 + 3 * WR_SEC, limit, &primary), "reset after the End");
    /* in a secondary: due through the primary that executed it */
    QsQuery x = {0};
    qs_reset(&x, t0);
    qs_begin(t, cbSecondary);
    QS_STORE(&x.endRec, qs_current(t, cbSecondary));
    QS_STORE(&x.endNs, t0 + 1 * WR_MS);
    qs_not_ready(&x, t0 + 5 * WR_MS, limit, &crossed);
    qs_begin(t, cbOther);
    CHECK(!qs_visible_due(t, &x, t0 + 2 * WR_SEC, limit, &primary), "secondary not executed");
    qs_executed(t, cbOther, &cbSecondary, 1);
    CHECK(!qs_visible_due(t, &x, t0 + 2 * WR_SEC, limit, &primary), "its primary not submitted");
    qs_submitted(t, &cbOther, 1, t0 + 20 * WR_MS);
    CHECK(qs_visible_due(t, &x, t0 + 2 * WR_SEC, limit, &primary) && primary == qs_current(t, cbOther), "secondary due");
    /* the primary was submitted before the End was recorded (an earlier recording's submission) */
    QsQuery y = {0};
    qs_reset(&y, t0);
    qs_begin(t, cbPrimary);
    qs_submitted(t, &cbPrimary, 1, t0 + 30 * WR_MS);
    QS_STORE(&y.endRec, qs_current(t, cbPrimary));
    QS_STORE(&y.endNs, t0 + 40 * WR_MS);
    qs_not_ready(&y, t0 + 50 * WR_MS, limit, &crossed);
    CHECK(!qs_visible_due(t, &y, t0 + 3 * WR_SEC, limit, &primary), "submitted before its End");
    /* a recording replaced in the table: not answered */
    QsQuery z = {0};
    qs_reset(&z, t0);
    qs_begin(t, cbPrimary);
    QS_STORE(&z.endRec, qs_current(t, cbPrimary));
    QS_STORE(&z.endNs, t0 + 60 * WR_MS);
    qs_submitted(t, &cbPrimary, 1, t0 + 70 * WR_MS);
    qs_not_ready(&z, t0 + 80 * WR_MS, limit, &crossed);
    CHECK(qs_visible_due(t, &z, t0 + 3 * WR_SEC, limit, &primary), "due before the table wraps");
    for (uint32_t i = 0; i < QS_RECS; i++) qs_begin(t, 0x9000 + i * 8);
    CHECK(!qs_visible_due(t, &z, t0 + 3 * WR_SEC, limit, &primary), "gone after the table wraps");
    pthread_mutex_destroy(&t->lock);
    free(t);
}

int main(void) {
    testHeapDefault();
    testHeapFollowFree();
    testHeapParse();
    testHeapApply();
    testWaitLegal();
    testInterruptedThenDone();
    testSignalledMeanwhile();
    testLegalErrorFromPoll();
    testDeadline();
    testCap();
    testEarlyTimeout();
    testEarlyTimeoutCap();
    testEarlyTimeoutFinite();
    testNoPoll();
    testLegalErrorFromRetry();
    testNapGrowth();
    testQueryRuns();
    testQueryWriteVisible();
    testQueryVisibleDue();
    printf("guards_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
