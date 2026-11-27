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

void zfg_ready_reset(struct zfg_ready *r) { memset(r, 0, sizeof(*r)); }

void zfg_ready_add(struct zfg_ready *r, int64_t delay) {
    if (delay <= 0) return;
    r->v[r->next] = delay;
    r->next = (r->next + 1) % ZFG_READY_HIST;
    if (r->n < ZFG_READY_HIST) r->n++;
}

int64_t zfg_ready_get(const struct zfg_ready *r) {
    if (r->n < ZFG_READY_HIST / 2) return 0;
    int64_t v[ZFG_READY_HIST];
    memcpy(v, r->v, sizeof(v));
    for (int i = 1; i < r->n; i++)
        for (int k = i; k > 0 && v[k] < v[k - 1]; k--) {
            const int64_t t = v[k];
            v[k] = v[k - 1];
            v[k - 1] = t;
        }
    return r->n % 2 ? v[r->n / 2] : (v[r->n / 2 - 1] + v[r->n / 2]) / 2;
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
    n = mult_of(n);
    const int64_t share = period > 0 && n > 2 ? (int64_t)((double)period / n * ZFG_SPACING) : 0;
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
    struct zfg_slots r = {s, s + (n - 1) * step, 0, n, step, 0};
    if (!prev_b || s >= prev_b + gap) return r;
    if (prev_b + gap <= hi) {
        r.s = prev_b + gap;
        r.b = r.s + (n - 1) * step;
        return r;
    }
    /* the first m generated frames left out, where n outputs fit the refresh rate (within ZFG_OVER_KEEP, as the
     * multiplier is kept); else (the multiplier is about to be lowered for the running rate) none */
    const int fit = refresh <= 0 || (double)q * (1.0 + ZFG_OVER_KEEP) >= (double)refresh;
    for (int m = 1; fit && m < n - 1; m++) {
        int64_t f = prev_b + gap;
        if (f < lo + m * step) f = lo + m * step;
        if (f <= hi + m * step) {
            r.first = m;
            r.s = f;
            r.b = f + (n - 1 - m) * step;
            return r;
        }
    }
    r.no_s = 1;
    r.first = n - 1;
    r.s = 0;
    const int64_t rest = lo + (n - 1) * q;
    r.b = rest > prev_b + gap ? rest : prev_b + gap;
    if (r.b > hi + (n - 1) * step) r.b = rest;
    return r;
}

struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                              int64_t refresh) {
    return zfg_schedule_n(arrival, prev_b, period, lead, slack, refresh, 2);
}

int64_t zfg_slot_s(const struct zfg_slots *t, int k) {
    return t->no_s || k < t->first || k > t->n - 2 ? 0 : t->s + (int64_t)(k - t->first) * t->step;
}

void zfg_slots_drop_first(struct zfg_slots *t, int m) {
    if (m <= 0 || t->no_s) return;
    if (t->first + m >= t->n - 1) {
        t->no_s = 1;
        t->first = t->n - 1;
        t->s = 0;
        return;
    }
    t->first += m;
    t->s += (int64_t)m * t->step;
}

int zfg_slots_shown(const struct zfg_slots *t) { return t->no_s ? 0 : t->n - 1 - t->first; }

int64_t zfg_desired(int64_t target, int64_t refresh_period) {
    return refresh_period > 0 ? target - refresh_period / 2 : target;
}

int64_t zfg_latch_deadline(int64_t target, int64_t refresh_period, int64_t latch) {
    return zfg_desired(target, refresh_period) - (latch > 0 ? latch : 0);
}

int64_t zfg_apply_at(int64_t desired, int64_t latch, int64_t margin) {
    return desired - (latch > 0 ? latch : 0) - (margin > 0 ? margin : 0);
}

void zfg_pair_times(const struct zfg_slots *t, int64_t refresh, int64_t latch, int64_t margin,
                    struct zfg_pair_times *out) {
    memset(out, 0, sizeof(*out));
    out->ns = zfg_slots_shown(t);
    out->b_desired = zfg_desired(t->b, refresh);
    out->b_apply = zfg_apply_at(out->b_desired, latch, margin);
    for (int k = 0; k < out->ns; k++) {
        out->s_desired[k] = zfg_desired(zfg_slot_s(t, t->first + k), refresh);
        out->s_apply[k] = zfg_apply_at(out->s_desired[k], latch, margin);
    }
    for (int k = 0; k < out->ns; k++) {
        const int64_t next = k + 1 < out->ns ? out->s_apply[k + 1] : out->b_apply;
        out->s_last[k] = next + (margin > 0 ? margin : 0) - refresh;
        if (out->s_last[k] < out->s_apply[k] + 1000000) out->s_last[k] = out->s_apply[k] + 1000000;
    }
}

int zfg_s_action(int ready, int64_t now, int64_t apply, int64_t last, int force) {
    const int late = now >= last;
    if (ready && !late && (force || now >= apply)) return ZFG_S_SHOW;
    if (late || force) return ZFG_S_DROP;
    return ZFG_S_WAIT;
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
    int kept = 0, even = 0;
    for (int i = 0; i < nc; i++) {
        if (cand[i] > top) top = cand[i];
        const int fits = vote_fits(cand[i], out, ZFG_VOTE_FIT, ZFG_VOTE_FIT);
        if (prev > 0.0f && fabs(cand[i] - prev) < 0.5 &&
            vote_fits(cand[i], out, ZFG_VOTE_KEEP_BELOW, ZFG_VOTE_KEEP_ABOVE)) {
            kept = 1;
            even = fits;
        }
        if (fits && (best <= 0.0f || cand[i] < best)) best = cand[i];
    }
    if (kept && !(best > 0.0f && best < prev && (!even || best <= prev / 2 + 0.5f))) return prev;
    return best > 0.0f ? best : top;
}

float zfg_vote_pick(int64_t period, const float *modes, int n, float peak_hz, float prev) {
    return zfg_vote_pick_n(period, 2, modes, n, peak_hz, prev);
}

void zfg_vote_reset(struct zfg_vote *v) { memset(v, 0, sizeof(*v)); }

void zfg_vote_layer(struct zfg_vote *v, int layer) {
    if (v->layer == layer + 1) return;
    zfg_vote_reset(v);
    v->layer = layer + 1;
}

/* Outputs at `out` a second take more refreshes of `hz` each than one and are more than ZFG_VOTE_UNEVEN off a whole
 * number of them. */
static int vote_uneven(double hz, double out) {
    const double r = hz / out;
    return r > 1.0 && fabs(r - floor(r + 0.5)) > ZFG_VOTE_UNEVEN;
}

float zfg_vote_step_n(struct zfg_vote *v, int64_t now, int64_t period, int mult, int held, const float *modes,
                      int n, float peak_hz) {
    if (period > 0 && v->frames < ZFG_PERIOD_HIST + 8) v->frames++;
    const int k = v->frames - ZFG_PERIOD_HIST;
    if (period > 0 && k > 0) v->period += (period - v->period) / k;
    int64_t dt = v->last > 0 && now > v->last ? now - v->last : 0;
    if (dt > ZFG_GAP_MIN_NS) dt = ZFG_GAP_MIN_NS;
    v->last = now;
    if (k < ZFG_VOTE_FIRST) return v->hz;
    const float pick = zfg_vote_pick_n(v->period, mult, modes, n, peak_hz, v->hz);
    if (pick <= 0.0f) return v->hz;
    if (v->hz <= 0.0f) {
        v->hz = pick;
        v->away = 0;
        v->young_until = now + ZFG_VOTE_HOLD_NS;
        return pick;
    }
    const double out = mult_of(mult) * 1e9 / (double)v->period;
    if (held || now < v->young_until || out > (double)v->hz * (1.0 + ZFG_VOTE_FAR) || vote_uneven(v->hz, out) ||
        v->hz > peak_hz + 0.5f)
        dt *= ZFG_VOTE_HOLD_NS / ZFG_VOTE_HOLD_FAR_NS;
    if (pick == v->hz) {
        v->away = v->away > dt ? v->away - dt : 0;
        return v->hz;
    }
    v->away += dt;
    if (v->away >= ZFG_VOTE_HOLD_NS) {
        v->hz = pick;
        v->away = 0;
        v->young_until = 0;
    }
    return v->hz;
}

float zfg_vote_step(struct zfg_vote *v, int64_t now, int64_t period, const float *modes, int n, float peak_hz) {
    return zfg_vote_step_n(v, now, period, 2, 0, modes, n, peak_hz);
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
    const int over_run = refresh_hz > 0.0f && 2.0 * real > (double)refresh_hz * (was ? 1.03 : 1.0 + ZFG_OVER_KEEP);
    if (!over_run) f->over_since = 0;
    else if (!f->over_since) f->over_since = now;
    f->on = zfg_too_fast(period, peak_hz, was) ||
            (refresh_hz > 0.0f && real > (double)refresh_hz * (was ? 1.0 : 1.02)) ||
            (over_run && now - f->over_since >= ZFG_FAST_GRACE_NS);
    return f->on;
}

void zfg_mult_reset(struct zfg_mult *m) { memset(m, 0, sizeof(*m)); }

int zfg_flight_pairs(int64_t ready_ns, int64_t period) {
    return ready_ns > 0 && period > 0 ? (int)(ready_ns / period) + 1 : 0;
}

double zfg_pair_gpu_ms(int mult, double first_ms, double phase_ms) {
    mult = mult_of(mult);
    return (first_ms > 0.0 ? first_ms : 0.0) + (mult - 2) * (phase_ms > 0.0 ? phase_ms : 0.0);
}

/* Whether `mult` times the real rate fits: at most `over` above a rate (0: unknown), the GPU time of a pair within
 * the share of the period, and its pairs in flight within the frame contexts and jobs. keep: the limits that keep a
 * multiplier in force (else those that let one come back; over: 5 % above the peak rate, ZFG_OVER_KEEP above the
 * running rate, else 3 %). */
static int fits_rate(int mult, double real, float hz, double over) {
    return hz <= 0.0f || mult * real <= (double)hz * (1.0 + over);
}

static double peak_over(int keep) { return keep ? 0.05 : 0.03; }
static double running_over(int keep) { return keep ? ZFG_OVER_KEEP : 0.03; }

static int fits_gpu(int mult, const struct zfg_mult_in *in, int keep) {
    return in->gpu_first_ms <= 0.0 || zfg_pair_gpu_ms(mult, in->gpu_first_ms, in->gpu_phase_ms) <=
                                          (double)in->period / 1e6 * ZFG_GPU_SHARE * (keep ? 1.0 : ZFG_GPU_RAISE);
}

static int fits_flight(int mult, const struct zfg_mult_in *in, int keep) {
    const int64_t ready = keep ? in->ready_ns : (int64_t)((double)in->ready_ns * ZFG_FLIGHT_RAISE);
    const int pairs = zfg_flight_pairs(ready, in->period);
    return !pairs || (pairs <= ZFG_JOBS && pairs * (mult_of(mult) - 1) <= ZFG_CONTEXTS);
}

int zfg_mult_update(struct zfg_mult *m, int64_t now, const struct zfg_mult_in *in) {
    const int want = mult_of(in->want);
    const int limit = in->limit >= 2 ? mult_of(in->limit) : ZFG_MULT_MAX;
    if (!m->n || want != m->want) {
        m->n = m->want = want;
        m->raise = 0;
        m->over_since = 0;
        m->gpu_block = 0;
    }
    if (m->gpu_block && now >= m->gpu_block_until) m->gpu_block = 0;
    const int64_t period = in->period;
    if (period <= 0) {
        if (m->n > limit) m->n = limit;
        m->vote_n = m->n;
        m->why = m->n < want ? (m->n >= limit ? ZFG_CAP_ENGINE : m->why) : ZFG_CAP_NONE;
        return m->n;
    }
    const double real = 1e9 / (double)period;
    int n = m->n;
    while (n > limit) n--;
    while (n > 2 && !fits_rate(n, real, in->peak_hz, peak_over(1))) n--;
    while (n > 2 && !fits_gpu(n, in, 1)) {
        m->gpu_block = n;
        m->gpu_block_until = now + ZFG_GPU_BLOCK_NS;
        n--;
    }
    while (n > 2 && !fits_flight(n, in, 1)) n--;
    if (n > 2 && !fits_rate(n, real, in->refresh_hz, running_over(1))) {
        if (!m->over_since) m->over_since = now;
        if (now - m->over_since >= ZFG_FAST_GRACE_NS)
            while (n > 2 && !fits_rate(n, real, in->refresh_hz, running_over(1))) n--;
    }
    if (n != m->n) m->raise = 0;
    if (n > 2 && fits_rate(n, real, in->refresh_hz, running_over(1))) m->over_since = 0;
    if (n == 2) m->over_since = 0;
    int up = 0;
    for (int k = want < limit ? want : limit; k > n && !up; k--)
        if (fits_rate(k, real, in->peak_hz, peak_over(0)) && fits_rate(k, real, in->refresh_hz, running_over(0)) &&
            fits_gpu(k, in, 0) && fits_flight(k, in, 0) && !(m->gpu_block && k >= m->gpu_block))
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
    for (int k = want < limit ? want : limit; k > n; k--)
        if (fits_rate(k, real, in->peak_hz, peak_over(k <= voted)) && fits_gpu(k, in, 1) && fits_flight(k, in, 1) &&
            !(m->gpu_block && k >= m->gpu_block)) {
            m->vote_n = k;
            break;
        }
    m->why = ZFG_CAP_NONE;
    if (n < want) {
        const int k = n + 1;
        m->why = k > limit                                                         ? ZFG_CAP_ENGINE
                 : !fits_rate(k, real, in->peak_hz, peak_over(0))                   ? ZFG_CAP_PEAK
                 : !fits_gpu(k, in, 0) || (m->gpu_block && k >= m->gpu_block)       ? ZFG_CAP_GPU
                 : !fits_flight(k, in, 0)                                           ? ZFG_CAP_LATE
                                                                                    : ZFG_CAP_REFRESH;
    }
    return n;
}

const char *zfg_cap_name(int why) {
    switch (why) {
        case ZFG_CAP_PEAK: return "peak";
        case ZFG_CAP_GPU: return "gpu";
        case ZFG_CAP_REFRESH: return "refresh";
        case ZFG_CAP_LATE: return "late";
        case ZFG_CAP_ENGINE: return "engine";
        default: return "none";
    }
}

int zfg_calls_fit(int calls_in_flight, int calls) {
    return calls_in_flight + (calls > 0 ? calls : 1) <= ZFG_CONTEXTS;
}

int zfg_pool_size(int n) { return (mult_of(n) - 1) * ZFG_POOL_PAIRS; }

uint32_t zfg_import_format(uint32_t ahb_format, uint32_t reported) {
    if (reported) return reported;
    return ahb_format == ZFG_AHB_BGRA8 ? ZFG_VK_B8G8R8A8_UNORM : 0;
}
