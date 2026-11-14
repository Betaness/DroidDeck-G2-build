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

int64_t zfg_target_lead(int64_t measured, int64_t period, int64_t margin, int64_t latch, int64_t refresh) {
    int64_t ready = (measured > 0 ? measured : period / 2) + (margin > 0 ? margin : 0);
    if (period > 0 && ready > 2 * period) ready = 2 * period;
    return ready + (latch > 0 ? latch : 0) + (refresh > 0 ? refresh / 2 : 0);
}

int64_t zfg_slack(int64_t period, int64_t refresh) {
    const int64_t q = period > 0 ? period / 4 : 0;
    return q > refresh ? q : refresh > 0 ? refresh : 0;
}

struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                              int64_t refresh) {
    const int64_t half = period > 0 ? period / 2 : 0;
    const int64_t gap = refresh > 0 ? refresh : 0;
    const int64_t step = half > gap ? half : gap;
    const int64_t lo = arrival + (lead > 0 ? lead : 0), hi = lo + (slack > 0 ? slack : 0);
    int64_t s = prev_b ? prev_b + half : lo;
    if (s < lo) s = lo;
    if (s > hi) s = hi;
    struct zfg_slots r = {s, s + step, 0};
    if (prev_b && s < prev_b + gap) {
        if (prev_b + gap <= hi) {
            r.s = prev_b + gap;
            r.b = r.s + step;
        } else {
            r.no_s = 1;
            r.s = 0;
            r.b = lo + half > prev_b + gap ? lo + half : prev_b + gap;
        }
    }
    return r;
}

int64_t zfg_desired(int64_t target, int64_t refresh_period) {
    return refresh_period > 0 ? target - refresh_period / 2 : target;
}

int64_t zfg_latch_deadline(int64_t target, int64_t refresh_period, int64_t latch) {
    return zfg_desired(target, refresh_period) - (latch > 0 ? latch : 0);
}

int64_t zfg_apply_at(int64_t desired, int64_t latch, int64_t margin) {
    return desired - (latch > 0 ? latch : 0) - (margin > 0 ? margin : 0);
}

int zfg_vote_div(int64_t period, float peak_hz, int prev_k) {
    if (period <= 0 || peak_hz <= 0.0f) return 0;
    const double out = 2e9 / (double)period, peak = peak_hz;
    int k = (int)floor(peak / out);
    if (k < 1) k = 1;
    if (prev_k <= 0 || prev_k == k) return k;
    if (prev_k > k) return out <= peak / prev_k * 1.02 ? prev_k : k;
    const int roomy = (int)floor(peak / (out * 1.05));
    return roomy > prev_k ? roomy : prev_k;
}

int zfg_too_fast(int64_t period, float peak_hz, int was_fast) {
    if (period <= 0 || peak_hz <= 0.0f) return 0;
    return 2e9 / (double)period > (double)peak_hz * (was_fast ? 1.03 : 1.05);
}
