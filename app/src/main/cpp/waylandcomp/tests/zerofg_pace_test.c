/* Tests of src/zerofg_pace.c: cc -O2 -Wall -Werror -Isrc tests/zerofg_pace_test.c src/zerofg_pace.c -lm */
#include "zerofg_pace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS 1000000LL

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

static int64_t absl(int64_t v) { return v < 0 ? -v : v; }

static void period_is_a_trimmed_mean(void) {
    struct zfg_period p;
    zfg_period_reset(&p);
    CHECK(zfg_period_get(&p) == 0, "no interval: 0");
    zfg_period_add(&p, 30 * MS);
    zfg_period_add(&p, 34 * MS);
    CHECK(zfg_period_get(&p) == 32 * MS, "two intervals: their mean, got %lld", (long long)zfg_period_get(&p));
    zfg_period_add(&p, 32 * MS);
    zfg_period_add(&p, 200 * MS);
    zfg_period_add(&p, 1 * MS);
    /* 30, 34, 32, 200, 1: without 200 and 1 */
    CHECK(zfg_period_get(&p) == 32 * MS, "outliers dropped, got %lld", (long long)zfg_period_get(&p));
    for (int i = 0; i < ZFG_PERIOD_HIST; i++) zfg_period_add(&p, 16 * MS);
    CHECK(zfg_period_get(&p) == 16 * MS, "the history holds the last %d, got %lld", ZFG_PERIOD_HIST,
          (long long)zfg_period_get(&p));
    zfg_period_add(&p, 0);
    zfg_period_add(&p, -5);
    CHECK(zfg_period_get(&p) == 16 * MS, "non-positive intervals ignored");
}

static void gaps(void) {
    CHECK(!zfg_is_gap(33 * MS, 32 * MS), "one period is no gap");
    CHECK(!zfg_is_gap(200 * MS, 32 * MS), "200 ms at 31 fps: below the 250 ms floor");
    CHECK(zfg_is_gap(300 * MS, 32 * MS), "300 ms at 31 fps: a gap");
    CHECK(!zfg_is_gap(390 * MS, 100 * MS), "390 ms at 10 fps: below four periods");
    CHECK(zfg_is_gap(410 * MS, 100 * MS), "410 ms at 10 fps: a gap");
    CHECK(zfg_is_gap(300 * MS, 0), "unknown period: the floor alone");
}

static void lead_rises_at_once_and_falls_slowly(void) {
    int64_t lead = zfg_lead_update(0, 20 * MS);
    CHECK(lead == 20 * MS, "first delay taken as it is");
    lead = zfg_lead_update(lead, 28 * MS);
    CHECK(lead == 28 * MS, "a longer delay at once");
    lead = zfg_lead_update(lead, 20 * MS);
    CHECK(lead == 27 * MS, "an eighth of the way down, got %lld", (long long)lead);
    for (int i = 0; i < 100; i++) lead = zfg_lead_update(lead, 20 * MS);
    CHECK(absl(lead - 20 * MS) < MS, "settles on a steady delay, got %lld", (long long)lead);
    CHECK(zfg_lead_update(lead, 0) == lead, "no measurement: unchanged");
}

static void target_lead_covers_the_latch(void) {
    const int64_t r = 8333333, p = 32 * MS;
    CHECK(zfg_target_lead(5 * MS, p, 2 * MS, r, r) == 7 * MS + r + r / 2, "ready + margin + latch + half a refresh");
    CHECK(zfg_target_lead(0, p, 2 * MS, r, r) == 18 * MS + r + r / 2, "no measurement: half a period");
    CHECK(zfg_target_lead(90 * MS, p, 2 * MS, r, r) == 64 * MS + r + r / 2, "the measured part at most two periods");
    CHECK(zfg_target_lead(5 * MS, p, 0, 0, 0) == 5 * MS, "nothing else known: the measurement");
}

/* The arithmetic alone, refresh unknown: generated and real frames alternate half a period apart, and the
 * extra delay stays within lead .. lead + slack. */
static void schedule_keeps_half_periods(void) {
    const int64_t period = 32258064, lead = 20 * MS, slack = period / 4;
    const int64_t jitter[] = {0, 3 * MS, -2 * MS, 1 * MS, -4 * MS, 4 * MS, 0, -1 * MS, 2 * MS, -3 * MS};
    int64_t prev_b = 0, prev_s = 0;
    for (int n = 0; n < 200; n++) {
        const int64_t arrival = 1000 * MS + n * period + jitter[n % 10];
        struct zfg_slots t = zfg_schedule(arrival, prev_b, period, lead, slack, 0);
        CHECK(!t.no_s, "frame %d: an S", n);
        CHECK(t.b - t.s == period / 2, "b half a period after s");
        CHECK(t.s >= arrival + lead && t.s <= arrival + lead + slack, "frame %d: s %lld ms after arrival", n,
              (long long)((t.s - arrival) / MS));
        if (prev_b) {
            CHECK(t.s > prev_b, "frame %d: s after the previous b", n);
            CHECK(absl((t.s - prev_s) - period) <= 8 * MS, "frame %d: s spacing %lld ms", n,
                  (long long)((t.s - prev_s) / MS));
        }
        prev_b = t.b;
        prev_s = t.s;
    }
}

static void schedule_follows_a_late_frame_and_pulls_back(void) {
    const int64_t period = 32 * MS, lead = 20 * MS, slack = 8 * MS;
    struct zfg_slots a = zfg_schedule(0, 0, period, lead, slack, 0);
    CHECK(a.s == lead, "first frame: arrival + lead");
    /* 30 ms late: the rhythm would put s before the frame can be ready */
    struct zfg_slots b = zfg_schedule(period + 30 * MS, a.b, period, lead, slack, 0);
    CHECK(b.s == period + 30 * MS + lead, "late frame: arrival + lead");
    /* back on time: lead + slack is still before the previous b, so no S; b half a period after arrival + lead */
    struct zfg_slots c = zfg_schedule(2 * period, b.b, period, lead, slack, 0);
    CHECK(c.no_s, "no S before the previous b");
    CHECK(c.b == 2 * period + lead + period / 2, "b half a period after arrival + lead, got %lld", (long long)c.b);
    struct zfg_slots d = zfg_schedule(3 * period, c.b, period, lead, slack, 0);
    CHECK(!d.no_s && d.s == 3 * period + lead, "then even again, got %lld", (long long)d.s);
}

static void schedule_keeps_a_refresh_between_outputs(void) {
    const int64_t r = 8333333, lead = 20 * MS, p = 15 * MS;
    /* 2x the real rate above the refresh rate: b one refresh after s */
    struct zfg_slots a = zfg_schedule(0, 0, p, lead, p / 4, r);
    CHECK(a.b - a.s == r, "b a refresh after s, got %lld", (long long)(a.b - a.s));
    /* half a period after the previous b is less than a refresh: s pushed to a refresh after it */
    struct zfg_slots b = zfg_schedule(30 * MS, 44 * MS, p, lead, p / 4, r);
    CHECK(!b.no_s && b.s == 44 * MS + r, "pushed a refresh after the previous b, got %lld", (long long)(b.s - 44 * MS));
    /* no room up to lead + slack: no S, b half a period after arrival + lead, a refresh after the previous b */
    struct zfg_slots c = zfg_schedule(30 * MS, 46 * MS, p, lead, p / 4, r);
    CHECK(c.no_s && c.s == 0, "no S");
    CHECK(c.b == 30 * MS + lead + p / 2, "b half a period after arrival + lead, got %lld", (long long)c.b);
    struct zfg_slots d = zfg_schedule(30 * MS, 52 * MS, p, lead, p / 4, r);
    CHECK(d.no_s && d.b == 52 * MS + r, "b a refresh after the previous b, got %lld", (long long)(d.b - 52 * MS));
    CHECK(zfg_slack(32 * MS, r) == r, "slack: at least a refresh");
    CHECK(zfg_slack(40 * MS, r) == 10 * MS, "slack: a quarter period");
}

static void desired_and_apply_times(void) {
    const int64_t r120 = 8333333;
    CHECK(zfg_desired(100 * MS, r120) == 100 * MS - r120 / 2, "half a refresh early");
    CHECK(zfg_desired(100 * MS, 0) == 100 * MS, "unknown refresh: the target");
    CHECK(zfg_latch_deadline(100 * MS, r120, r120) == 100 * MS - r120 / 2 - r120, "a latch before the desired time");
    CHECK(zfg_apply_at(100 * MS, r120, 2 * MS) == 100 * MS - r120 - 2 * MS, "latch and margin before");
}

static void votes(void) {
    CHECK(zfg_vote_div(0, 120.0f, 0) == 0, "unknown period: no vote");
    CHECK(zfg_vote_div(32 * MS, 0.0f, 0) == 0, "unknown peak: no vote");
    CHECK(zfg_vote_div(32258064, 120.0f, 0) == 1, "31 fps on 120: 62 does not fit 60, 120");
    CHECK(zfg_vote_div(40 * MS, 120.0f, 0) == 2, "25 fps on 120: 50 fits 60");
    CHECK(zfg_vote_div(16 * MS, 120.0f, 0) == 1, "62.5 fps: the peak");
    CHECK(zfg_vote_div(28985507, 144.0f, 0) == 2, "34.5 fps on 144: 69 -> 72");
    /* 34-35 fps on a 144 Hz peak (68-70 outputs): one divisor, no flapping */
    int k = 0;
    const int64_t periods[] = {29411764, 28571428, 29000000, 28700000, 29411764, 28571428};
    for (unsigned i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
        k = zfg_vote_div(periods[i], 144.0f, k);
        CHECK(k == 2, "68-70 outputs on 144: 72, got %.1f", 144.0 / k);
    }
    /* 60.4 fps measured for a 60 fps cap, after 60: stays at 120 / 1 */
    CHECK(zfg_vote_div(16556291, 120.0f, 1) == 1, "60.4 fps: 120");
    /* a little above the voted 60: stays; clearly above: the peak */
    CHECK(zfg_vote_div(32786885, 120.0f, 2) == 2, "61 outputs on a 60 vote: within 2 %%");
    CHECK(zfg_vote_div(31746031, 120.0f, 2) == 1, "63 outputs: 120");
    /* down to 58 outputs from 120: not enough room for 60 yet; 50: 60 */
    CHECK(zfg_vote_div(34482758, 120.0f, 1) == 1, "58 outputs stay on 120");
    CHECK(zfg_vote_div(40 * MS, 120.0f, 1) == 2, "50 outputs: 60");
}

static void too_fast(void) {
    CHECK(!zfg_too_fast(32 * MS, 120.0f, 0), "31 fps on 120 Hz: room");
    CHECK(!zfg_too_fast(16666666, 120.0f, 0), "60 fps on 120 Hz: room");
    CHECK(!zfg_too_fast(16129032, 120.0f, 0), "62 fps on 120 Hz: the schedule leaves S out where needed");
    CHECK(zfg_too_fast(11235955, 120.0f, 0), "89 fps on 120 Hz: no room");
    CHECK(!zfg_too_fast(13888888, 144.0f, 0), "72 fps on 144 Hz: room");
    CHECK(!zfg_too_fast(11 * MS, 0.0f, 0), "refresh unknown: not judged");
    CHECK(zfg_too_fast(15873015, 120.0f, 0), "63 fps: 5 %% above");
    CHECK(zfg_too_fast(16000000, 120.0f, 1), "62.5 fps after too fast: still");
    CHECK(!zfg_too_fast(16260162, 120.0f, 1), "61.5 fps after too fast: 2.5 %% above, room again");
}

/* ---- zfg_present's flow against a model of SurfaceFlinger ----
 *
 * One apply token for the process: transactions are taken in the order they were applied; at each refresh V
 * SurfaceFlinger commits at V - sf_latch and takes them from the front while each was applied by then, its
 * buffer is ready and its desired present time is before V. The last one taken for the layer is on the screen
 * for V. */

enum { T_S, T_B };
struct tx {
    int64_t applied, ready, desired;
    int kind, frame;
};

#define SIM_FRAMES 600
#define SIM_TX (2 * SIM_FRAMES + 8)

struct sim {
    /* input */
    int64_t refresh, sf_latch, phase, margin, latch, gen_ns, jitter;
    float peak;
    int64_t (*period_of)(int n);
    int restart_at;     /* forget the previous frame at this frame (a hidden window, a new swapchain) */
    /* output */
    int frames, s_made, s_shown, b_shown, b_lost_after_warmup, s_early, s_missed;
    int s_hold[SIM_FRAMES], b_hold[SIM_FRAMES];
    int order_bad;      /* s not a refresh after the previous b */
    struct tx q[SIM_TX];
    int nq;
};

static uint32_t rng_state;
static int64_t rnd(int64_t span) { /* -span .. span */
    rng_state = rng_state * 1664525u + 1013904223u;
    if (span <= 0) return 0;
    return (int64_t)(rng_state >> 8) % (2 * span + 1) - span;
}

static void push(struct sim *m, int64_t applied, int64_t ready, int64_t desired, int kind, int frame) {
    if (m->nq >= SIM_TX) return;
    m->q[m->nq++] = (struct tx){applied, ready > applied ? ready : applied, desired, kind, frame};
}

/* A pending pair, as zfg_zc.c's pend_run decides it: S when it is ready and its apply time has come, until B's
 * apply time; B then. `next` is the next frame's arrival, which flushes whatever still waits. */
static void settle(struct sim *m, int has_s, int64_t s_ready, int64_t s_apply, int64_t s_desired, int64_t b_ready,
                   int64_t b_apply, int64_t b_desired, int64_t next, int frame) {
    const int64_t timer = 900000; /* a timer fires up to a millisecond late */
    int64_t b_at = b_apply + timer;
    if (has_s) {
        int64_t s_at = s_ready > s_apply + timer ? s_ready : s_apply + timer;
        int shown = s_ready <= b_apply;
        if (next < s_at && next < b_at) { /* flushed by the next frame */
            shown = s_ready <= next;
            s_at = next;
        }
        if (shown) push(m, s_at, s_ready, s_desired, T_S, frame);
        else m->s_missed++;
    }
    if (next < b_at) b_at = next;
    push(m, b_at, b_ready, b_desired, T_B, frame);
}

static void sim_run(struct sim *m) {
    struct zfg_period per;
    zfg_period_reset(&per);
    int64_t last = 0, prev_b = 0, lead = 0, arrival = 1000 * MS;
    int have_prev = 0, fast = 0;
    int64_t arrivals[SIM_FRAMES + 1];
    for (int n = 0; n <= SIM_FRAMES; n++) {
        arrivals[n] = arrival + rnd(m->jitter);
        arrival += m->period_of(n);
    }
    struct { int on, has_s; int64_t s_ready, s_apply, s_desired, b_ready, b_apply, b_desired; } pend = {0};
    for (int n = 0; n < SIM_FRAMES; n++) {
        const int64_t now = arrivals[n];
        if (pend.on) {
            settle(m, pend.has_s, pend.s_ready, pend.s_apply, pend.s_desired, pend.b_ready, pend.b_apply,
                   pend.b_desired, now, n - 1);
            pend.on = 0;
        }
        if (n == m->restart_at) { have_prev = 0; prev_b = 0; }
        const int64_t iv = last ? now - last : 0;
        last = now;
        if (iv > 0 && !zfg_is_gap(iv, zfg_period_get(&per))) zfg_period_add(&per, iv);
        const int64_t period = zfg_period_get(&per);
        const int64_t b_ready = now + MS;
        int why = -1; /* 0: shown at once */
        struct zfg_slots t = {0, 0, 0};
        if (!have_prev || period <= 0) why = 0;
        else {
            fast = zfg_too_fast(period, m->peak, fast);
            if (fast) why = 0;
        }
        if (why < 0) {
            const int64_t l = zfg_target_lead(lead, period, m->margin, m->latch, m->refresh);
            t = zfg_schedule(now, prev_b, period, l, zfg_slack(period, m->refresh), m->refresh);
            if (prev_b && !t.no_s && t.s < prev_b + m->refresh) m->order_bad++;
            if (prev_b && t.b < prev_b + m->refresh) m->order_bad++;
        }
        have_prev = 1;
        if (why == 0) {
            push(m, now, b_ready, 0, T_B, n);
            prev_b = 0;
            continue;
        }
        const int64_t s_ready = now + m->gen_ns + rnd(m->gen_ns / 5);
        pend.on = 1;
        pend.has_s = !t.no_s;
        pend.s_ready = s_ready;
        pend.s_desired = zfg_desired(t.s, m->refresh);
        pend.s_apply = zfg_apply_at(pend.s_desired, m->latch, m->margin);
        pend.b_ready = b_ready;
        pend.b_desired = zfg_desired(t.b, m->refresh);
        pend.b_apply = zfg_apply_at(pend.b_desired, m->latch, m->margin);
        if (t.no_s) m->s_early++;
        else {
            m->s_made++;
            /* note_ready: the measurement is in before the next frame needs it */
            if (s_ready < arrivals[n + 1]) lead = zfg_lead_update(lead, s_ready - now);
        }
        prev_b = t.b;
    }
    if (pend.on)
        settle(m, pend.has_s, pend.s_ready, pend.s_apply, pend.s_desired, pend.b_ready, pend.b_apply, pend.b_desired,
               INT64_MAX / 2, SIM_FRAMES - 1);
    m->frames = SIM_FRAMES;

    /* SurfaceFlinger */
    memset(m->s_hold, 0, sizeof(m->s_hold));
    memset(m->b_hold, 0, sizeof(m->b_hold));
    int head = 0, kind = -1, frame = -1;
    const int64_t end = arrivals[SIM_FRAMES] + 200 * MS;
    for (int64_t v = 1000 * MS + m->phase; v < end; v += m->refresh) {
        const int64_t commit = v - m->sf_latch;
        while (head < m->nq) {
            const struct tx *x = &m->q[head];
            if (x->applied > commit || x->ready > commit || (x->desired && x->desired >= v)) break;
            kind = x->kind;
            frame = x->frame;
            head++;
        }
        if (frame < 0) continue;
        if (kind == T_S) m->s_hold[frame]++;
        else m->b_hold[frame]++;
    }
    for (int n = 0; n < SIM_FRAMES; n++) {
        if (m->s_hold[n]) m->s_shown++;
        if (m->b_hold[n]) m->b_shown++;
        else if (n >= 20) m->b_lost_after_warmup++;
    }
}

static int64_t g_p1, g_p2;
static int g_step_at;
static int64_t period_fixed(int n) { (void)n; return g_p1; }
static int64_t period_step(int n) { return n < g_step_at ? g_p1 : g_p2; }

static void sim_init(struct sim *m, int64_t refresh, int64_t gen, int64_t jitter) {
    memset(m, 0, sizeof(*m));
    m->refresh = refresh;
    m->sf_latch = refresh;
    m->latch = refresh; /* zerofg_zc.c's default: one refresh */
    m->margin = 2 * MS;
    m->phase = 3100000;
    m->gen_ns = gen;
    m->jitter = jitter;
    m->peak = (float)(1e9 / (double)refresh);
    m->period_of = period_fixed;
    m->restart_at = -1;
    rng_state = 12345;
}

static void hold_means(const struct sim *m, int from, int to, double *s, double *b) {
    int ns = 0, nb = 0;
    long hs = 0, hb = 0;
    for (int n = from; n < to; n++) {
        if (m->s_hold[n]) { hs += m->s_hold[n]; ns++; }
        hb += m->b_hold[n];
        nb++;
    }
    *s = ns ? (double)hs / ns : 0.0;
    *b = nb ? (double)hb / nb : 0.0;
}

/* 30 fps on 120 Hz: every S and B on two refreshes of its own. */
static void sim_30_on_120(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 5 * MS, 1 * MS);
    g_p1 = 33333333;
    sim_run(m);
    CHECK(m->b_lost_after_warmup == 0, "30 on 120: %d real frames never shown", m->b_lost_after_warmup);
    CHECK(m->s_shown >= m->s_made - 2, "30 on 120: %d of %d S shown", m->s_shown, m->s_made);
    int even = 0;
    for (int n = 20; n < SIM_FRAMES - 2; n++) even += m->s_hold[n] == 2 && m->b_hold[n] == 2;
    CHECK(even >= (SIM_FRAMES - 22) * 9 / 10, "30 on 120: %d of %d pairs held 2/2", even, SIM_FRAMES - 22);
    CHECK(m->order_bad == 0, "30 on 120: %d outputs closer than a refresh", m->order_bad);
    free(m);
}

/* 31 fps on 120 Hz, also right after a restart (the rhythm starts at the lower bound): S and B share the 3.87
 * refreshes evenly, not one refresh for S and three for B. */
static void sim_31_on_120_after_restart(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 5 * MS, 3 * MS);
    g_p1 = 32258064;
    m->restart_at = 300;
    sim_run(m);
    double s, b;
    hold_means(m, 20, 290, &s, &b);
    CHECK(s > 1.6 && b > 1.6, "31 on 120: S held %.2f, B held %.2f refreshes", s, b);
    hold_means(m, 302, SIM_FRAMES - 2, &s, &b);
    CHECK(s > 1.6 && b > 1.6, "31 on 120 after a restart: S held %.2f, B held %.2f refreshes", s, b);
    CHECK(m->b_lost_after_warmup == 0, "31 on 120: %d real frames never shown", m->b_lost_after_warmup);
    CHECK(m->s_shown >= m->s_made * 95 / 100, "31 on 120: %d of %d S shown", m->s_shown, m->s_made);
    free(m);
}

/* 62 fps on 120 Hz: 124 outputs a second do not fit 120 refreshes. S frames are left out, never a real one. */
static void sim_62_on_120(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 3 * MS, 300000);
    g_p1 = 16129032;
    sim_run(m);
    CHECK(m->b_lost_after_warmup == 0, "62 on 120: %d real frames never shown", m->b_lost_after_warmup);
    CHECK(m->order_bad == 0, "62 on 120: %d outputs closer than a refresh", m->order_bad);
    const double secs = SIM_FRAMES * 16129032.0 / 1e9;
    const double left_out = (m->s_early + m->s_missed + (m->s_made - m->s_shown)) / secs;
    CHECK(left_out > 2.0 && left_out < 8.0, "62 on 120: %.1f S a second left out (about 4 expected)", left_out);
    free(m);
}

/* 60 fps on 120 Hz: one refresh each. */
static void sim_60_on_120(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 3 * MS, 300000);
    g_p1 = 16666666;
    sim_run(m);
    CHECK(m->b_lost_after_warmup == 0, "60 on 120: %d real frames never shown", m->b_lost_after_warmup);
    CHECK(m->s_shown >= (SIM_FRAMES - 20) * 9 / 10, "60 on 120: %d S shown", m->s_shown);
    free(m);
}

/* A step from 30 to 60 fps (menu to gameplay): no output closer than a refresh to the one before, and no real
 * frame lost while the period estimate catches up. */
static void sim_rate_step(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 5 * MS, 300000);
    g_p1 = 33333333;
    g_p2 = 16666666;
    g_step_at = 200;
    m->period_of = period_step;
    sim_run(m);
    CHECK(m->order_bad == 0, "30 -> 60: %d outputs closer than a refresh", m->order_bad);
    CHECK(m->b_lost_after_warmup == 0, "30 -> 60: %d real frames never shown", m->b_lost_after_warmup);
    g_p1 = 16666666;
    g_p2 = 33333333;
    memset(m, 0, sizeof(*m));
    sim_init(m, 8333333, 5 * MS, 300000);
    m->period_of = period_step;
    sim_run(m);
    CHECK(m->order_bad == 0, "60 -> 30: %d outputs closer than a refresh", m->order_bad);
    CHECK(m->b_lost_after_warmup == 0, "60 -> 30: %d real frames never shown", m->b_lost_after_warmup);
    free(m);
}

/* SurfaceFlinger latching a little later than assumed (0.8 refresh) or earlier (1.2): still every real frame. */
static void sim_latch_tolerance(void) {
    const int64_t sf[] = {6666666, 10000000};
    for (int i = 0; i < 2; i++) {
        struct sim *m = calloc(1, sizeof(*m));
        sim_init(m, 8333333, 5 * MS, 2 * MS);
        g_p1 = 32258064;
        m->sf_latch = sf[i];
        sim_run(m);
        CHECK(m->b_lost_after_warmup == 0, "SurfaceFlinger latching %.1f ms ahead: %d real frames never shown",
              sf[i] / 1e6, m->b_lost_after_warmup);
        CHECK(m->s_shown >= m->s_made * 9 / 10, "SurfaceFlinger latching %.1f ms ahead: %d of %d S shown", sf[i] / 1e6,
              m->s_shown, m->s_made);
        free(m);
    }
}

int main(void) {
    period_is_a_trimmed_mean();
    gaps();
    lead_rises_at_once_and_falls_slowly();
    target_lead_covers_the_latch();
    schedule_keeps_half_periods();
    schedule_follows_a_late_frame_and_pulls_back();
    schedule_keeps_a_refresh_between_outputs();
    desired_and_apply_times();
    votes();
    too_fast();
    sim_30_on_120();
    sim_31_on_120_after_restart();
    sim_62_on_120();
    sim_60_on_120();
    sim_rate_step();
    sim_latch_tolerance();
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("zerofg_pace_test: all passed\n");
    return 0;
}
