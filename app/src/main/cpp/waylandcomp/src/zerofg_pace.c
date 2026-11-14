#include "zerofg_pace.h"

#include <math.h>

void zfg_period_reset(struct zfg_period *p) {
    p->n = 0;
    p->next = 0;
}

void zfg_period_add(struct zfg_period *p, int64_t interval) {
    if (interval <= 0) return;
    p->iv[p->next] = interval;
    p->next = (p->next + 1) % ZFG_PERIOD_HIST;
    if (p->n < ZFG_PERIOD_HIST) p->n++;
}

int64_t zfg_period_get(const struct zfg_period *p) {
    if (p->n <= 0) return 0;
    int64_t sum = 0, lo = p->iv[0], hi = p->iv[0];
    for (int i = 0; i < p->n; i++) {
        sum += p->iv[i];
        if (p->iv[i] < lo) lo = p->iv[i];
        if (p->iv[i] > hi) hi = p->iv[i];
    }
    if (p->n >= 4) return (sum - lo - hi) / (p->n - 2);
    return sum / p->n;
}

int zfg_is_gap(int64_t interval, int64_t period) {
    int64_t limit = 4 * period;
    if (limit < ZFG_GAP_MIN_NS) limit = ZFG_GAP_MIN_NS;
    return interval > limit;
}

int64_t zfg_lead_update(int64_t lead, int64_t ready_delay) {
    if (ready_delay <= 0) return lead;
    if (lead <= 0 || ready_delay >= lead) return ready_delay;
    return lead - (lead - ready_delay) / 8;
}

struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack) {
    const int64_t half = period > 0 ? period / 2 : 0;
    const int64_t lo = arrival + (lead > 0 ? lead : 0), hi = lo + (slack > 0 ? slack : 0);
    int64_t s = prev_b ? prev_b + half : lo;
    if (s < lo) s = lo;
    if (s > hi) s = hi;
    struct zfg_slots r = {s, s + half};
    return r;
}

int64_t zfg_desired(int64_t target, int64_t refresh_period) {
    return refresh_period > 0 ? target - refresh_period / 2 : target;
}

float zfg_vote(int64_t period, float refresh_hz) {
    if (period <= 0) return 0.0f;
    double fps = 2e9 / (double)period;
    if (refresh_hz > 0.0f && fps > refresh_hz) fps = refresh_hz;
    return (float)floor(fps + 0.5);
}

int zfg_too_fast(int64_t period, float refresh_hz) {
    if (period <= 0 || refresh_hz <= 0.0f) return 0;
    return 2e9 / (double)period > (double)refresh_hz * 1.05;
}
