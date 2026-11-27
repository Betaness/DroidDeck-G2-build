#include "zerofg_pace.h"

#include <math.h>
#include <string.h>

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

static int mult_of(int n) { return n < 2 ? 2 : n > ZFG_MULT_MAX ? ZFG_MULT_MAX : n; }

int64_t zfg_spacing(int64_t period, int n, int64_t refresh) {
    const int64_t share = period > 0 ? (int64_t)((double)period / mult_of(n) * ZFG_SPACING) : 0;
    return share > refresh ? share : refresh > 0 ? refresh : 0;
}

struct zfg_slots zfg_schedule_n(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                                int64_t refresh, int n) {
    n = mult_of(n);
    const int64_t q = period > 0 ? period / n : 0;
    const int64_t gap = zfg_spacing(period, n, refresh);
    const int64_t step = q > refresh ? q : refresh > 0 ? refresh : 0;
    const int64_t lo = arrival + (lead > 0 ? lead : 0), hi = lo + (slack > 0 ? slack : 0);
    int64_t s = prev_b ? prev_b + q : lo;
    if (s < lo) s = lo;
    if (s > hi) s = hi;
    struct zfg_slots r = {s, s + (n - 1) * step, 0, n, step};
    if (prev_b && s < prev_b + gap) {
        if (prev_b + gap <= hi) {
            r.s = prev_b + gap;
            r.b = r.s + (n - 1) * step;
        } else {
            r.no_s = 1;
            r.s = 0;
            const int64_t rest = lo + (n - 1) * q;
            r.b = rest > prev_b + gap ? rest : prev_b + gap;
            if (r.b > hi + (n - 1) * step) r.b = rest;
        }
    }
    return r;
}

struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                              int64_t refresh) {
    return zfg_schedule_n(arrival, prev_b, period, lead, slack, refresh, 2);
}

int64_t zfg_slot_s(const struct zfg_slots *t, int k) { return t->no_s ? 0 : t->s + (int64_t)k * t->step; }

int64_t zfg_desired(int64_t target, int64_t refresh_period) {
    return refresh_period > 0 ? target - refresh_period / 2 : target;
}

int64_t zfg_latch_deadline(int64_t target, int64_t refresh_period, int64_t latch) {
    return zfg_desired(target, refresh_period) - (latch > 0 ? latch : 0);
}

int64_t zfg_apply_at(int64_t desired, int64_t latch, int64_t margin) {
    return desired - (latch > 0 ? latch : 0) - (margin > 0 ? margin : 0);
}

/* Twice the real rate `out` is within (1 - below) .. (1 + above) of c / m for a whole m: c shows every output
 * for m refreshes. */
static int vote_fits(double c, double out, double below, double above) {
    const double m0 = floor(c / out);
    for (double m = m0 < 1.0 ? 1.0 : m0; m <= m0 + 1.0; m++) {
        const double fit = c / m;
        if (out >= fit * (1.0 - below) && out <= fit * (1.0 + above)) return 1;
    }
    return 0;
}

float zfg_vote_pick_n(int64_t period, int mult, const float *modes, int n, float peak_hz, float prev) {
    if (period <= 0 || peak_hz <= 0.0f) return 0.0f;
    const double out = mult_of(mult) * 1e9 / (double)period, peak = peak_hz;
    float cand[16];
    int nc = 0;
    for (int i = 0; modes && i < n && nc < 16; i++)
        if (modes[i] > 1.0f && modes[i] <= peak + 0.5) cand[nc++] = modes[i];
    if (!nc)
        for (int k = 1; k <= 8; k++) cand[nc++] = (float)(peak / k);
    float best = 0.0f, top = 0.0f;
    int kept = 0;
    for (int i = 0; i < nc; i++) {
        if (cand[i] > top) top = cand[i];
        if (prev > 0.0f && fabs(cand[i] - prev) < 0.5 &&
            vote_fits(cand[i], out, ZFG_VOTE_KEEP_BELOW, ZFG_VOTE_KEEP_ABOVE))
            kept = 1;
        if (vote_fits(cand[i], out, ZFG_VOTE_FIT, ZFG_VOTE_FIT) && (best <= 0.0f || cand[i] < best)) best = cand[i];
    }
    if (kept && !(best > 0.0f && best < prev)) return prev;
    return best > 0.0f ? best : top;
}

float zfg_vote_pick(int64_t period, const float *modes, int n, float peak_hz, float prev) {
    return zfg_vote_pick_n(period, 2, modes, n, peak_hz, prev);
}

void zfg_vote_reset(struct zfg_vote *v) {
    v->hz = 0.0f;
    v->away = 0;
    v->away_since = 0;
    v->period = 0;
}

float zfg_vote_step_n(struct zfg_vote *v, int64_t now, int64_t period, int mult, const float *modes, int n,
                      float peak_hz) {
    if (period > 0) v->period = v->period > 0 ? v->period + (period - v->period) / 8 : period;
    const float pick = zfg_vote_pick_n(v->period, mult, modes, n, peak_hz, v->hz);
    if (pick <= 0.0f || pick == v->hz) {
        v->away = 0;
        return v->hz;
    }
    if (v->hz <= 0.0f) {
        v->hz = pick;
        v->away = 0;
        return pick;
    }
    if (!v->away) {
        v->away = 1;
        v->away_since = now;
    }
    if (now - v->away_since >= ZFG_VOTE_HOLD_NS) {
        v->hz = pick;
        v->away = 0;
    }
    return v->hz;
}

float zfg_vote_step(struct zfg_vote *v, int64_t now, int64_t period, const float *modes, int n, float peak_hz) {
    return zfg_vote_step_n(v, now, period, 2, modes, n, peak_hz);
}

int zfg_too_fast(int64_t period, float peak_hz, int was_fast) {
    if (period <= 0 || peak_hz <= 0.0f) return 0;
    return 2e9 / (double)period > (double)peak_hz * (was_fast ? 1.03 : 1.05);
}

int zfg_fast_update(struct zfg_fast *f, int64_t now, int64_t period, float refresh_hz, float peak_hz) {
    if (period <= 0) {
        f->on = 0;
        f->over_since = 0;
        return 0;
    }
    const double real = 1e9 / (double)period;
    const int was = f->on;
    const int over_run = refresh_hz > 0.0f && 2.0 * real > (double)refresh_hz * (was ? 1.03 : 1.05);
    if (!over_run) f->over_since = 0;
    else if (!f->over_since) f->over_since = now;
    f->on = zfg_too_fast(period, peak_hz, was) ||
            (refresh_hz > 0.0f && real > (double)refresh_hz * (was ? 1.0 : 1.02)) ||
            (over_run && now - f->over_since >= ZFG_FAST_GRACE_NS);
    return f->on;
}

void zfg_mult_reset(struct zfg_mult *m) { memset(m, 0, sizeof(*m)); }

/* Whether `mult` times the real rate fits: below a rate (peak or running refresh rate, 0: unknown), and the GPU
 * time of its generated frames within the share of the period. keep: the limits that keep a multiplier in force
 * (else those that let one come back). */
static int fits_rate(int mult, double real, float hz, int keep) {
    return hz <= 0.0f || mult * real <= (double)hz * (keep ? 1.05 : 1.03);
}

static int fits_gpu(int mult, int64_t period, double gpu_ms, int keep) {
    return gpu_ms <= 0.0 || (mult - 1) * gpu_ms <= (double)period / 1e6 * ZFG_GPU_SHARE * (keep ? 1.0 : ZFG_GPU_RAISE);
}

int zfg_mult_update(struct zfg_mult *m, int64_t now, int want, int64_t period, float refresh_hz, float peak_hz,
                    double gpu_ms) {
    want = mult_of(want);
    if (!m->n || want != m->want) {
        m->n = m->want = want;
        m->raise = 0;
        m->over_since = 0;
        m->gpu_block = 0;
    }
    if (m->gpu_block && now >= m->gpu_block_until) m->gpu_block = 0;
    if (period <= 0) {
        m->vote_n = m->n;
        m->why = m->n < want ? m->why : ZFG_CAP_NONE;
        return m->n;
    }
    const double real = 1e9 / (double)period;
    int n = m->n;
    while (n > 2 && !fits_rate(n, real, peak_hz, 1)) n--;
    while (n > 2 && !fits_gpu(n, period, gpu_ms, 1)) {
        m->gpu_block = n;
        m->gpu_block_until = now + ZFG_GPU_BLOCK_NS;
        n--;
    }
    if (n > 2 && !fits_rate(n, real, refresh_hz, 1)) {
        if (!m->over_since) m->over_since = now;
        if (now - m->over_since >= ZFG_FAST_GRACE_NS)
            while (n > 2 && !fits_rate(n, real, refresh_hz, 1)) n--;
    }
    if (n != m->n) m->raise = 0;
    if (n > 2 && fits_rate(n, real, refresh_hz, 1)) m->over_since = 0;
    if (n == 2) m->over_since = 0;
    int up = 0;
    for (int k = want; k > n && !up; k--)
        if (fits_rate(k, real, peak_hz, 0) && fits_rate(k, real, refresh_hz, 0) && fits_gpu(k, period, gpu_ms, 0) &&
            !(m->gpu_block && k >= m->gpu_block))
            up = k;
    if (!up) m->raise = 0;
    else if (m->raise != up) {
        m->raise = up;
        m->raise_since = now;
    } else if (now - m->raise_since >= ZFG_MULT_RAISE_NS) {
        n = up;
        m->raise = 0;
        m->over_since = 0;
    }
    m->n = n;
    const int voted = m->vote_n;
    m->vote_n = n;
    for (int k = want; k > n; k--)
        if (fits_rate(k, real, peak_hz, k <= voted) && fits_gpu(k, period, gpu_ms, 1) &&
            !(m->gpu_block && k >= m->gpu_block)) {
            m->vote_n = k;
            break;
        }
    m->why = ZFG_CAP_NONE;
    if (n < want) {
        const int k = n + 1;
        m->why = !fits_rate(k, real, peak_hz, 0)                               ? ZFG_CAP_PEAK
                 : !fits_gpu(k, period, gpu_ms, 0) || (m->gpu_block && k >= m->gpu_block) ? ZFG_CAP_GPU
                                                                               : ZFG_CAP_REFRESH;
    }
    return n;
}

const char *zfg_cap_name(int why) {
    switch (why) {
        case ZFG_CAP_PEAK: return "peak";
        case ZFG_CAP_GPU: return "gpu";
        case ZFG_CAP_REFRESH: return "refresh";
        default: return "none";
    }
}

int zfg_calls_fit(int calls_in_flight, int n) { return calls_in_flight + mult_of(n) - 1 <= ZFG_CONTEXTS; }

int zfg_pool_size(int n) { return (mult_of(n) - 1) * ZFG_POOL_PAIRS; }

uint32_t zfg_import_format(uint32_t ahb_format, uint32_t reported) {
    if (reported) return reported;
    return ahb_format == ZFG_AHB_BGRA8 ? ZFG_VK_B8G8R8A8_UNORM : 0;
}
