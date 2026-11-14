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

static float pick(double fps, const float *modes, int n, float peak, float prev) {
    return zfg_vote_pick((int64_t)(1e9 / fps), modes, n, peak, prev);
}

static void votes(void) {
    const float m120[] = {30, 48, 50, 60, 90, 120, 144}; /* the tablet's modes; the session's peak 120 or 144 */
    const float m144[] = {60, 90, 120, 144};
    const float m90[] = {60, 90, 120};
    CHECK(zfg_vote_pick(0, m144, 4, 144.0f, 0) == 0.0f, "unknown period: no vote");
    CHECK(pick(30, m144, 4, 0.0f, 0) == 0.0f, "unknown peak: no vote");
    /* 30 fps: 60 outputs; 60 Hz is the lowest mode with one refresh each (72 would make 144 hold 2/3/2/3) */
    CHECK(pick(30, m144, 4, 144.0f, 0) == 60.0f, "30 fps on 60/90/120/144: 60, got %.1f", pick(30, m144, 4, 144.0f, 0));
    CHECK(pick(30, m120, 7, 144.0f, 0) == 60.0f, "30 fps on the tablet: 60");
    CHECK(pick(60, m144, 4, 144.0f, 0) == 120.0f, "60 fps on 144: 120, not 144");
    CHECK(pick(45, m90, 3, 120.0f, 0) == 90.0f, "45 fps on 60/90/120: 90, not 120");
    CHECK(pick(35.5, m120, 7, 144.0f, 0) == 144.0f, "35.5 fps: 71 outputs, 144 holds two each");
    /* no mode fits evenly (33.8 fps: 67.6 outputs): the highest up to the peak */
    CHECK(pick(33.8, m120, 7, 144.0f, 0) == 144.0f, "33.8 fps: the peak");
    CHECK(pick(33.8, m120, 7, 120.0f, 0) == 120.0f, "33.8 fps, peak 120: 120, modes above the peak left out");
    CHECK(pick(31, m120, 7, 120.0f, 0) == 120.0f, "31 fps: 62 does not fit 60 or 120");
    /* jitter around 30 fps: 60 stays */
    float v = 0.0f;
    const double fps[] = {30.0, 29.4, 30.6, 31.0, 29.0, 30.2};
    for (unsigned i = 0; i < sizeof(fps) / sizeof(fps[0]); i++) {
        v = pick(fps[i], m120, 7, 120.0f, v);
        CHECK(v == 60.0f, "%.1f fps after 60: 60, got %.1f", fps[i], v);
    }
    /* clearly away from 60 outputs: the vote moves */
    CHECK(pick(32.0, m120, 7, 120.0f, 60.0f) == 120.0f, "64 outputs after 60: 120");
    /* no modes given: whole fractions of the peak */
    CHECK(pick(30, NULL, 0, 120.0f, 0) == 60.0f, "30 fps, modes unknown: 120 / 2");
    CHECK(pick(20, NULL, 0, 120.0f, 0) == 40.0f, "20 fps, modes unknown: 120 / 3");
    CHECK(pick(25, NULL, 0, 120.0f, 0) == 120.0f, "25 fps, modes unknown: 50 fits no fraction, the peak");
    CHECK(pick(60.4, NULL, 0, 120.0f, 0) == 120.0f, "60.4 fps: 120");
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

/* The panel below the peak rate: twice the real rate above the running rate is fast once the panel did not
 * come up within the grace; the real rate itself above it at once. */
static void fast_against_the_running_rate(void) {
    struct zfg_fast f = {0, 0};
    const int64_t t0 = 1000 * MS;
    CHECK(!zfg_fast_update(&f, t0, 32 * MS, 120.0f, 120.0f), "31 fps on 120: not fast");
    /* 60 fps with the panel held at 60 and a peak of 120 */
    int64_t t = t0;
    int on_at = -1;
    for (int n = 0; n < 200; n++, t += 16666666)
        if (zfg_fast_update(&f, t, 16666666, 60.0f, 120.0f) && on_at < 0) on_at = n;
    CHECK(on_at >= 115 && on_at <= 125, "60 fps on a 60 Hz panel (peak 120): fast after about 2 s, got frame %d", on_at);
    /* the panel came up: room again */
    CHECK(!zfg_fast_update(&f, t, 16666666, 120.0f, 120.0f), "60 fps on 120 again: not fast");
    CHECK(f.over_since == 0, "the grace starts anew");
    /* back on 60 for less than the grace: not fast */
    CHECK(!zfg_fast_update(&f, t + 100 * MS, 16666666, 60.0f, 120.0f), "60 on 60 again: the grace first");
    struct zfg_fast g = {0, 0};
    CHECK(zfg_fast_update(&g, t0, 15873015, 60.0f, 120.0f), "63 fps on a 60 Hz panel: fast at once");
    CHECK(zfg_fast_update(&g, t0 + 20 * MS, 16556291, 60.0f, 120.0f), "60.4 fps after that: still (grace runs)");
    struct zfg_fast h = {0, 0};
    CHECK(!zfg_fast_update(&h, t0, 16393442, 60.0f, 120.0f), "61 fps on 60 Hz: within 2 %%, the grace first");
    CHECK(zfg_fast_update(&h, t0, 11 * MS, 0.0f, 120.0f), "refresh unknown: the peak alone (91 fps)");
    CHECK(!zfg_fast_update(&h, t0, 0, 60.0f, 120.0f) && !h.on && !h.over_since, "period unknown: reset");
}

/* Real frames a little faster than the refresh rate, no S: b no longer moves later with every frame. */
static void schedule_does_not_drift(void) {
    const int64_t r = 16666666, p = 16393442, lead = 30 * MS; /* 61 fps on 60 Hz */
    int64_t prev_b = 0, worst = 0;
    for (int n = 0; n < 1000; n++) {
        const int64_t arrival = 1000 * MS + n * p;
        struct zfg_slots t = zfg_schedule(arrival, prev_b, p, lead, zfg_slack(p, r), r);
        if (t.b - arrival > worst) worst = t.b - arrival;
        prev_b = t.b;
    }
    const int64_t bound = lead + zfg_slack(p, r) + r;
    CHECK(worst <= bound, "61 fps on 60 Hz: b at most %lld ms after arrival, got %lld", (long long)(bound / MS),
          (long long)(worst / MS));
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
#define SIM_JOBS 3          /* zerofg_zc.c's ZJOBS */
#define SIM_PEND 4          /* ZPEND */
#define DISPATCH 200000LL   /* the event loop runs a due timer or a completion this long after it is due */

struct sim {
    /* input */
    int64_t refresh, sf_latch, phase, margin, latch, gen_ns, render_ns, jitter;
    float peak, running;  /* running: the panel's rate as zerofg_zc.c sees it (0: 1e9 / refresh) */
    int dpt;
    int64_t (*period_of)(int n);
    int restart_at;     /* forget the previous frame at this frame (a hidden window, a new swapchain) */
    /* output */
    int frames, s_made, s_shown, b_shown, b_lost_after_warmup, s_early, s_missed, s_late, forced, first_fast;
    int64_t b_after_max; /* longest from arrival to B's target after the warm-up */
    int s_hold[SIM_FRAMES], b_hold[SIM_FRAMES], fast[SIM_FRAMES];
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

/* The pairs waiting, as zerofg_zc.c's pend_run handles them. */
struct spair {
    int frame, has_s, s_done;
    int64_t push, s_ready, s_apply, s_last, s_desired, b_ready, b_apply, b_desired;
};
struct squeue {
    struct spair p[SIM_PEND];
    int n;
    int64_t cursor; /* when the last B went */
};

static void sq_pop(struct squeue *q) {
    memmove(&q->p[0], &q->p[1], (size_t)(q->n - 1) * sizeof(q->p[0]));
    q->n--;
}

/* The oldest pair up to `until`: S once it is complete and its apply time has come, before s_last (else left
 * out then); B at its apply time. 0: nothing more happens before `until`. */
static int sq_step(struct sim *m, struct squeue *q, int64_t until) {
    struct spair *p = &q->p[0];
    int64_t at = q->cursor > p->push ? q->cursor : p->push;
    if (p->has_s && !p->s_done) {
        int64_t x;
        if (p->s_ready <= (at > p->s_apply ? at : p->s_apply)) x = at >= p->s_apply ? at : p->s_apply + DISPATCH;
        else x = p->s_ready + DISPATCH; /* its completion */
        const int shown = x < p->s_last;
        if (!shown) x = at > p->s_last ? at : p->s_last + DISPATCH;
        if (x > until) return 0;
        if (shown) push(m, x, p->s_ready, m->dpt ? p->s_desired : 0, T_S, p->frame);
        else m->s_missed++;
        p->s_done = 1;
        at = x;
    }
    const int64_t y = at >= p->b_apply ? at : p->b_apply + DISPATCH;
    if (y > until) return 0;
    push(m, y, p->b_ready, m->dpt ? p->b_desired : 0, T_B, p->frame);
    q->cursor = y;
    sq_pop(q);
    return 1;
}

/* The oldest pair now: S only if complete and before s_last. */
static void sq_force(struct sim *m, struct squeue *q, int64_t now) {
    struct spair *p = &q->p[0];
    if (p->has_s && !p->s_done) {
        if (p->s_ready <= now && now < p->s_last) push(m, now, p->s_ready, m->dpt ? p->s_desired : 0, T_S, p->frame);
        else m->s_missed++;
    }
    push(m, now, p->b_ready, m->dpt ? p->b_desired : 0, T_B, p->frame);
    q->cursor = now;
    sq_pop(q);
    m->forced++;
}

static void sim_run(struct sim *m) {
    struct zfg_period per;
    zfg_period_reset(&per);
    struct zfg_fast fast = {0, 0};
    int64_t last = 0, prev_b = 0, lead = 0, arrival = 1000 * MS;
    int have_prev = 0;
    const float running = m->running > 0.0f ? m->running : (float)(1e9 / (double)m->refresh);
    int64_t arrivals[SIM_FRAMES + 1];
    for (int n = 0; n <= SIM_FRAMES; n++) {
        arrivals[n] = arrival + rnd(m->jitter);
        arrival += m->period_of(n);
    }
    static struct squeue q;
    memset(&q, 0, sizeof(q));
    /* generations in flight: completion and arrival, in submission order */
    int64_t job_done[SIM_FRAMES], job_arrival[SIM_FRAMES];
    int jobs = 0, jobs_seen = 0;
    m->first_fast = -1;
    for (int n = 0; n < SIM_FRAMES; n++) {
        const int64_t now = arrivals[n];
        while (q.n && sq_step(m, &q, now)) {}
        /* note_ready: completions the event loop saw before this frame */
        while (jobs_seen < jobs && job_done[jobs_seen] <= now) {
            lead = zfg_lead_update(lead, job_done[jobs_seen] - job_arrival[jobs_seen]);
            jobs_seen++;
        }
        int running_jobs = 0;
        for (int i = jobs_seen; i < jobs; i++) running_jobs += job_done[i] > now;
        if (n == m->restart_at) { have_prev = 0; prev_b = 0; }
        const int64_t iv = last ? now - last : 0;
        last = now;
        if (iv > 0 && !zfg_is_gap(iv, zfg_period_get(&per))) zfg_period_add(&per, iv);
        const int64_t period = zfg_period_get(&per);
        const int64_t b_ready = now + (m->render_ns > 0 ? m->render_ns : MS);
        enum { GEN, DIRECT, NO_S } why = GEN;
        struct zfg_slots t = {0, 0, 0};
        const int64_t l = zfg_target_lead(lead, period, m->margin, m->latch, m->refresh);
        if (period > 0) t = zfg_schedule(now, prev_b, period, l, zfg_slack(period, m->refresh), m->refresh);
        zfg_fast_update(&fast, now, period, running, m->peak);
        if (!have_prev || period <= 0) why = DIRECT;
        else if (fast.on) {
            why = DIRECT;
            m->fast[n] = 1;
            if (m->first_fast < 0) m->first_fast = n;
        } else if (t.no_s) {
            why = NO_S;
            m->s_early++;
        } else if (running_jobs >= SIM_JOBS) {
            why = NO_S;
            m->s_late++;
        }
        if (why != DIRECT && prev_b) {
            if (why == GEN && t.s < prev_b + m->refresh) m->order_bad++;
            if (t.b < prev_b + m->refresh) m->order_bad++;
        }
        have_prev = 1;
        if (why == DIRECT || (why == NO_S && !prev_b)) {
            while (q.n) sq_force(m, &q, now);
            push(m, now, b_ready, 0, T_B, n);
            prev_b = 0;
            continue;
        }
        if (q.n >= SIM_PEND) sq_force(m, &q, now);
        struct spair *p = &q.p[q.n++];
        memset(p, 0, sizeof(*p));
        p->frame = n;
        p->push = now;
        p->has_s = why == GEN;
        p->b_ready = b_ready;
        p->b_desired = zfg_desired(t.b, m->refresh);
        p->b_apply = zfg_apply_at(p->b_desired, m->latch, m->margin);
        if (p->has_s) {
            int64_t ready = now + m->gen_ns + rnd(m->gen_ns / 5);
            if (ready < b_ready) ready = b_ready;
            p->s_ready = ready;
            p->s_desired = zfg_desired(t.s, m->refresh);
            p->s_apply = zfg_apply_at(p->s_desired, m->latch, m->margin);
            p->s_last = p->b_apply + m->margin - m->refresh;
            if (p->s_last < p->s_apply + MS) p->s_last = p->s_apply + MS;
            job_done[jobs] = ready + DISPATCH;
            job_arrival[jobs++] = now;
            m->s_made++;
        }
        if (n >= 20 && t.b - now > m->b_after_max) m->b_after_max = t.b - now;
        prev_b = t.b;
    }
    while (q.n) sq_step(m, &q, INT64_MAX / 2);
    m->frames = SIM_FRAMES;

    /* SurfaceFlinger */
    memset(m->s_hold, 0, sizeof(m->s_hold));
    memset(m->b_hold, 0, sizeof(m->b_hold));
    int head = 0, kind = -1, frame = -1;
    const int64_t end = arrivals[SIM_FRAMES] + 300 * MS;
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
    m->dpt = 1;
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
    sim_init(m, 8333333, 5 * MS, 300000);
    m->period_of = period_step;
    sim_run(m);
    CHECK(m->order_bad == 0, "60 -> 30: %d outputs closer than a refresh", m->order_bad);
    CHECK(m->b_lost_after_warmup == 0, "60 -> 30: %d real frames never shown", m->b_lost_after_warmup);
    free(m);
}

/* SurfaceFlinger committing later (0.8 refresh) or earlier than assumed, up to two refreshes ahead (its early
 * offsets around mode switches), at 30, 40 and 60 fps and every phase: S and B slip together, no S is taken
 * with its B, no real frame is lost. */
static void sim_latch_tolerance(void) {
    const int64_t sf[] = {6666666, 10000000, 10500000, 12000000, 16600000};
    const int64_t periods[] = {33333333, 25000000, 16666666};
    for (unsigned pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++)
        for (unsigned i = 0; i < sizeof(sf) / sizeof(sf[0]); i++) {
            int made = 0, shown = 0, lost = 0;
            for (int ph = 0; ph < 8; ph++) {
                struct sim *m = calloc(1, sizeof(*m));
                sim_init(m, 8333333, 5 * MS, 300000);
                g_p1 = periods[pi];
                m->sf_latch = sf[i];
                m->phase = ph * 8333333LL / 8;
                sim_run(m);
                made += m->s_made;
                shown += m->s_shown;
                lost += m->b_lost_after_warmup;
                free(m);
            }
            CHECK(lost == 0, "%.0f fps, SurfaceFlinger %.1f ms ahead: %d real frames never shown", 1e9 / periods[pi],
                  sf[i] / 1e6, lost);
            CHECK(shown >= made * 97 / 100, "%.0f fps, SurfaceFlinger %.1f ms ahead: %d of %d S shown", 1e9 / periods[pi],
                  sf[i] / 1e6, shown, made);
        }
}

/* The tablet at its native size: S is complete 46-60 ms after B arrives (B itself rendered a few ms before),
 * longer than a real-frame period. The pairs queue: every S is shown, no pair is pushed out early. */
static void sim_slow_generation(void) {
    const struct { int64_t period, render, gen; } cases[] = {{35000000, 48 * MS, 54 * MS}, {28169014, 42 * MS, 46 * MS},
                                                             {33333333, 40 * MS, 50 * MS}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct sim *m = calloc(1, sizeof(*m));
        sim_init(m, 8333333, cases[i].gen, 1 * MS);
        m->render_ns = cases[i].render;
        m->peak = 144.0f;
        m->running = 120.0f;
        g_p1 = cases[i].period;
        sim_run(m);
        const double fps = 1e9 / (double)cases[i].period;
        CHECK(m->b_lost_after_warmup == 0, "%.1f fps, S after %lld ms: %d real frames never shown", fps,
              (long long)(cases[i].gen / MS), m->b_lost_after_warmup);
        CHECK(m->s_made >= (SIM_FRAMES - 20) * 95 / 100, "%.1f fps, S after %lld ms: %d S made", fps,
              (long long)(cases[i].gen / MS), m->s_made);
        CHECK(m->s_shown >= m->s_made * 95 / 100, "%.1f fps, S after %lld ms: %d of %d S shown", fps,
              (long long)(cases[i].gen / MS), m->s_shown, m->s_made);
        CHECK(m->forced == 0, "%.1f fps: %d pairs pushed out by the queue", fps, m->forced);
        CHECK(m->order_bad == 0, "%.1f fps: %d outputs closer than a refresh", fps, m->order_bad);
        double s, b;
        hold_means(m, 20, SIM_FRAMES - 2, &s, &b);
        CHECK(s > 1.6 && b > 1.6, "%.1f fps, S after %lld ms: S held %.2f, B held %.2f refreshes", fps,
              (long long)(cases[i].gen / MS), s, b);
        free(m);
    }
}

/* No desired present time: S and B are still handed over a refresh apart, so S is seen. */
static void sim_without_desired_present_time(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 5 * MS, 1 * MS);
    m->dpt = 0;
    g_p1 = 33333333;
    sim_run(m);
    CHECK(m->b_lost_after_warmup == 0, "no DPT: %d real frames never shown", m->b_lost_after_warmup);
    CHECK(m->s_shown >= m->s_made * 9 / 10, "no DPT: %d of %d S shown", m->s_shown, m->s_made);
    free(m);
}

/* The panel held at 60 Hz below a peak of 120 (a power or thermal cap): a 60 or 61 fps game is shown as it
 * comes once the panel did not come up, and B is never far behind its arrival meanwhile. */
static void sim_panel_below_the_peak(void) {
    const int64_t periods[] = {16666666, 16393442, 16129032};
    for (unsigned i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
        struct sim *m = calloc(1, sizeof(*m));
        sim_init(m, 16666666, 3 * MS, 300000);
        m->peak = 120.0f;
        g_p1 = periods[i];
        sim_run(m);
        const double fps = 1e9 / (double)periods[i];
        CHECK(m->first_fast >= 0 && m->first_fast <= 130, "%.1f fps on 60 Hz (peak 120): shown as it comes from frame %d",
              fps, m->first_fast);
        int direct = 0;
        for (int n = 140; n < SIM_FRAMES; n++) direct += m->fast[n];
        CHECK(direct == SIM_FRAMES - 140, "%.1f fps on 60 Hz: %d of %d later frames shown as they come", fps, direct,
              SIM_FRAMES - 140);
        const int64_t bound = 3 * MS + 2 * MS + 16666666 * 2 + 16666666 + 16666666;
        CHECK(m->b_after_max <= bound, "%.1f fps on 60 Hz: B up to %lld ms after arrival", fps,
              (long long)(m->b_after_max / MS));
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
    schedule_does_not_drift();
    desired_and_apply_times();
    votes();
    too_fast();
    fast_against_the_running_rate();
    sim_30_on_120();
    sim_31_on_120_after_restart();
    sim_62_on_120();
    sim_60_on_120();
    sim_rate_step();
    sim_latch_tolerance();
    sim_slow_generation();
    sim_without_desired_present_time();
    sim_panel_below_the_peak();
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("zerofg_pace_test: all passed\n");
    return 0;
}
