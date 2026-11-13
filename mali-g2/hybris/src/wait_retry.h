/*
 * [mali-g2] Waits the driver ends with a result a wait may not return, or with VK_TIMEOUT before the
 * caller's deadline, are waited on again (guards.c). Behaviour after MaliBridge's wait_retry (MK-01;
 * re-implemented, not copied): Arm's driver waits with ppoll() and does not restart it when a signal
 * interrupts it, and the wait fails with VK_ERROR_INITIALIZATION_FAILED (Mali-G925 under
 * vkd3d-proton: vkWaitSemaphores every 7-10 s); DXVK takes anything but VK_SUCCESS from its
 * timeline waits as a lost device. No Vulkan calls: the clock, the naps and the waits are callbacks
 * (tests/guards_test.c fakes them).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_HYBRIS_WAIT_RETRY_H
#define DROIDDECK_HYBRIS_WAIT_RETRY_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define WR_MS 1000000ull
#define WR_SEC 1000000000ull
#define WR_SLACK_NS (2 * WR_MS)          /* a VK_TIMEOUT this close to the deadline is the caller's */
#define WR_QUICK_NS (2 * WR_MS)          /* a try that ends sooner is followed by a nap */
#define WR_NAP_FIRST_NS (WR_MS / 2)
#define WR_NAP_LAST_NS (20 * WR_MS)
#define WR_DEFAULT_CAP_NS (30 * WR_SEC)

typedef struct WaitOp {
    VkResult (*wait)(void* ctx, uint64_t timeout); /* the blocking call (the idle waits ignore the timeout) */
    VkResult (*poll)(void* ctx);                   /* the same without blocking, or NULL */
    void* ctx;
    uint64_t timeout; /* the caller's; UINT64_MAX: none */
    uint64_t start;   /* when the caller's call began */
    uint64_t capNs;   /* how long answers that are not whole waits are retried */
    uint64_t (*now)(void);
    void (*nap)(uint64_t ns);
} WaitOp;

typedef struct WaitOutcome {
    VkResult result;
    uint32_t tries;     /* waits made after the first */
    uint64_t retriedNs; /* time spent after the first */
    bool keptFailing;   /* the result is the driver's illegal one */
} WaitOutcome;

/* Results a wait may return: success, VK_TIMEOUT, VK_NOT_READY (any status >= 0), device lost and
 * the two out-of-memory errors. */
static inline bool wr_legal(VkResult r) {
    switch (r) {
    case VK_ERROR_DEVICE_LOST:
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return true;
    default:
        return r >= 0;
    }
}

/* Whether the driver's answer `r`, `took` ns after the call began, goes to wr_retry. */
static inline bool wr_needed(VkResult r, uint64_t took, uint64_t timeout) {
    if (!wr_legal(r)) return true;
    if (r != VK_TIMEOUT || timeout <= WR_SLACK_NS) return false;
    return timeout == UINT64_MAX || took < timeout - WR_SLACK_NS;
}

static inline uint64_t wr_after(uint64_t t, uint64_t ns) {
    return ns > UINT64_MAX - t ? UINT64_MAX : t + ns;
}

/* DROIDDECK_HYBRIS_WAIT_RETRY_S in ns: seconds, saturated; `fallback` when it is not a number. */
static inline uint64_t wr_parse_cap(const char* text, uint64_t fallback) {
    if (!text || *text < '0' || *text > '9') return fallback;
    uint64_t s = 0;
    for (; *text >= '0' && *text <= '9'; text++) {
        uint64_t digit = (uint64_t)(*text - '0');
        if (s > (UINT64_MAX / WR_SEC - digit) / 10) return UINT64_MAX;
        s = s * 10 + digit;
    }
    return *text ? fallback : s * WR_SEC;
}

/* Waits again after the driver's answer `first`. Each round polls first (what was signalled
 * meanwhile ends it with VK_SUCCESS; a legal error ends it with that error), then waits capNs, or
 * until the caller's deadline when that is sooner (after the deadline the caller gets VK_TIMEOUT). A
 * whole wait (one that used its whole timeout and ended with VK_TIMEOUT: the driver waits as it
 * should) is followed by another; after capNs of answers that are not, the caller gets the
 * driver's last illegal result (VK_TIMEOUT if it only timed out early). A driver that answers at
 * once is given naps of 0.5 ms, doubling up to 20 ms. */
static inline WaitOutcome wr_retry(const WaitOp* w, VkResult first) {
    WaitOutcome out = {first, 0, 0, false};
    const uint64_t began = w->now();
    const bool deadline = w->timeout != UINT64_MAX && w->timeout <= UINT64_MAX - w->start;
    const uint64_t due = deadline ? w->start + w->timeout : UINT64_MAX;
    uint64_t failingSince = began;
    VkResult bad = first;
    uint64_t nap = WR_NAP_FIRST_NS;
    for (;;) {
        VkResult r = w->poll ? w->poll(w->ctx) : VK_TIMEOUT;
        if (r == VK_SUCCESS || (r < 0 && wr_legal(r))) {
            out.result = r;
            break;
        }
        uint64_t now = w->now();
        if (now >= due) {
            out.result = VK_TIMEOUT;
            break;
        }
        uint64_t failing = now - failingSince;
        if (failing >= w->capNs) {
            out.result = bad;
            break;
        }
        uint64_t segment = due - now < w->capNs ? due - now : w->capNs;
        out.tries++;
        r = w->wait(w->ctx, segment);
        if (r != VK_TIMEOUT && wr_legal(r)) {
            out.result = r;
            break;
        }
        uint64_t after = w->now();
        if (r == VK_TIMEOUT && after - now + WR_SLACK_NS >= segment) {
            bad = VK_TIMEOUT;
            failingSince = after;
            nap = WR_NAP_FIRST_NS;
            continue;
        }
        if (r != VK_TIMEOUT) bad = r;
        if (after - now >= WR_QUICK_NS) {
            nap = WR_NAP_FIRST_NS;
            continue;
        }
        uint64_t stop = wr_after(failingSince, w->capNs) < due ? wr_after(failingSince, w->capNs) : due;
        if (after < stop) w->nap(stop - after < nap ? stop - after : nap);
        nap = nap * 2 > WR_NAP_LAST_NS ? WR_NAP_LAST_NS : nap * 2;
    }
    out.retriedNs = w->now() - began;
    out.keptFailing = !wr_legal(out.result);
    return out;
}

#endif
