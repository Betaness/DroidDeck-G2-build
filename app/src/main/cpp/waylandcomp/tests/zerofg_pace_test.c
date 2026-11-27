/* Tests of src/zerofg_pace.c: cc -O2 -Wall -Wextra -Werror -Isrc tests/zerofg_pace_test.c src/zerofg_pace.c
 * src/scene_map.c src/game_fill.c -lm */
#include "zerofg_pace.h"
#include "game_fill.h"
#include "scene_map.h"

#include <math.h>
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
    CHECK(pick(32.0, m120, 7, 120.0f, 60.0f) == 60.0f, "64 outputs after 60: 60 stays");
    CHECK(pick(34.0, m120, 7, 120.0f, 60.0f) == 120.0f, "68 outputs after 60: 120");
    /* no modes given: whole fractions of the peak */
    CHECK(pick(30, NULL, 0, 120.0f, 0) == 60.0f, "30 fps, modes unknown: 120 / 2");
    CHECK(pick(20, NULL, 0, 120.0f, 0) == 40.0f, "20 fps, modes unknown: 120 / 3");
    CHECK(pick(25, NULL, 0, 120.0f, 0) == 120.0f, "25 fps, modes unknown: 50 fits no fraction, the peak");
    CHECK(pick(60.4, NULL, 0, 120.0f, 0) == 120.0f, "60.4 fps: 120");
}

/* The tablet at about 30 fps (2x 59.8 .. 61.6) with the panel capped at 120 by the system: the session's
 * peak is 144 (or 120), the panel runs at 120. */
static void votes_near_30_on_the_tablet(void) {
    const float m[] = {30, 48, 50, 60, 90, 120, 144};
    const double fps[] = {29.9, 30.0, 30.8};
    for (unsigned i = 0; i < sizeof(fps) / sizeof(fps[0]); i++) {
        const double f = fps[i];
        CHECK(pick(f, m, 7, 144.0f, 0) == 60.0f, "%.1f fps, peak 144: 60, got %.1f", f, pick(f, m, 7, 144.0f, 0));
        CHECK(pick(f, m, 7, 144.0f, 60.0f) == 60.0f, "%.1f fps after 60: 60", f);
        CHECK(pick(f, m, 7, 144.0f, 144.0f) == 60.0f, "%.1f fps after 144: 60", f);
        CHECK(pick(f, m, 7, 120.0f, 0) == 60.0f, "%.1f fps, peak 120: 60", f);
        CHECK(pick(f, m, 7, 120.0f, 120.0f) == 60.0f, "%.1f fps after 120: 60, the lower rate that fits", f);
        struct zfg_fast fs = {0, 0};
        CHECK(!zfg_fast_update(&fs, 1000 * MS, (int64_t)(1e9 / f), 120.0f, 144.0f), "%.1f fps on 120 (peak 144): not fast",
              f);
    }
    /* a vote of 60 stays while 2x the real rate is at most 12 % above or below 60 */
    CHECK(pick(26.5, m, 7, 144.0f, 60.0f) == 60.0f, "53 outputs after 60: 60");
    CHECK(pick(26.25, m, 7, 144.0f, 60.0f) == 144.0f, "52.5 outputs after 60: 144");
    CHECK(pick(27.75, m, 7, 144.0f, 0) == 144.0f, "55.5 outputs, no vote yet: 144");
    CHECK(pick(33.5, m, 7, 144.0f, 60.0f) == 60.0f, "67 outputs after 60: 60");
    CHECK(pick(33.75, m, 7, 144.0f, 60.0f) == 144.0f, "67.5 outputs after 60: 144");
    /* x3 at 31.4 .. 33.5 fps (94 .. 100.5 outputs) after 90: 90 stays; from 144 only once 90 fits within 3 % */
    const double near90[] = {31.4, 32.0, 32.5, 33.5};
    for (unsigned i = 0; i < sizeof(near90) / sizeof(near90[0]); i++) {
        CHECK(zfg_vote_pick_n((int64_t)(1e9 / near90[i]), 3, m, 7, 144.0f, 90.0f) == 90.0f,
              "x3 at %.1f fps after 90: 90", near90[i]);
        CHECK(zfg_vote_pick_n((int64_t)(1e9 / near90[i]), 3, m, 7, 144.0f, 144.0f) == 144.0f,
              "x3 at %.1f fps after 144: 144", near90[i]);
    }
    CHECK(zfg_vote_pick_n((int64_t)(1e9 / 30.8), 3, m, 7, 144.0f, 144.0f) == 90.0f, "x3 at 30.8 fps after 144: 90");
    CHECK(zfg_vote_pick_n((int64_t)(1e9 / 33.7), 3, m, 7, 144.0f, 90.0f) == 144.0f, "x3 at 33.7 fps after 90: 144");
}

/* Intervals of a game at about `fps`: each within 10 % of the mean, a 1.7x one every 23rd. */
static uint32_t jit_state;
static int64_t jittered(double fps, int n) {
    jit_state = jit_state * 1664525u + 1013904223u;
    const double u = (double)(jit_state >> 8) / 16777216.0;
    double iv = 1e9 / fps * (0.9 + 0.2 * u);
    if (n % 23 == 22) iv *= 1.7;
    return (int64_t)iv;
}

/* Frames as zfg_present takes them: the trimmed-mean period, then the vote. Returns the vote at the end and
 * counts the frames after `settle` whose vote was not `want`. */
static float vote_run(double fps, float peak, int frames, int64_t settle, float want, struct zfg_vote *v,
                      int64_t *t, int *off) {
    const float m[] = {30, 48, 50, 60, 90, 120, 144};
    struct zfg_period p;
    zfg_period_reset(&p);
    const int64_t t0 = *t;
    float hz = v->hz;
    for (int n = 0; n < frames; n++) {
        const int64_t iv = jittered(fps, n);
        *t += iv;
        if (!zfg_is_gap(iv, zfg_period_get(&p))) zfg_period_add(&p, iv);
        hz = zfg_vote_step(v, *t, zfg_period_get(&p), m, 7, peak);
        if (*t - t0 >= settle && hz != want) (*off)++;
    }
    return hz;
}

static void vote_steady_through_uneven_frames(void) {
    const double fps[] = {29.9, 30.0, 30.8};
    const float peaks[] = {144.0f, 120.0f};
    for (unsigned i = 0; i < sizeof(fps) / sizeof(fps[0]); i++)
        for (unsigned k = 0; k < 2; k++) {
            struct zfg_vote v;
            zfg_vote_reset(&v);
            jit_state = 7 + i;
            int64_t t = 1000 * MS;
            int off = 0;
            const float hz = vote_run(fps[i], peaks[k], (int)(fps[i] * 120), 3000 * MS, 60.0f, &v, &t, &off);
            CHECK(hz == 60.0f && off == 0, "%.1f fps, peak %.0f, frames 10 %% uneven: 60 after 3 s, got %.1f, %d "
                  "frames not 60", fps[i], peaks[k], hz, off);
        }
    /* the vote follows a real change of rate, after the hold */
    const float m[] = {30, 48, 50, 60, 90, 120, 144};
    struct zfg_vote v;
    zfg_vote_reset(&v);
    jit_state = 3;
    int64_t t = 1000 * MS;
    int off = 0;
    vote_run(30.0, 144.0f, 300, 3000 * MS, 60.0f, &v, &t, &off);
    CHECK(v.hz == 60.0f && off == 0, "30 fps: 60, %d frames not", off);
    const int64_t step = t;
    int64_t moved = 0;
    for (int n = 0; n < 200 && !moved; n++) {
        const int64_t iv = (int64_t)(1e9 / 45.0);
        t += iv;
        if (zfg_vote_step(&v, t, iv, m, 7, 144.0f) != 60.0f) moved = t - step;
    }
    CHECK(v.hz == 90.0f, "30 then 45 fps: 90, got %.1f", v.hz);
    CHECK(moved >= ZFG_VOTE_HOLD_FAR_NS && moved <= ZFG_VOTE_HOLD_FAR_NS + 500 * MS,
          "the vote moved %lld ms after the rate changed (far above 60)", (long long)(moved / MS));
}

static void vote_steps(void) {
    const float m[] = {30, 48, 50, 60, 90, 120, 144};
    struct zfg_vote v;
    zfg_vote_reset(&v);
    CHECK(zfg_vote_step(&v, 1000 * MS, 0, m, 7, 144.0f) == 0.0f, "period unknown: no vote");
    int64_t t = 1000 * MS;
    for (int n = 1; n < ZFG_PERIOD_HIST + ZFG_VOTE_FIRST; n++) {
        t += 33333333;
        CHECK(zfg_vote_step(&v, t, 33333333, m, 7, 144.0f) == 0.0f, "frame %d with a period: no vote yet", n);
    }
    t += 33333333;
    CHECK(zfg_vote_step(&v, t, 33333333, m, 7, 144.0f) == 60.0f, "the first vote after %d frames",
          ZFG_PERIOD_HIST + ZFG_VOTE_FIRST);
    /* 20 fps for 500 ms (the period of the vote moves towards 50 ms): 60 stays */
    for (int n = 0; n < 10; n++) {
        t += 50 * MS;
        CHECK(zfg_vote_step(&v, t, 50 * MS, m, 7, 144.0f) == 60.0f, "a stretch at 20 fps, frame %d: 60 stays", n);
    }
    for (int n = 0; n < 30; n++) {
        t += 33333333;
        zfg_vote_step(&v, t, 33333333, m, 7, 144.0f);
    }
    CHECK(v.hz == 60.0f && !v.away, "back at 30 fps: 60, nothing pending (%lld ms)", (long long)(v.away / MS));
    zfg_vote_reset(&v);
    CHECK(v.hz == 0.0f && v.period == 0, "reset: no vote");
}

static void import_formats(void) {
    CHECK(zfg_import_format(ZFG_AHB_BGRA8, 0) == ZFG_VK_B8G8R8A8_UNORM, "BGRA_8888 without a format: B8G8R8A8_UNORM");
    CHECK(zfg_import_format(ZFG_AHB_BGRA8, ZFG_VK_B8G8R8A8_UNORM) == ZFG_VK_B8G8R8A8_UNORM, "BGRA_8888 as reported");
    CHECK(zfg_import_format(ZFG_AHB_BGRA8, 50) == 50, "the driver's own format first (B8G8R8A8_SRGB)");
    CHECK(zfg_import_format(1, 37) == 37, "R8G8B8A8_UNORM as reported");
    CHECK(zfg_import_format(1, 0) == 0, "R8G8B8A8 without a format: none");
    CHECK(zfg_import_format(0x2b, 0) == 0, "R10G10B10A2 without a format: none");
    CHECK(zfg_import_format(0x23, 0) == 0, "YCbCr without a format: none");
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
 * for V. A pair has n outputs: n - 1 generated frames (phases 1/n .. (n - 1)/n, from one generation) and B. */

#define SIM_GEN (ZFG_MULT_MAX - 1)
/* a transaction's phase: 0 .. SIM_GEN - 1 the generated frames, T_B the real one; T_REMAP: the layer's geometry
 * alone (vortek_present_remap after a mapping change, for a real frame the layer shows) */
enum { T_B = SIM_GEN, T_REMAP };
struct tx {
    int64_t applied, ready, desired;
    int phase, frame, map; /* map: the scene-to-output mapping it was placed with (game fill's crop) */
};

#define SIM_FRAMES 600
#define SIM_TX (ZFG_MULT_MAX * SIM_FRAMES + 8)
#define SIM_JOBS 3          /* zerofg_zc.c's ZJOBS */
#define SIM_PEND 4          /* ZPEND */
#define DISPATCH 200000LL   /* the event loop runs a due timer or a completion this long after it is due */

struct sim {
    /* input */
    int64_t refresh, sf_latch, phase, margin, latch, gen_ns, gen_more, render_ns, jitter;
    int render_slow_every;  /* every this many frames B takes render_slow_ns to render (0: never) */
    int64_t render_slow_ns;
    float peak, running;  /* running: the panel's rate as zerofg_zc.c sees it (0: 1e9 / refresh) */
    int dpt;
    int mult;           /* the multiplier asked for (0: x2) */
    double gpu_first, gpu_phase; /* GPU time of a pair's first and of each further call zfg_mult_update is given */
    int64_t (*period_of)(int n);
    int restart_at;     /* forget the previous frame at this frame (a hidden window, a new swapchain) */
    int64_t (*map_at)(int64_t t); /* the mapping in force at t (0: none given, all 0) */
    const int64_t *map_change;    /* when it changes, in order */
    int map_changes;
    int s_map_at_arrival; /* place generated frames with the mapping of their real frame's arrival (not done) */
    /* output */
    int frames, s_made, s_shown, b_shown, b_lost_after_warmup, s_early, s_missed, s_late, forced, first_fast;
    int s_early_ph;     /* generated frames left out at the start of a pair that keeps later ones (early) */
    int bare_after_warmup; /* pairs from frame 20 on whose B was shown without any of their generated frames */
    int64_t b_after_max; /* longest from arrival to B's target after the warm-up */
    int s_hold[SIM_FRAMES][SIM_GEN], b_hold[SIM_FRAMES], fast[SIM_FRAMES], n_of[SIM_FRAMES];
    int64_t arrival[SIM_FRAMES];
    int map_back;       /* refreshes that show an older mapping than one shown before */
    int s_across_map;   /* generated frames shown whose real frame arrived before the mapping changed */
    int order_bad;      /* an output's target less than a refresh after the one before */
    int spacing_bad;    /* an output's target less than zfg_spacing after the one before */
    int mult_lo, mult_hi;
    struct tx q[SIM_TX];
    int nq;
};

static uint32_t rng_state;
static int64_t rnd(int64_t span) { /* -span .. span */
    rng_state = rng_state * 1664525u + 1013904223u;
    if (span <= 0) return 0;
    return (int64_t)(rng_state >> 8) % (2 * span + 1) - span;
}

/* An output to SurfaceFlinger, placed with the mapping in force when it is handed over, as zerofg_zc.c's
 * pend_apply_s and pend_apply_b place it. */
static void push(struct sim *m, int64_t applied, int64_t ready, int64_t desired, int phase, int frame) {
    if (m->nq >= SIM_TX) return;
    const int64_t placed = phase != T_B && m->s_map_at_arrival ? m->arrival[frame] : applied;
    const int map = m->map_at ? (int)m->map_at(placed) : 0;
    m->q[m->nq++] = (struct tx){applied, ready > applied ? ready : applied, desired, phase, frame, map};
}

/* The pairs waiting, as zerofg_zc.c's pend_run handles them. */
struct spair {
    int frame, ns, next, first; /* generated frame k is phase first + k */
    int64_t push, at, s_ready, b_ready;
    struct zfg_pair_times t;
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

/* The oldest pair up to `until`: each generated frame once the generation is complete and its apply time has
 * come, before its s_last (else left out then); B at its apply time. 0: nothing more happens before `until`. */
static int sq_step(struct sim *m, struct squeue *q, int64_t until) {
    struct spair *p = &q->p[0];
    if (p->at < q->cursor) p->at = q->cursor;
    while (p->next < p->ns) {
        const int k = p->next;
        const int64_t at = p->at, apply = p->t.s_apply[k], last = p->t.s_last[k];
        /* the first moment pend_run looks at it with the generation complete and its apply time come */
        int64_t x;
        if (p->s_ready <= (at > apply ? at : apply)) x = at >= apply ? at : apply + DISPATCH;
        else x = p->s_ready + DISPATCH; /* its completion */
        const int act = zfg_s_action(p->s_ready <= x, x, apply, last, 0);
        if (act != ZFG_S_SHOW) x = at > last ? at : last + DISPATCH; /* the timer at s_last */
        if (x > until) return 0;
        if (act == ZFG_S_SHOW) push(m, x, p->s_ready, m->dpt ? p->t.s_desired[k] : 0, p->first + k, p->frame);
        else m->s_missed++;
        p->next++;
        p->at = x;
    }
    const int64_t y = p->at >= p->t.b_apply ? p->at : p->t.b_apply + DISPATCH;
    if (y > until) return 0;
    push(m, y, p->b_ready, m->dpt ? p->t.b_desired : 0, T_B, p->frame);
    q->cursor = y;
    sq_pop(q);
    return 1;
}

/* The oldest pair now: its generated frames only if complete and before their s_last. */
static void sq_force(struct sim *m, struct squeue *q, int64_t now) {
    struct spair *p = &q->p[0];
    for (; p->next < p->ns; p->next++) {
        const int k = p->next;
        if (zfg_s_action(p->s_ready <= now, now, p->t.s_apply[k], p->t.s_last[k], 1) == ZFG_S_SHOW)
            push(m, now, p->s_ready, m->dpt ? p->t.s_desired[k] : 0, p->first + k, p->frame);
        else m->s_missed++;
    }
    push(m, now, p->b_ready, m->dpt ? p->t.b_desired : 0, T_B, p->frame);
    q->cursor = now;
    sq_pop(q);
    m->forced++;
}

static void sim_run(struct sim *m) {
    struct zfg_period per;
    zfg_period_reset(&per);
    struct zfg_fast fast = {0, 0};
    struct zfg_mult mult;
    zfg_mult_reset(&mult);
    int64_t last = 0, prev_b = 0, lead = 0, arrival = 1000 * MS;
    struct zfg_ready ready_hist;
    zfg_ready_reset(&ready_hist);
    int have_prev = 0;
    const float running = m->running > 0.0f ? m->running : (float)(1e9 / (double)m->refresh);
    int64_t arrivals[SIM_FRAMES + 1];
    for (int n = 0; n <= SIM_FRAMES; n++) {
        arrivals[n] = arrival + rnd(m->jitter);
        if (n < SIM_FRAMES) m->arrival[n] = arrivals[n];
        arrival += m->period_of(n);
    }
    static struct squeue q;
    memset(&q, 0, sizeof(q));
    /* generations in flight: completion, arrival and Interpolate calls, in submission order */
    int64_t job_done[SIM_FRAMES], job_arrival[SIM_FRAMES];
    int job_calls[SIM_FRAMES];
    int jobs = 0, jobs_seen = 0;
    m->first_fast = -1;
    m->mult_lo = ZFG_MULT_MAX + 1;
    m->mult_hi = 0;
    for (int n = 0; n < SIM_FRAMES; n++) {
        const int64_t now = arrivals[n];
        while (q.n && sq_step(m, &q, now)) {}
        /* note_ready: completions the event loop saw before this frame */
        while (jobs_seen < jobs && job_done[jobs_seen] <= now) {
            lead = zfg_lead_update(lead, job_done[jobs_seen] - job_arrival[jobs_seen]);
            zfg_ready_add(&ready_hist, job_done[jobs_seen] - job_arrival[jobs_seen]);
            jobs_seen++;
        }
        int running_jobs = 0, running_calls = 0;
        for (int i = jobs_seen; i < jobs; i++)
            if (job_done[i] > now) {
                running_jobs++;
                running_calls += job_calls[i];
            }
        if (n == m->restart_at) { have_prev = 0; prev_b = 0; }
        const int64_t iv = last ? now - last : 0;
        last = now;
        if (iv > 0 && !zfg_is_gap(iv, zfg_period_get(&per))) zfg_period_add(&per, iv);
        const int64_t period = zfg_period_get(&per);
        const int64_t render = m->render_slow_every && n % m->render_slow_every == 5 ? m->render_slow_ns : m->render_ns;
        const int64_t b_ready = now + (render > 0 ? render : MS);
        enum { GEN, DIRECT, NO_S } why = GEN;
        zfg_fast_update(&fast, now, period, running, m->peak);
        const struct zfg_mult_in min = {.want = m->mult ? m->mult : 2, .period = period, .refresh_hz = running,
                                        .peak_hz = m->peak, .gpu_first_ms = m->gpu_first, .gpu_phase_ms = m->gpu_phase,
                                        .ready_ns = zfg_ready_get(&ready_hist)};
        const int nm = zfg_mult_update(&mult, now, &min);
        m->n_of[n] = nm;
        if (n >= 20 && nm < m->mult_lo) m->mult_lo = nm;
        if (n >= 20 && nm > m->mult_hi) m->mult_hi = nm;
        struct zfg_slots t = {0};
        const int64_t l = zfg_target_lead(lead, period, m->margin, m->latch, m->refresh);
        if (period > 0) t = zfg_schedule_n(now, prev_b, period, l, zfg_slack(period, m->refresh), m->refresh, nm);
        if (!have_prev || period <= 0) why = DIRECT;
        else if (fast.on) {
            why = DIRECT;
            m->fast[n] = 1;
            if (m->first_fast < 0) m->first_fast = n;
        } else if (t.no_s) {
            why = NO_S;
            m->s_early++;
        } else if (running_jobs >= SIM_JOBS || !zfg_calls_fit(running_calls, zfg_slots_shown(&t))) {
            why = NO_S;
            m->s_late++;
        }
        if (why == GEN && n >= 20) m->s_early_ph += t.first;
        if (why != DIRECT && prev_b) {
            int64_t before = prev_b;
            const int64_t gap = zfg_spacing(period, nm, 0);
            for (int k = t.first; why == GEN && k < nm - 1; k++) {
                const int64_t s = zfg_slot_s(&t, k);
                if (s < before + m->refresh) m->order_bad++;
                if (s < before + gap) m->spacing_bad++;
                before = s;
            }
            if (t.b < before + m->refresh) m->order_bad++;
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
        p->push = p->at = now;
        if (why != GEN) {
            t.no_s = 1;
            t.first = t.n - 1;
        }
        zfg_pair_times(&t, m->refresh, m->latch, m->margin, &p->t);
        p->ns = p->t.ns;
        p->first = t.first;
        p->b_ready = b_ready;
        if (p->ns) {
            int64_t ready = now + m->gen_ns + (p->ns - 1) * m->gen_more + rnd(m->gen_ns / 5);
            if (ready < b_ready) ready = b_ready;
            p->s_ready = ready;
            job_done[jobs] = ready + DISPATCH;
            job_calls[jobs] = p->ns;
            job_arrival[jobs++] = now;
            m->s_made += p->ns;
        }
        if (n >= 20 && t.b - now > m->b_after_max) m->b_after_max = t.b - now;
        prev_b = t.b;
    }
    while (q.n) sq_step(m, &q, INT64_MAX / 2);
    m->frames = SIM_FRAMES;

    /* A mapping change re-places the layer at once when the last frame handed to it is a real frame
     * (sc_layer_vortek_remap); a generated one (a pool buffer) keeps its place until the next output. The geometry
     * goes behind the transactions applied before. */
    for (int c = 0; c < m->map_changes && m->nq < SIM_TX; c++) {
        const int64_t at = m->map_change[c];
        int i = 0;
        while (i < m->nq && m->q[i].applied <= at) i++;
        if (!i || m->q[i - 1].phase != T_B) continue;
        memmove(&m->q[i + 1], &m->q[i], (size_t)(m->nq - i) * sizeof(m->q[0]));
        m->q[i] = (struct tx){at, at, 0, T_REMAP, m->q[i - 1].frame, (int)m->map_at(at)};
        m->nq++;
    }

    /* SurfaceFlinger */
    memset(m->s_hold, 0, sizeof(m->s_hold));
    memset(m->b_hold, 0, sizeof(m->b_hold));
    int head = 0, phase = -1, frame = -1, map = 0, map_hi = 0;
    const int64_t end = arrivals[SIM_FRAMES] + 300 * MS;
    for (int64_t v = 1000 * MS + m->phase; v < end; v += m->refresh) {
        const int64_t commit = v - m->sf_latch;
        while (head < m->nq) {
            const struct tx *x = &m->q[head];
            if (x->applied > commit || x->ready > commit || (x->desired && x->desired >= v)) break;
            head++;
            if (x->phase == T_REMAP) {
                if (phase == T_B) map = x->map;
                continue;
            }
            phase = x->phase;
            frame = x->frame;
            map = x->map;
        }
        if (frame < 0) continue;
        if (map < map_hi) m->map_back++;
        else map_hi = map;
        if (phase == T_B) m->b_hold[frame]++;
        else {
            if (!m->s_hold[frame][phase] && m->map_at && m->map_at(m->arrival[frame]) != map) m->s_across_map++;
            m->s_hold[frame][phase]++;
        }
    }
    for (int n = 0; n < SIM_FRAMES; n++) {
        int any = 0;
        for (int k = 0; k < SIM_GEN; k++)
            if (m->s_hold[n][k]) {
                m->s_shown++;
                any = 1;
            }
        if (m->b_hold[n]) m->b_shown++;
        else if (n >= 20) m->b_lost_after_warmup++;
        if (n >= 20 && n < SIM_FRAMES - 2 && m->b_hold[n] && !any) m->bare_after_warmup++;
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

/* x2: the mean refreshes S (phase 1/2) and B were held, over the frames from .. to. */
static void hold_means(const struct sim *m, int from, int to, double *s, double *b) {
    int ns = 0, nb = 0;
    long hs = 0, hb = 0;
    for (int n = from; n < to; n++) {
        if (m->s_hold[n][0]) { hs += m->s_hold[n][0]; ns++; }
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
    for (int n = 20; n < SIM_FRAMES - 2; n++) even += m->s_hold[n][0] == 2 && m->b_hold[n] == 2;
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

/* ---- x3 and x4 ---- */

static void schedule_n_spaces_the_outputs(void) {
    const int64_t r120 = 8333333, r144 = 6944444, p = 33333333, lead = 20 * MS;
    CHECK(zfg_spacing(p, 3, r120) == (int64_t)(p / 3.0 * ZFG_SPACING), "x3 at 30 fps on 120 Hz: 80 %% of 11.1 ms");
    CHECK(zfg_spacing(p, 4, r144) == r144, "x4 at 30 fps on 144 Hz: a refresh (more than 80 %% of 8.3 ms)");
    CHECK(zfg_spacing(p, 2, r120) == r120 && zfg_spacing(p, 2, r144) == r144, "x2: a refresh, as before x3 and x4");
    CHECK(zfg_spacing(p, 9, r120) == zfg_spacing(p, ZFG_MULT_MAX, r120), "above x4: x4");
    for (int n = 2; n <= ZFG_MULT_MAX; n++) {
        const int64_t q = p / n;
        int64_t prev_b = 0;
        for (int f = 0; f < 60; f++) {
            const int64_t arrival = 1000 * MS + f * p;
            const struct zfg_slots t = zfg_schedule_n(arrival, prev_b, p, lead, zfg_slack(p, r120), r120, n);
            CHECK(!t.no_s && t.n == n, "x%d frame %d: generated frames", n, f);
            CHECK(t.step == (q > r120 ? q : r120), "x%d: step %lld", n, (long long)t.step);
            CHECK(t.b == zfg_slot_s(&t, 0) + (n - 1) * t.step, "x%d: b n - 1 steps after the first", n);
            if (prev_b) {
                CHECK(absl(zfg_slot_s(&t, 0) - prev_b - q) <= n, "x%d frame %d: the first a period / n after the "
                      "previous b, got %lld", n, f, (long long)(zfg_slot_s(&t, 0) - prev_b));
                CHECK(absl(t.b - prev_b - p) <= n || q < r120, "x%d frame %d: b a period after the previous b", n, f);
            }
            for (int k = 1; k < n - 1; k++)
                CHECK(zfg_slot_s(&t, k) - zfg_slot_s(&t, k - 1) == t.step, "x%d: evenly spaced", n);
            prev_b = t.b;
        }
    }
    /* no room for the first generated frame up to lead + slack (the frame came 2 ms early against the rhythm):
     * the first is left out, the others and b a step earlier than the rhythm */
    const struct zfg_slots c = zfg_schedule_n(30 * MS, 52 * MS, p, lead, r120, r120, 4);
    CHECK(!c.no_s && c.first == 1 && zfg_slots_shown(&c) == 2 && zfg_slot_s(&c, 0) == 0, "x4, 2 ms early: the first "
          "left out (first %d, shown %d)", c.first, zfg_slots_shown(&c));
    CHECK(zfg_slot_s(&c, 1) == 52 * MS + p / 4 && zfg_slot_s(&c, 2) == 52 * MS + 2 * (p / 4) &&
          c.b == 52 * MS + 3 * (p / 4), "x4, 2 ms early: phases 2/4 and 3/4 a step apart after the previous b, b a "
          "step after them, got %lld", (long long)(c.b - 52 * MS));
    /* no room for any: no generated frames, b at least the spacing after the previous b */
    const struct zfg_slots c2 = zfg_schedule_n(30 * MS, 70 * MS, p, lead, r120, r120, 4);
    CHECK(c2.no_s && zfg_slots_shown(&c2) == 0 && zfg_slot_s(&c2, 2) == 0, "x4 without room: none");
    CHECK(c2.b == 70 * MS + r120, "x4 without room: b a refresh after the previous b, got %lld",
          (long long)(c2.b - 70 * MS));
    /* x3 at 50 fps on 120 Hz: a third of a period after the previous b is less than a refresh, the first is pushed
     * to a refresh after it, the others follow a refresh apart */
    const struct zfg_slots d = zfg_schedule_n(30 * MS, 44 * MS, 20 * MS, lead, 2 * r120, r120, 3);
    CHECK(!d.no_s && d.s == 44 * MS + r120 && d.step == r120 && d.b == d.s + 2 * r120,
          "x3 at 50 fps: pushed to a refresh after the previous b, got %lld", (long long)(d.s - 44 * MS));
    CHECK(zfg_schedule_n(0, 0, p, lead, r120, r120, 2).b == zfg_schedule(0, 0, p, lead, r120, r120).b,
          "zfg_schedule is x2");
}

static float pick_n(double fps, int mult, const float *modes, int n, float peak, float prev) {
    return zfg_vote_pick_n((int64_t)(1e9 / fps), mult, modes, n, peak, prev);
}

static void votes_n(void) {
    const float m[] = {30, 48, 50, 60, 90, 120, 144}; /* the tablet's modes */
    CHECK(pick_n(30, 3, m, 7, 144.0f, 0) == 90.0f, "x3 at 30 fps, peak 144: 90, got %.1f", pick_n(30, 3, m, 7, 144.0f, 0));
    CHECK(pick_n(30, 3, m, 7, 120.0f, 0) == 90.0f, "x3 at 30 fps, peak 120: 90");
    CHECK(pick_n(30, 4, m, 7, 144.0f, 0) == 120.0f, "x4 at 30 fps, peak 144: 120");
    CHECK(pick_n(30, 4, m, 7, 120.0f, 0) == 120.0f, "x4 at 30 fps, peak 120: 120");
    CHECK(pick_n(36, 4, m, 7, 144.0f, 0) == 144.0f, "x4 at 36 fps: 144");
    CHECK(pick_n(40, 3, m, 7, 144.0f, 0) == 120.0f, "x3 at 40 fps: 120");
    CHECK(pick_n(48, 3, m, 7, 144.0f, 0) == 144.0f, "x3 at 48 fps: 144");
    CHECK(pick_n(29.5, 4, m, 7, 144.0f, 120.0f) == 120.0f, "x4 at 29.5 fps after 120: 120 stays");
    CHECK(pick_n(30, 4, NULL, 0, 120.0f, 0) == 120.0f, "x4 at 30 fps, modes unknown: the peak");
    CHECK(pick_n(30, 3, NULL, 0, 180.0f, 0) == 90.0f, "x3 at 30 fps, modes unknown: 180 / 2");
    CHECK(pick_n(30, 2, m, 7, 144.0f, 0) == pick(30, m, 7, 144.0f, 0), "zfg_vote_pick is x2");
    /* the vote over the frames follows a multiplier change after the hold: twice the outputs, far above 60 */
    struct zfg_vote v;
    zfg_vote_reset(&v);
    int64_t t = 1000 * MS;
    for (int n = 0; n <= 20 * 30; n++, t += 33333333) zfg_vote_step_n(&v, t, 33333333, 2, 0, m, 7, 144.0f);
    CHECK(v.hz == 60.0f, "x2 at 30 fps: 60");
    t -= 33333333;
    const int64_t t0 = t;
    int64_t moved = 0;
    for (int n = 0; n < 100 && !moved; n++) {
        t += 33333333;
        if (zfg_vote_step_n(&v, t, 33333333, 4, 0, m, 7, 144.0f) == 120.0f) moved = t - t0;
    }
    CHECK(moved >= ZFG_VOTE_HOLD_FAR_NS && moved <= ZFG_VOTE_HOLD_FAR_NS + 100 * MS, "x2 -> x4: 120 after %lld ms",
          (long long)(moved / MS));
    /* x4 -> x3 (90 outputs on 120, 1.33 refreshes each): uneven, within about a second */
    int64_t t1 = t;
    moved = 0;
    for (int n = 0; n < 400 && !moved; n++) {
        t += 33333333;
        if (zfg_vote_step_n(&v, t, 33333333, 3, 0, m, 7, 144.0f) == 90.0f) moved = t - t1;
    }
    CHECK(moved >= ZFG_VOTE_HOLD_FAR_NS && moved <= ZFG_VOTE_HOLD_FAR_NS + 500 * MS, "x4 -> x3: 90 after %lld ms",
          (long long)(moved / MS));
    /* x3 at 30 on 90 -> x2 at 30 (60 outputs, 1.5 refreshes each on 90): uneven, within about a second */
    t1 = t;
    moved = 0;
    for (int n = 0; n < 400 && !moved; n++) {
        t += 33333333;
        if (zfg_vote_step_n(&v, t, 33333333, 2, 0, m, 7, 144.0f) == 60.0f) moved = t - t1;
    }
    CHECK(moved >= ZFG_VOTE_HOLD_FAR_NS && moved <= ZFG_VOTE_HOLD_FAR_NS + 500 * MS, "x3 -> x2 on 90: 60 after %lld ms",
          (long long)(moved / MS));
    /* x4 at 30 on 120 -> x2 (60 outputs, two refreshes each): even, 60 after the long hold */
    zfg_vote_reset(&v);
    for (int n = 0; n <= 20 * 30; n++, t += 33333333) zfg_vote_step_n(&v, t, 33333333, 4, 0, m, 7, 144.0f);
    CHECK(v.hz == 120.0f, "x4 at 30 fps: 120");
    t1 = t;
    moved = 0;
    for (int n = 0; n < 400 && !moved; n++) {
        t += 33333333;
        if (zfg_vote_step_n(&v, t, 33333333, 2, 0, m, 7, 144.0f) == 60.0f) moved = t - t1;
    }
    CHECK(moved >= ZFG_VOTE_HOLD_NS - 50 * MS && moved <= ZFG_VOTE_HOLD_NS + 500 * MS, "x4 -> x2: 60 after %lld ms",
          (long long)(moved / MS));
}

/* zfg_vote_layer before each frame, as zerofg_zc.c runs it: the frames of one layer come to a vote; another
 * layer's frames start over. */
static void vote_layers(void) {
    const float m[] = {30, 48, 50, 60, 90, 120, 144};
    struct zfg_vote v;
    zfg_vote_reset(&v);
    int64_t t = 1000 * MS;
    int first = 0;
    for (int n = 1; n <= 100 && !first; n++) {
        t += 33333333;
        zfg_vote_layer(&v, 0);
        if (zfg_vote_step_n(&v, t, 33333333, 3, 0, m, 7, 144.0f) > 0.0f) first = n;
    }
    CHECK(first == ZFG_PERIOD_HIST + ZFG_VOTE_FIRST && v.hz == 90.0f, "layer 0, x3 at 30 fps: %.0f after %d frames",
          (double)v.hz, first);
    for (int n = 0; n < 100; n++) {
        t += 33333333;
        zfg_vote_layer(&v, 0);
        zfg_vote_step_n(&v, t, 33333333, 3, 0, m, 7, 144.0f);
    }
    CHECK(v.hz == 90.0f && v.layer == 1, "layer 0 goes on: %.0f", (double)v.hz);
    t += 33333333;
    zfg_vote_layer(&v, 2);
    CHECK(zfg_vote_step_n(&v, t, 33333333, 3, 0, m, 7, 144.0f) == 0.0f && v.layer == 3, "layer 2: starts over");
    zfg_vote_reset(&v);
    zfg_vote_layer(&v, 0);
    CHECK(v.layer == 1 && v.hz == 0.0f, "after a reset: layer 0 starts over");
}

#define P30 33333333LL
#define P40 25000000LL

static void contexts_and_pool(void) {
    CHECK(ZFG_CONTEXTS >= ZFG_MULT_MAX + 1 && ZFG_CONTEXTS <= 8, "frame contexts: N + 1 for x4, ZeroFG's limit 8");
    CHECK(zfg_calls_fit(0, 3) && zfg_calls_fit(3, 3) && zfg_calls_fit(5, 3), "x4: up to 8 calls in flight");
    CHECK(!zfg_calls_fit(6, 3), "x4: a third pair waits");
    CHECK(zfg_calls_fit(6, 2), "x4 with its first generated frame left out: two calls fit");
    CHECK(zfg_calls_fit(2, 1) && zfg_calls_fit(7, 1) && !zfg_calls_fit(8, 1), "x2");
    CHECK(zfg_flight_pairs(0, P30) == 0 && zfg_flight_pairs(50 * MS, 0) == 0, "flight: unknown");
    CHECK(zfg_flight_pairs(20 * MS, P30) == 1 && zfg_flight_pairs(50 * MS, P30) == 2 &&
          zfg_flight_pairs(70 * MS, P30) == 3, "flight: pairs in flight");
    CHECK(zfg_pool_size(2) == 3, "x2: the pool of before");
    CHECK(zfg_pool_size(3) == 6 && zfg_pool_size(4) == 9 && zfg_pool_size(4) == ZFG_POOL_MAX, "x3, x4: three pairs");
}

/* zfg_mult_update with a pair's GPU time given per generated frame (first call and each further one the same), no
 * flight time and no engine limit. */
static int mu(struct zfg_mult *m, int64_t t, int want, int64_t period, float refresh, float peak, double gpu) {
    const struct zfg_mult_in in = {.want = want, .period = period, .refresh_hz = refresh, .peak_hz = peak,
                                   .gpu_first_ms = gpu, .gpu_phase_ms = gpu};
    return zfg_mult_update(m, t, &in);
}

static int mult_at(struct zfg_mult *m, int64_t *t, int64_t until, int want, int64_t period, float refresh, float peak,
                   double gpu) {
    int n = 0;
    for (; *t < until; *t += period) n = mu(m, *t, want, period, refresh, peak, gpu);
    return n;
}

static void multiplier_downgrades(void) {
    struct zfg_mult m;
    int64_t t = 1000 * MS;
    zfg_mult_reset(&m);
    CHECK(mu(&m, t, 4, P30, 120.0f, 144.0f, 2.0) == 4 && m.why == ZFG_CAP_NONE && m.vote_n == 4,
          "x4 at 30 fps on 120 (peak 144): x4");
    CHECK(mu(&m, t, 4, 0, 120.0f, 144.0f, 2.0) == 4, "period unknown: as it was");
    /* the peak: at once */
    zfg_mult_reset(&m);
    CHECK(mu(&m, t, 4, P40, 144.0f, 144.0f, 0) == 3 && m.why == ZFG_CAP_PEAK && m.vote_n == 3,
          "x4 at 40 fps, peak 144: x3 (160 above 144)");
    zfg_mult_reset(&m);
    CHECK(mu(&m, t, 4, 16666666, 120.0f, 120.0f, 0) == 2 && m.why == ZFG_CAP_PEAK,
          "x4 at 60 fps, peak 120: x2");
    CHECK(!strcmp(zfg_cap_name(ZFG_CAP_PEAK), "peak") && !strcmp(zfg_cap_name(ZFG_CAP_GPU), "gpu") &&
          !strcmp(zfg_cap_name(ZFG_CAP_REFRESH), "refresh") && !strcmp(zfg_cap_name(ZFG_CAP_NONE), "none"), "names");
    /* within 5 % kept, back up within 3 % */
    zfg_mult_reset(&m);
    CHECK(mu(&m, t, 4, (int64_t)(1e9 / 37.7), 144.0f, 144.0f, 0) == 4, "x4 at 37.7 fps (150.8): kept");
    CHECK(mu(&m, t + 30 * MS, 4, (int64_t)(1e9 / 38.2), 144.0f, 144.0f, 0) == 3, "38.2 fps (152.8): x3");
    t = 1100 * MS;
    CHECK(mult_at(&m, &t, 5000 * MS, 4, (int64_t)(1e9 / 37.7), 144.0f, 144.0f, 0) == 3, "back at 37.7 fps: x3 stays");
    const int64_t t0 = t;
    int64_t up = 0;
    for (; t < t0 + 4000 * MS && !up; t += 27777777)
        if (mu(&m, t, 4, 27777777, 144.0f, 144.0f, 0) == 4) up = t - t0;
    CHECK(up >= ZFG_MULT_RAISE_NS && up <= ZFG_MULT_RAISE_NS + 50 * MS, "36 fps: x4 again %lld ms later",
          (long long)(up / MS));
    /* the GPU: x4 at 30 fps with 7 ms a generated frame is 21 ms of 33; x3 14 ms fits half the period */
    zfg_mult_reset(&m);
    t = 1000 * MS;
    CHECK(mu(&m, t, 4, P30, 120.0f, 144.0f, 7.0) == 3 && m.why == ZFG_CAP_GPU && m.vote_n == 3,
          "x4, 7 ms a generated frame: x3");
    CHECK(mu(&m, t + P30, 4, P30, 120.0f, 144.0f, 9.0) == 2 && m.why == ZFG_CAP_GPU, "9 ms: x2");
    t += 2 * P30;
    const int64_t fit_from = t;
    up = 0;
    for (; t < fit_from + 15000 * MS && !up; t += P30)
        if (mu(&m, t, 4, P30, 120.0f, 144.0f, 3.0) == 4) up = t - fit_from;
    CHECK(up >= ZFG_GPU_BLOCK_NS - P30 && up <= ZFG_GPU_BLOCK_NS + ZFG_MULT_RAISE_NS + 100 * MS,
          "3 ms after the GPU cap: x4 again %lld ms later (not tried for %lld s)", (long long)(up / MS),
          (long long)(ZFG_GPU_BLOCK_NS / 1000000000LL));
    /* the running rate below the peak (a power cap): the vote asks for x3's rate at once, x3 goes after the grace */
    zfg_mult_reset(&m);
    t = 1000 * MS;
    CHECK(mu(&m, t, 3, P30, 60.0f, 144.0f, 0) == 3 && m.vote_n == 3, "x3 on a 60 Hz panel: x3 at first");
    const int64_t t1 = t;
    int64_t down = 0;
    for (t += P30; t < t1 + 4000 * MS && !down; t += P30)
        if (mu(&m, t, 3, P30, 60.0f, 144.0f, 0) == 2) down = t - t1;
    CHECK(down >= ZFG_FAST_GRACE_NS && down <= ZFG_FAST_GRACE_NS + 50 * MS && m.why == ZFG_CAP_REFRESH,
          "x3 on a panel held at 60: x2 after %lld ms (%s)", (long long)(down / MS), zfg_cap_name(m.why));
    CHECK(m.vote_n == 3, "the vote still for x3, so the panel can come up");
    const int64_t t2 = t;
    up = 0;
    for (; t < t2 + 4000 * MS && !up; t += P30)
        if (mu(&m, t, 3, P30, 90.0f, 144.0f, 0) == 3) up = t - t2;
    CHECK(up >= ZFG_MULT_RAISE_NS && up <= ZFG_MULT_RAISE_NS + 50 * MS, "the panel at 90: x3 again %lld ms later",
          (long long)(up / MS));
    /* the menu: a new multiplier at once */
    CHECK(mu(&m, t, 2, P30, 90.0f, 144.0f, 0) == 2 && m.why == ZFG_CAP_NONE, "x2 asked for: x2");
    CHECK(mu(&m, t + P30, 4, P30, 120.0f, 144.0f, 0) == 4, "x4 asked for: x4 at once");
    CHECK(mu(&m, t + 2 * P30, 9, P30, 120.0f, 144.0f, 0) == 4, "above x4: x4");
}

/* The pairs from frame 20 on whose n outputs were each held `holds` refreshes (0: any of 1 .. 2), and B shown. */
static int pairs_held(const struct sim *m, int n, int holds) {
    int good = 0;
    for (int f = 20; f < SIM_FRAMES - 2; f++) {
        if (m->n_of[f] != n || !m->b_hold[f]) continue;
        int ok = holds ? m->b_hold[f] == holds : m->b_hold[f] <= 2;
        for (int k = 0; k < n - 1 && ok; k++)
            ok = holds ? m->s_hold[f][k] == holds : m->s_hold[f][k] >= 1 && m->s_hold[f][k] <= 2;
        good += ok;
    }
    return good;
}

static struct sim *sim_xn(int mult, int64_t refresh, double fps, float peak) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, refresh, 6 * MS, 1 * MS);
    m->gen_more = 2 * MS;
    m->mult = mult;
    if (peak > 0.0f) m->peak = peak;
    g_p1 = (int64_t)(1e9 / fps);
    sim_run(m);
    return m;
}

/* x3 and x4 at 30 fps on 120 and 144 Hz (and on 90, the rate x3's vote picks): every output on the screen,
 * evenly: one refresh each where the outputs fit the refresh rate, else one or two. */
static void sim_xn_at_30(void) {
    const struct { int mult; int64_t refresh; int holds; const char *what; } cases[] = {
        {4, 8333333, 1, "x4 on 120"}, {3, 11111111, 1, "x3 on 90"}, {3, 8333333, 0, "x3 on 120"},
        {4, 6944444, 0, "x4 on 144"}, {3, 6944444, 0, "x3 on 144"}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct sim *m = sim_xn(cases[i].mult, cases[i].refresh, 30.0, 144.0f);
        const int pairs = SIM_FRAMES - 22;
        CHECK(m->mult_lo == cases[i].mult && m->mult_hi == cases[i].mult, "%s: the multiplier %d .. %d", cases[i].what,
              m->mult_lo, m->mult_hi);
        CHECK(m->b_lost_after_warmup == 0, "%s: %d real frames never shown", cases[i].what, m->b_lost_after_warmup);
        CHECK(m->s_made >= (SIM_FRAMES - 20) * (cases[i].mult - 1) * 95 / 100, "%s: %d generated", cases[i].what,
              m->s_made);
        CHECK(m->s_shown >= m->s_made * 98 / 100, "%s: %d of %d generated frames shown", cases[i].what, m->s_shown,
              m->s_made);
        CHECK(m->order_bad == 0 && m->spacing_bad == 0, "%s: %d outputs closer than a refresh, %d than the spacing",
              cases[i].what, m->order_bad, m->spacing_bad);
        const int even = pairs_held(m, cases[i].mult, cases[i].holds);
        CHECK(even >= pairs * 9 / 10, "%s: %d of %d pairs held evenly", cases[i].what, even, pairs);
        free(m);
    }
}

/* x4 asked for where it does not fit: x4 at 40 fps on 144 Hz is 160 a second, x3 runs (and fits evenly);
 * x4 at 60 fps on 120 Hz runs as x2; x4 with 7 ms of GPU time a generated frame runs as x3. */
static void sim_xn_downgrades(void) {
    struct sim *m = sim_xn(4, 6944444, 40.0, 144.0f);
    CHECK(m->mult_lo == 3 && m->mult_hi == 3, "x4 at 40 fps on 144: x3, got %d .. %d", m->mult_lo, m->mult_hi);
    CHECK(m->n_of[1] == 3, "x3 from the first frames on, got x%d", m->n_of[1]);
    CHECK(m->b_lost_after_warmup == 0 && m->order_bad == 0, "x4 at 40 on 144: %d lost, %d too close",
          m->b_lost_after_warmup, m->order_bad);
    CHECK(m->s_shown >= m->s_made * 98 / 100, "x4 at 40 on 144: %d of %d shown", m->s_shown, m->s_made);
    free(m);
    m = sim_xn(4, 8333333, 60.0, 120.0f);
    CHECK(m->mult_lo == 2 && m->mult_hi == 2, "x4 at 60 fps on 120: x2, got %d .. %d", m->mult_lo, m->mult_hi);
    CHECK(m->b_lost_after_warmup == 0 && m->order_bad == 0, "x4 at 60 on 120: %d lost, %d too close",
          m->b_lost_after_warmup, m->order_bad);
    free(m);
    m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 6 * MS, 1 * MS);
    m->mult = 4;
    m->gpu_first = m->gpu_phase = 7.0;
    m->peak = 144.0f;
    g_p1 = 33333333;
    sim_run(m);
    CHECK(m->mult_lo == 3 && m->mult_hi == 3, "x4, 7 ms a generated frame: x3, got %d .. %d", m->mult_lo, m->mult_hi);
    CHECK(m->b_lost_after_warmup == 0, "x4 -> x3 for the GPU: %d lost", m->b_lost_after_warmup);
    free(m);
}

/* x4 at 30 fps with the panel held at 60 Hz below a peak of 144: after the grace x2, which fits; no real frame
 * lost meanwhile. */
static void sim_xn_panel_below_the_peak(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 16666666, 6 * MS, 1 * MS);
    m->mult = 4;
    m->peak = 144.0f;
    g_p1 = 33333333;
    sim_run(m);
    const int grace = (int)(ZFG_FAST_GRACE_NS / 33333333) + 2;
    int x2 = 0;
    for (int f = grace; f < SIM_FRAMES; f++) x2 += m->n_of[f] == 2;
    CHECK(m->n_of[1] == 4 && x2 == SIM_FRAMES - grace, "x4 on a panel held at 60: x4 first, x2 from frame %d on (%d of %d)",
          grace, x2, SIM_FRAMES - grace);
    CHECK(m->b_lost_after_warmup == 0 && m->order_bad == 0, "x4 on 60: %d lost, %d too close", m->b_lost_after_warmup,
          m->order_bad);
    double s, b;
    hold_means(m, grace + 10, SIM_FRAMES - 2, &s, &b);
    CHECK(s > 0.9 && b > 0.9, "then x2 on 60: S held %.2f, B held %.2f refreshes", s, b);
    free(m);
}

/* x3 at 31.5 .. 33 fps on 90 Hz (94.5 .. 99 outputs a second, up to 10 % above the refresh rate): x3 stays, every
 * output one refresh, the first generated frame of some pairs left out; no real frame lost. */
static void sim_x3_a_little_above_90(void) {
    const double fps[] = {31.5, 32.0, 32.5, 33.0};
    for (unsigned i = 0; i < sizeof(fps) / sizeof(fps[0]); i++) {
        struct sim *m = sim_xn(3, 11111111, fps[i], 144.0f);
        int one = 0, outs = 0;
        for (int f = 20; f < SIM_FRAMES - 2; f++) {
            for (int k = 0; k < 2; k++)
                if (m->s_hold[f][k]) {
                    outs++;
                    one += m->s_hold[f][k] == 1;
                }
            if (m->b_hold[f]) {
                outs++;
                one += m->b_hold[f] == 1;
            }
        }
        const double secs = (SIM_FRAMES - 22) / fps[i];
        CHECK(m->mult_lo == 3 && m->mult_hi == 3, "x3 at %.1f fps on 90: the multiplier %d .. %d", fps[i], m->mult_lo,
              m->mult_hi);
        CHECK(m->b_lost_after_warmup == 0 && m->order_bad == 0 && m->spacing_bad == 0 && m->bare_after_warmup == 0,
              "x3 at %.1f fps on 90: %d lost, %d too close, %d off the spacing, %d without generated frames", fps[i],
              m->b_lost_after_warmup, m->order_bad, m->spacing_bad, m->bare_after_warmup);
        CHECK(one >= outs - 2, "x3 at %.1f fps on 90: %d of %d outputs held one refresh", fps[i], one, outs);
        CHECK(outs >= secs * 90 * 0.97, "x3 at %.1f fps on 90: %.1f outputs a second", fps[i], outs / secs);
        free(m);
    }
}

/* ---- the vote, the multiplier and the panel together ----
 *
 * Frames come at a real rate that wanders or steps, each interval up to 10 % off its mean. The vote, the multiplier
 * and fast go as zerofg_zc.c runs them; the session samples the vote once a second and the
 * refresh lift (RefreshRateLiftLogic.due) puts its mode in place once it was wanted for 2 s without a break, at
 * least 5 s after the lift's last write; no vote: the top rate. */
struct loop_case {
    int mult;
    const float *modes;
    int nm;
    float peak;
    double (*fps_at)(double secs, const struct loop_case *c);
    double lo, hi, wander_s, phase; /* the real rate between lo and hi, a cycle (or a step) every wander_s */
    double x2_gain;                 /* the real rate this much higher while x2 runs (its GPU time is shorter) */
    int spikes;                     /* every 23rd interval 1.7 times as long */
    double jitter;                  /* each interval up to this share off its mean (0: 10 %) */
    double count_from;              /* seconds: changes before are not counted */
    double secs;
};
struct loop_out {
    int vote_changes, panel_changes, mult_lo, mult_hi;
    float first_vote, last_vote, vote_from; /* vote_from: at count_from */
    int panel, first_panel;
    double vote_changed_at, panel_changed_at; /* seconds, the first change after the first vote; -1: none */
};

static double fps_sine(double secs, const struct loop_case *c) {
    const double mid = (c->lo + c->hi) / 2, half = (c->hi - c->lo) / 2, w = 6.283185307179586 / c->wander_s;
    return mid + half * (0.7 * sin(w * secs + c->phase) + 0.3 * sin(2.3 * w * secs + 1.0));
}

static uint32_t walk_state;
static double walk_fps;
static double fps_walk(double secs, const struct loop_case *c) {
    (void)secs;
    walk_state = walk_state * 1664525u + 1013904223u;
    const double u = (double)(walk_state >> 8) / 16777216.0 - 0.5;
    if (walk_fps <= 0.0) walk_fps = c->lo + (c->hi - c->lo) * c->phase;
    walk_fps += u * (c->hi - c->lo) / c->wander_s * 0.4;
    if (walk_fps < c->lo) walk_fps = 2 * c->lo - walk_fps;
    if (walk_fps > c->hi) walk_fps = 2 * c->hi - walk_fps;
    return walk_fps;
}

static double fps_step(double secs, const struct loop_case *c) { return secs < c->wander_s ? c->lo : c->hi; }

static int mode_of(float hz, const float *modes, int nm, float peak) {
    int top = 0;
    for (int i = 0; i < nm; i++)
        if (modes[i] <= peak + 0.5f && (int)(modes[i] + 0.5f) > top) top = (int)(modes[i] + 0.5f);
    return hz > 1.0f ? (int)(hz + 0.5f) : top;
}

static void loop_run(const struct loop_case *c, struct loop_out *o) {
    struct zfg_period per;
    zfg_period_reset(&per);
    struct zfg_vote v;
    zfg_vote_reset(&v);
    struct zfg_fast fast = {0, 0};
    struct zfg_mult mult;
    zfg_mult_reset(&mult);
    memset(o, 0, sizeof(*o));
    o->mult_lo = ZFG_MULT_MAX + 1;
    o->vote_changed_at = o->panel_changed_at = -1.0;
    walk_fps = 0.0;
    float vote = 0.0f;
    int want = mode_of(0.0f, c->modes, c->nm, c->peak), wanted = want, panel = want;
    int64_t wanted_since = 0, written_at = 0, sampled = 0;
    const int64_t t0 = 1000 * MS;
    int64_t t = t0;
    for (int n = 0; t - t0 < (int64_t)(c->secs * 1e9); n++) {
        const double secs = (double)(t - t0) / 1e9;
        double fps = c->fps_at(secs, c);
        if (mult.n == 2) fps *= 1.0 + c->x2_gain;
        jit_state = jit_state * 1664525u + 1013904223u;
        const double j = c->jitter > 0.0 ? c->jitter : 0.1;
        double iv = 1e9 / fps * (1.0 - j + 2.0 * j * ((double)(jit_state >> 8) / 16777216.0));
        if (c->spikes && n % 23 == 22) iv *= 1.7;
        t += (int64_t)iv;
        if (!zfg_is_gap((int64_t)iv, zfg_period_get(&per))) zfg_period_add(&per, (int64_t)iv);
        const int64_t period = zfg_period_get(&per);
        zfg_fast_update(&fast, t, period, (float)panel, c->peak);
        const struct zfg_mult_in in = {.want = c->mult, .period = period, .refresh_hz = (float)panel,
                                       .peak_hz = c->peak};
        const int nm = zfg_mult_update(&mult, t, &in);
        if (t - t0 > 2000 * MS) {
            if (nm < o->mult_lo) o->mult_lo = nm;
            if (nm > o->mult_hi) o->mult_hi = nm;
        }
        const float before = vote;
        const int counted = secs >= c->count_from;
        if (!counted) o->vote_from = vote;
        zfg_vote_layer(&v, 0);
        const float hz = zfg_vote_step_n(&v, t, period, mult.vote_n ? mult.vote_n : 2, fast.on || nm < mult.vote_n,
                                         c->modes, c->nm, c->peak);
        if (hz > 0.0f) vote = hz;
        if (vote != before) {
            if (o->first_vote > 0.0f) {
                if (counted && o->vote_changed_at < 0) o->vote_changed_at = (double)(t - t0) / 1e9;
                o->vote_changes += counted;
            } else {
                o->first_vote = vote;
                /* the panel as the lift would have put it by then: the changes after it count */
                panel = wanted = want = mode_of(vote, c->modes, c->nm, c->peak);
                o->first_panel = panel;
                written_at = wanted_since = t;
            }
        }
        if (t - sampled >= 1000 * MS) {
            sampled = t;
            want = mode_of(vote, c->modes, c->nm, c->peak);
        }
        if (want != wanted) {
            wanted = want;
            wanted_since = t;
        }
        if (wanted != panel && t - wanted_since >= 2000 * MS && t - written_at >= 5000 * MS) {
            panel = wanted;
            written_at = t;
            if (o->first_vote > 0.0f && counted) {
                if (o->panel_changed_at < 0) o->panel_changed_at = (double)(t - t0) / 1e9;
                o->panel_changes++;
            }
        }
    }
    o->last_vote = vote;
    o->panel = panel;
}

/* x3 on 60/90/120/144 at a real rate that wanders over 30.5 .. 32.5 fps for 2 minutes (91.5 .. 97.5 outputs, at
 * 90's band edges), slowly or quickly, starting low or high, with a 1.7 times longer frame now and then, with x2's
 * shorter GPU time raising the real rate: at most one switch of the vote and of the panel, and none once at 90. */
static void loop_x3_near_90(void) {
    const float m[] = {60, 90, 120, 144};
    const double waves[] = {4.0, 9.0, 20.0, 45.0, 120.0};
    const double phases[] = {-1.5708, 0.0, 1.5708, 3.1416};
    int cases = 0, at90 = 0;
    for (int shape = 0; shape < 2; shape++)
        for (unsigned w = 0; w < sizeof(waves) / sizeof(waves[0]); w++)
            for (unsigned ph = 0; ph < sizeof(phases) / sizeof(phases[0]); ph++)
                for (int variant = 0; variant < 4; variant++) {
                    struct loop_case c = {.mult = 3, .modes = m, .nm = 4, .peak = 144.0f, .lo = 30.5, .hi = 32.5,
                                          .wander_s = waves[w], .phase = phases[ph], .secs = 120.0};
                    c.fps_at = shape ? fps_walk : fps_sine;
                    if (shape) c.phase = (ph + 0.5) / 4.0;
                    c.spikes = variant & 1;
                    c.x2_gain = variant & 2 ? 0.07 : 0.0;
                    jit_state = 101u + 7u * w + 31u * ph + 977u * (unsigned)variant;
                    walk_state = 5u + w + 13u * ph;
                    struct loop_out o;
                    loop_run(&c, &o);
                    cases++;
                    at90 += o.last_vote == 90.0f;
                    CHECK(o.vote_changes <= 1 && o.panel_changes <= 1,
                          "x3 at 30.5 .. 32.5 fps (%s, %.0f s, phase %u, spikes %d, x2 gain %.2f): the vote %.0f -> "
                          "%.0f, %d changes, the panel %d changes (first at %.1f s)", shape ? "walk" : "sine",
                          waves[w], ph, c.spikes, c.x2_gain, (double)o.first_vote, (double)o.last_vote,
                          o.vote_changes, o.panel_changes, o.panel_changed_at);
                    CHECK(!(o.first_vote == 90.0f && o.vote_changes), "x3 (%s, %.0f s, phase %u): 90 first, then %.0f",
                          shape ? "walk" : "sine", waves[w], ph, (double)o.last_vote);
                    CHECK(o.mult_lo == 3 && o.mult_hi == 3, "x3 (%s, %.0f s, phase %u): the multiplier %d .. %d",
                          shape ? "walk" : "sine", waves[w], ph, o.mult_lo, o.mult_hi);
                }
    CHECK(at90 >= cases / 2, "x3 at 30.5 .. 32.5 fps: 90 at the end in %d of %d runs", at90, cases);
}

/* Other multipliers and mode sets near a band edge: x2 about 60 and 90, x4 about 120, x3 on the tablet's modes, x2
 * and x3 with the peak at 120: at most one switch of the vote and of the panel. */
static void loop_near_band_edges(void) {
    const float m4[] = {60, 90, 120, 144};
    const float m7[] = {30, 48, 50, 60, 90, 120, 144};
    const struct { int mult; const float *modes; int nm; float peak; double lo, hi; } cases[] = {
        {2, m4, 4, 144.0f, 29.5, 31.5}, {2, m4, 4, 144.0f, 44.5, 47.5}, {4, m4, 4, 144.0f, 29.5, 31.5},
        {3, m7, 7, 144.0f, 30.5, 32.5}, {2, m7, 7, 120.0f, 29.5, 31.5}, {3, m7, 7, 120.0f, 38.5, 40.5},
        {2, m7, 7, 144.0f, 24.0, 26.0}, {4, m7, 7, 144.0f, 35.0, 37.0}};
    const double waves[] = {5.0, 20.0, 60.0};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (unsigned w = 0; w < sizeof(waves) / sizeof(waves[0]); w++)
            for (int ph = 0; ph < 4; ph++) {
                struct loop_case c = {.mult = cases[i].mult, .modes = cases[i].modes, .nm = cases[i].nm,
                                      .peak = cases[i].peak, .fps_at = fps_sine, .lo = cases[i].lo, .hi = cases[i].hi,
                                      .wander_s = waves[w], .phase = ph * 1.5708, .spikes = ph & 1, .secs = 120.0};
                jit_state = 3u + 11u * i + 101u * w + 1009u * (unsigned)ph;
                struct loop_out o;
                loop_run(&c, &o);
                CHECK(o.vote_changes <= 1 && o.panel_changes <= 1,
                      "x%d at %.1f .. %.1f fps, peak %.0f, %u modes (%.0f s, phase %d): the vote %.0f -> %.0f, %d "
                      "changes, the panel %d", c.mult, c.lo, c.hi, (double)c.peak, c.nm, waves[w], ph,
                      (double)o.first_vote, (double)o.last_vote, o.vote_changes, o.panel_changes);
            }
}

/* A real change of rate still moves the vote and the panel, once. Far above the vote (x3 from 30 to 45 fps: 90 to 135
 * outputs) within about a second, the panel within the lift's 2 s after it; just out of the band (x3 at 34.5 fps, 103.5
 * outputs, 15 % above 90) after the long hold; further (35.5 fps, 18 % above) once the multiplier went down to x2 for
 * the running rate after its grace; below the vote with uneven refreshes (x3 from 45 to 30 on 144: 1.6 refreshes an
 * output; x2 from 60 to 45 on 120: 1.33) within about a second; where a lower rate fits and the vote is still even (x2
 * from 60 to 30 on 120: two refreshes an output) after the long hold, less the uneven stretch on the way. x2 (fast until the panel is up) and x4 likewise. */
static void loop_steps(void) {
    const float m[] = {60, 90, 120, 144}, m7[] = {30, 48, 50, 60, 90, 120, 144};
    const struct {
        int mult;
        double lo, hi;
        float from, to;
        double vote_min, vote_max, panel_max;
        int all;
    } cases[] = {
        {3, 30.0, 45.0, 90.0f, 144.0f, 0.5, 1.5, 4.5, 0},  {3, 30.0, 34.5, 90.0f, 144.0f, 9.5, 15.0, 18.0, 0},
        {3, 30.0, 35.5, 90.0f, 144.0f, 2.0, 7.0, 10.0, 0},  {3, 45.0, 30.0, 144.0f, 90.0f, 0.5, 2.5, 5.5, 0},
        {2, 30.0, 45.0, 60.0f, 90.0f, 0.5, 1.5, 4.5, 0},   {2, 45.0, 30.0, 90.0f, 60.0f, 0.5, 2.5, 5.5, 0},
        {4, 30.0, 36.0, 120.0f, 144.0f, 1.0, 4.5, 7.5, 0}, {2, 60.0, 45.0, 120.0f, 90.0f, 0.5, 2.5, 5.5, 1},
        {3, 40.0, 30.0, 120.0f, 90.0f, 0.5, 2.5, 5.5, 1},  {3, 30.0, 20.0, 90.0f, 60.0f, 0.5, 2.5, 5.5, 1},
        {2, 60.0, 24.0, 120.0f, 48.0f, 0.5, 2.5, 5.5, 1},  {4, 30.0, 22.5, 120.0f, 90.0f, 0.5, 2.5, 5.5, 1},
        {2, 60.0, 30.0, 120.0f, 60.0f, 8.0, 11.5, 14.5, 1}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (int spikes = 0; spikes < 2; spikes++) {
            const double step = 30.0;
            const float *mm = cases[i].all ? m7 : m;
            const int nm = cases[i].all ? 7 : 4;
            struct loop_case c = {.mult = cases[i].mult, .modes = mm, .nm = nm, .peak = 144.0f, .fps_at = fps_step,
                                  .lo = cases[i].lo, .hi = cases[i].hi, .wander_s = step, .spikes = spikes,
                                  .count_from = step, .secs = 60.0};
            jit_state = 17u + 3u * i + (unsigned)spikes;
            struct loop_out o;
            loop_run(&c, &o);
            const double dv = o.vote_changed_at - step, dp = o.panel_changed_at - step;
            CHECK(o.vote_from == cases[i].from && o.last_vote == cases[i].to && o.vote_changes == 1 &&
                  o.panel_changes == 1 && o.panel == mode_of(cases[i].to, mm, nm, 144.0f),
                  "x%d %.1f -> %.1f fps, %d modes (spikes %d): the vote %.0f -> %.0f (%d changes), the panel %d (%d "
                  "changes)", c.mult, c.lo, c.hi, nm, spikes, (double)o.vote_from, (double)o.last_vote, o.vote_changes, o.panel,
                  o.panel_changes);
            CHECK(dv >= cases[i].vote_min && dv <= cases[i].vote_max && dp > dv && dp <= cases[i].panel_max,
                  "x%d %.1f -> %.1f fps, %d modes (spikes %d): the vote moved %.1f s, the panel %.1f s after the step",
                  c.mult, c.lo, c.hi, nm, spikes, dv, dp);
        }
}

/* g2-060's zfg_schedule, before x3 and x4. */
static struct zfg_slots old_x2_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                                        int64_t refresh) {
    const int64_t half = period > 0 ? period / 2 : 0;
    const int64_t gap = refresh > 0 ? refresh : 0;
    const int64_t step = half > gap ? half : gap;
    const int64_t lo = arrival + (lead > 0 ? lead : 0), hi = lo + (slack > 0 ? slack : 0);
    int64_t s = prev_b ? prev_b + half : lo;
    if (s < lo) s = lo;
    if (s > hi) s = hi;
    struct zfg_slots r = {s, s + step, 0, 2, 0, 0};
    if (prev_b && s < prev_b + gap) {
        if (prev_b + gap <= hi) {
            r.s = prev_b + gap;
            r.b = r.s + step;
        } else {
            r.no_s = 1;
            r.s = 0;
            r.b = lo + half > prev_b + gap ? lo + half : prev_b + gap;
            if (r.b > hi + step) r.b = lo + half;
        }
    }
    return r;
}

/* x2 places its outputs as it did before x3 and x4 (one refresh between outputs at least): for any arrival
 * against the previous b, at 20 .. 62 fps on 60 .. 144 Hz. */
static void schedule_x2_as_before(void) {
    const int64_t refreshes[] = {16666666, 11111111, 8333333, 6944444, 0};
    const int64_t periods[] = {50 * MS, 33333333, 32258064, 25 * MS, 16666666};
    int bad = 0, cases = 0;
    for (unsigned r = 0; r < sizeof(refreshes) / sizeof(refreshes[0]); r++)
        for (unsigned q = 0; q < sizeof(periods) / sizeof(periods[0]); q++)
            for (int64_t lead = 10 * MS; lead <= 60 * MS; lead += 25 * MS)
                for (int64_t off = -40 * MS; off <= 60 * MS; off += MS / 2) {
                    const int64_t p = periods[q], arrival = 1000 * MS, prev_b = off ? arrival + off : 0;
                    const int64_t slack = zfg_slack(p, refreshes[r]);
                    const struct zfg_slots a = zfg_schedule(arrival, prev_b, p, lead, slack, refreshes[r]);
                    const struct zfg_slots b = old_x2_schedule(arrival, prev_b, p, lead, slack, refreshes[r]);
                    cases++;
                    if (a.no_s != b.no_s || a.b != b.b || (!a.no_s && zfg_slot_s(&a, 0) != b.s)) {
                        if (!bad) fprintf(stderr, "x2: period %lld refresh %lld lead %lld prev_b %+lld: s %lld/%lld "
                                                  "b %lld/%lld\n", (long long)p, (long long)refreshes[r],
                                          (long long)lead, (long long)off, (long long)zfg_slot_s(&a, 0),
                                          (long long)b.s, (long long)a.b, (long long)b.b);
                        bad++;
                    }
                }
    CHECK(!bad, "x2: %d of %d schedules differ from g2-060's", bad, cases);
}

static void slots_drop_first(void) {
    const int64_t p = 33333333, r = 8333333;
    struct zfg_slots t = zfg_schedule_n(0, 0, p, 20 * MS, r, r, 4);
    const int64_t s1 = zfg_slot_s(&t, 1), s2 = zfg_slot_s(&t, 2), b = t.b;
    CHECK(zfg_slots_shown(&t) == 3, "x4: three");
    zfg_slots_drop_first(&t, 1);
    CHECK(zfg_slots_shown(&t) == 2 && t.first == 1 && zfg_slot_s(&t, 0) == 0 && zfg_slot_s(&t, 1) == s1 &&
          zfg_slot_s(&t, 2) == s2 && t.b == b, "one fewer buffer: the first left out, the others where they were");
    zfg_slots_drop_first(&t, 2);
    CHECK(t.no_s && zfg_slots_shown(&t) == 0 && t.b == b, "none left: B alone, where it was");
    CHECK(zfg_slot_s(&t, 3) == 0, "past the last phase: none");
}

/* The output times pend_push gives a pair, and what pend_run does with each generated frame. */
static void pair_times_and_actions(void) {
    const int64_t r = 8333333, latch = r, margin = 2 * MS, p = 33333333;
    struct zfg_slots t = zfg_schedule_n(0, 0, p, 20 * MS, r, r, 3);
    struct zfg_pair_times pt;
    zfg_pair_times(&t, r, latch, margin, &pt);
    CHECK(pt.ns == 2, "x3: two generated frames");
    for (int k = 0; k < 2; k++) {
        CHECK(pt.s_desired[k] == zfg_slot_s(&t, k) - r / 2 && pt.s_apply[k] == pt.s_desired[k] - latch - margin,
              "x3: frame %d's desired present and apply times", k);
        const int64_t next = k ? pt.b_apply : pt.s_apply[1];
        CHECK(pt.s_last[k] == next + margin - r, "x3: frame %d goes until a refresh before the next output's commit", k);
    }
    CHECK(pt.b_desired == t.b - r / 2 && pt.b_apply == pt.b_desired - latch - margin, "x3: B's times");
    /* outputs a refresh apart: the last moment is never before the apply time + 1 ms */
    struct zfg_slots u = zfg_schedule_n(0, 0, 25 * MS, 20 * MS, r, r, 3);
    zfg_pair_times(&u, r, latch, margin, &pt);
    CHECK(pt.s_last[0] >= pt.s_apply[0] + MS, "a refresh apart: at least 1 ms after the apply time");
    zfg_slots_drop_first(&t, 1);
    zfg_pair_times(&t, r, latch, margin, &pt);
    CHECK(pt.ns == 1 && pt.s_desired[0] == zfg_slot_s(&t, 1) - r / 2, "the first left out: the second's times");
    CHECK(zfg_s_action(1, 10, 10, 20, 0) == ZFG_S_SHOW && zfg_s_action(1, 9, 10, 20, 0) == ZFG_S_WAIT &&
          zfg_s_action(0, 15, 10, 20, 0) == ZFG_S_WAIT && zfg_s_action(0, 20, 10, 20, 0) == ZFG_S_DROP &&
          zfg_s_action(1, 20, 10, 20, 0) == ZFG_S_DROP, "ready at its apply time: shown; late: left out");
    CHECK(zfg_s_action(1, 5, 10, 20, 1) == ZFG_S_SHOW && zfg_s_action(0, 5, 10, 20, 1) == ZFG_S_DROP,
          "forced: shown if complete, else left out");
}

/* A multiplier lowered for the GPU time comes back once the GPU time is back: ZeroFG 1.2 records the pair whole
 * once (F) and only the picture at every further phase (p), so x3 costs F + p and x4 F + 2 p. */
static int mg(struct zfg_mult *m, int64_t t, int want, double first, double phase) {
    const struct zfg_mult_in in = {.want = want, .period = P30, .refresh_hz = 120.0f, .peak_hz = 144.0f,
                                   .gpu_first_ms = first, .gpu_phase_ms = phase};
    return zfg_mult_update(m, t, &in);
}

static void multiplier_gpu_model(void) {
    CHECK(zfg_pair_gpu_ms(2, 9.0, 2.0) == 9.0 && zfg_pair_gpu_ms(3, 9.0, 2.0) == 11.0 &&
          zfg_pair_gpu_ms(4, 9.0, 2.0) == 13.0 && zfg_pair_gpu_ms(4, 9.0, 0) == 9.0, "pair GPU time: F + (N - 2) p");
    /* 30 fps, F 9 ms, p 2 ms: x4 13 ms of the 16.7 ms budget */
    struct zfg_mult m;
    zfg_mult_reset(&m);
    int64_t t = 1000 * MS;
    CHECK(mg(&m, t, 4, 9.0, 2.0) == 4 && m.why == ZFG_CAP_NONE, "x4: F 9 ms, p 2 ms fits");
    /* a 3 s spike (F 14, p 4): x2 */
    int n = 0;
    const int64_t spike = t;
    for (; t < spike + 3000 * MS; t += P30) n = mg(&m, t, 4, 14.0, 4.0);
    CHECK(n == 2 && m.why == ZFG_CAP_GPU, "the spike: x2 (got x%d, %s)", n, zfg_cap_name(m.why));
    /* back to F 9; at x2 no further calls are measured, p stays at the spike's 4 ms until x3 runs again */
    int64_t x3_at = 0, x4_at = 0;
    double phase = 4.0;
    for (; t < spike + 30000 * MS && !x4_at; t += P30) {
        n = mg(&m, t, 4, 9.0, phase);
        if (n >= 3 && !x3_at) x3_at = t - spike;
        if (n == 3) phase = 2.0;
        if (n == 4) x4_at = t - spike;
    }
    CHECK(x3_at && x3_at <= ZFG_GPU_BLOCK_NS + ZFG_MULT_RAISE_NS + 100 * MS, "after the spike: x3 %lld ms after it "
          "began", (long long)(x3_at / MS));
    CHECK(x4_at && x4_at <= x3_at + ZFG_MULT_RAISE_NS + 100 * MS, "then x4 %lld ms after it began",
          (long long)(x4_at / MS));
    /* x2 asked for, then x4 from the menu: x2's samples (F) alone do not hold x4 back */
    zfg_mult_reset(&m);
    t = 1000 * MS;
    CHECK(mg(&m, t, 2, 9.0, 0) == 2, "x2");
    CHECK(mg(&m, t + P30, 4, 9.0, 0) == 4 && m.why == ZFG_CAP_NONE, "x2 -> x4 from the menu, p not measured: x4");
    CHECK(mg(&m, t + 2 * P30, 4, 9.0, 2.0) == 4, "p 2 ms measured: x4 stays");
    CHECK(mg(&m, t + 3 * P30, 4, 9.0, 6.0) == 3 && m.why == ZFG_CAP_GPU, "p 6 ms (x4 21 ms): x3 (15 ms)");
    /* F alone over the budget: x2 */
    zfg_mult_reset(&m);
    CHECK(mg(&m, t, 4, 17.0, 0) == 2 && m.why == ZFG_CAP_GPU, "F 17 ms: x2");
}

static int mf(struct zfg_mult *m, int64_t t, int want, int64_t ready) {
    const struct zfg_mult_in in = {.want = want, .period = P30, .refresh_hz = 120.0f, .peak_hz = 144.0f,
                                   .ready_ns = ready};
    return zfg_mult_update(m, t, &in);
}

/* Generations done long after their real frame arrived keep several pairs in flight: x4's three calls a pair fit
 * the 8 frame contexts for two pairs, x3's two for three (ZFG_JOBS). */
static void multiplier_flight(void) {
    struct zfg_mult m;
    zfg_mult_reset(&m);
    int64_t t = 1000 * MS;
    CHECK(mf(&m, t, 4, 50 * MS) == 4 && m.why == ZFG_CAP_NONE, "x4, done 50 ms after arrival at 30 fps: two pairs");
    CHECK(mf(&m, t + P30, 4, 70 * MS) == 3 && m.why == ZFG_CAP_LATE && m.vote_n == 3, "70 ms: three pairs, x3 (%s)",
          zfg_cap_name(m.why));
    t += 2 * P30;
    const int64_t t0 = t;
    int64_t up = 0;
    for (; t < t0 + 4000 * MS && !up; t += P30)
        if (mf(&m, t, 4, 50 * MS) == 4) up = t - t0;
    CHECK(up >= ZFG_MULT_RAISE_NS && up <= ZFG_MULT_RAISE_NS + 50 * MS, "back to 50 ms: x4 %lld ms later",
          (long long)(up / MS));
    zfg_mult_reset(&m);
    CHECK(mf(&m, t, 4, 60 * MS) == 4, "60 ms: x4 kept");
    zfg_mult_reset(&m);
    CHECK(mf(&m, t, 4, 59 * MS) == 4 && mf(&m, t + P30, 4, 62 * MS) == 4, "59 then 62 ms: x4 kept");
    zfg_mult_reset(&m);
    CHECK(mf(&m, t, 3, 90 * MS) == 3, "x3 at 90 ms: three pairs of two calls fit");
    CHECK(mf(&m, t + P30, 3, 110 * MS) == 2 && m.why == ZFG_CAP_LATE, "110 ms: four pairs, more than the jobs: x2");
    CHECK(mf(&m, t + 2 * P30, 3, 0) == 2, "unknown: as it was");
    CHECK(!strcmp(zfg_cap_name(ZFG_CAP_LATE), "late") && !strcmp(zfg_cap_name(ZFG_CAP_ENGINE), "engine"), "names");
}

static void ready_median(void) {
    struct zfg_ready r;
    zfg_ready_reset(&r);
    for (int i = 0; i < 7; i++) zfg_ready_add(&r, 50 * MS);
    CHECK(zfg_ready_get(&r) == 0, "fewer than 8: unknown");
    zfg_ready_add(&r, 0);
    CHECK(zfg_ready_get(&r) == 0, "0 is no measurement");
    zfg_ready_add(&r, 90 * MS);
    CHECK(zfg_ready_get(&r) == 50 * MS, "one slow pair of 8: the median stays, got %lld", (long long)zfg_ready_get(&r));
    for (int i = 0; i < ZFG_READY_HIST; i++) zfg_ready_add(&r, 70 * MS);
    CHECK(zfg_ready_get(&r) == 70 * MS, "the last %d: 70 ms", ZFG_READY_HIST);
}

/* An engine whose phase forms failed records x2 only. */
static void multiplier_engine_limit(void) {
    struct zfg_mult m;
    zfg_mult_reset(&m);
    struct zfg_mult_in in = {.want = 4, .limit = 2, .period = P30, .refresh_hz = 120.0f, .peak_hz = 144.0f};
    CHECK(zfg_mult_update(&m, 1000 * MS, &in) == 2 && m.why == ZFG_CAP_ENGINE && m.vote_n == 2, "limit 2: x2 (%s)",
          zfg_cap_name(m.why));
    in.limit = 3;
    CHECK(zfg_mult_update(&m, 1000 * MS + P30, &in) == 2, "limit 3: x3 comes back only after the raise time");
    int n = 0;
    for (int64_t t = 1000 * MS + 2 * P30; t < 4000 * MS; t += P30) n = zfg_mult_update(&m, t, &in);
    CHECK(n == 3 && m.why == ZFG_CAP_ENGINE, "limit 3: x3 (%s)", zfg_cap_name(m.why));
    in.limit = 0;
    in.period = 0;
    zfg_mult_reset(&m);
    CHECK(zfg_mult_update(&m, 1000 * MS, &in) == 4, "no limit: x4");
    in.limit = 2;
    CHECK(zfg_mult_update(&m, 1000 * MS + P30, &in) == 2 && m.why == ZFG_CAP_ENGINE, "period unknown, limit 2: x2");
}

/* x4 at 30 fps on 120 Hz and x3 on 90 Hz with uneven arrivals: one output a refresh, so a real frame that comes
 * early against the rhythm has no room before it; only the first generated frame of such a pair is left out. */
static void sim_xn_jitter(void) {
    const struct { int mult; int64_t refresh; const char *what; } cases[] = {
        {4, 8333333, "x4 on 120"}, {3, 11111111, "x3 on 90"}};
    const int64_t jitters[] = {1 * MS, 3 * MS, 5 * MS};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (unsigned j = 0; j < sizeof(jitters) / sizeof(jitters[0]); j++) {
            struct sim *m = calloc(1, sizeof(*m));
            sim_init(m, cases[i].refresh, 6 * MS, jitters[j]);
            m->gen_more = 2 * MS;
            m->mult = cases[i].mult;
            m->peak = 144.0f;
            g_p1 = 33333333;
            sim_run(m);
            const int want = (SIM_FRAMES - 20) * (cases[i].mult - 1);
            CHECK(m->bare_after_warmup <= 2 && m->s_early <= 2, "%s, +-%lld ms: %d pairs shown without generated frames "
                  "(%d early)", cases[i].what, (long long)(jitters[j] / MS), m->bare_after_warmup, m->s_early);
            CHECK(m->b_lost_after_warmup == 0 && m->order_bad == 0, "%s, +-%lld ms: %d lost, %d too close",
                  cases[i].what, (long long)(jitters[j] / MS), m->b_lost_after_warmup, m->order_bad);
            CHECK(m->s_shown >= want * 95 / 100, "%s, +-%lld ms: %d of %d generated frames shown (%d left out as "
                  "early)", cases[i].what, (long long)(jitters[j] / MS), m->s_shown, want, m->s_early_ph);
            free(m);
        }
}

/* x2 at 30 fps on 120 Hz with uneven arrivals: as many generated frames as before x3 and x4. */
static void sim_x2_jitter(void) {
    const int64_t jitters[] = {3 * MS, 5 * MS, 8 * MS};
    for (unsigned j = 0; j < sizeof(jitters) / sizeof(jitters[0]); j++) {
        struct sim *m = calloc(1, sizeof(*m));
        sim_init(m, 8333333, 6 * MS, jitters[j]);
        m->peak = 144.0f;
        g_p1 = 33333333;
        sim_run(m);
        CHECK(m->s_early <= 4, "x2 on 120, +-%lld ms: %d pairs without S (early)", (long long)(jitters[j] / MS),
              m->s_early);
        CHECK(m->b_lost_after_warmup == 0, "x2 on 120, +-%lld ms: %d lost", (long long)(jitters[j] / MS),
              m->b_lost_after_warmup);
        free(m);
    }
}

/* x4 at 30 fps where B is rendered 70 ms after it arrived: three pairs in flight need 9 frame contexts, so x3 runs
 * (6 calls in three jobs) instead of every third pair being skipped as late. */
static void sim_xn_slow_render(void) {
    struct sim *m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 6 * MS, 1 * MS);
    m->gen_more = 2 * MS;
    m->render_ns = 70 * MS;
    m->mult = 4;
    m->peak = 144.0f;
    g_p1 = 33333333;
    sim_run(m);
    CHECK(m->mult_lo == 3 && m->mult_hi == 3, "x4, B rendered after 70 ms: x3, got %d .. %d", m->mult_lo, m->mult_hi);
    /* late only until ZFG_READY_HIST / 2 generations are measured */
    CHECK(m->s_late <= ZFG_READY_HIST / 2, "x4, B rendered after 70 ms: %d pairs late", m->s_late);
    CHECK(m->s_shown >= (SIM_FRAMES - 20) * 2 * 97 / 100 && m->b_lost_after_warmup == 0,
          "x4, B rendered after 70 ms: %d generated frames shown, %d lost", m->s_shown, m->b_lost_after_warmup);
    free(m);
    m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 6 * MS, 1 * MS);
    m->gen_more = 2 * MS;
    m->render_ns = 50 * MS;
    m->mult = 4;
    m->peak = 144.0f;
    sim_run(m);
    CHECK(m->mult_lo == 4 && m->mult_hi == 4 && m->s_late <= 3, "x4, B rendered after 50 ms: x4 (%d .. %d), %d late",
          m->mult_lo, m->mult_hi, m->s_late);
    free(m);
    /* every tenth B after 75 ms: x4 stays (the median), each slow one costs at most the pair after it */
    m = calloc(1, sizeof(*m));
    sim_init(m, 8333333, 6 * MS, 1 * MS);
    m->gen_more = 2 * MS;
    m->render_ns = 50 * MS;
    m->render_slow_every = 10;
    m->render_slow_ns = 75 * MS;
    m->mult = 4;
    m->peak = 144.0f;
    sim_run(m);
    CHECK(m->mult_lo == 4 && m->mult_hi == 4, "x4, every tenth B after 75 ms: x4, got %d .. %d", m->mult_lo, m->mult_hi);
    CHECK(m->s_late <= SIM_FRAMES / 10 + 2, "x4, every tenth B after 75 ms: %d late", m->s_late);
    free(m);
}

/* ---- game fill (game_fill.c, scene_map.c) ----
 *
 * Richman 11 on the tablet: the game's 3408x1917 window on a 3408x2272 X screen, shown at first as
 * the scene (no crop), then cropped to the window once it has stayed GF_SETTLE_MS (stretched over the output),
 * then the game's option changes to keep its aspect (at once). The mapping changes while pairs wait: their
 * generated frames are placed by the mapping in force when they go to the display, as their B is, so a
 * generated frame never shows the place its B (or the B before) left. */
static int64_t g_crop_on, g_crop_aspect;
static int64_t map_game_fill(int64_t t) { return t >= g_crop_aspect ? 2 : t >= g_crop_on ? 1 : 0; }

static void game_fill_mapping(int map, struct scene_map *out) {
    const int crop[4] = {0, 0, 3408, 1917};
    scene_map_compute(out, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, map ? crop : NULL,
                      map == 2 ? SCENE_MODE_FIT : SCENE_MODE_STRETCH);
}

static void sim_game_fill_crop(void) {
    /* when game_fill.c applies the crop and the option */
    struct gf_state st;
    memset(&st, 0, sizeof(st));
    struct gf_choice c;
    memset(&c, 0, sizeof(c));
    c.reason = GF_CROP;
    c.window = 0x1a00003u;
    c.app = 2074800u;
    c.index = 1;
    c.rect[2] = 3408;
    c.rect[3] = 1917;
    c.mode = GF_MODE_STRETCH;
    const int64_t report = 1000 * MS + 200 * 33333333LL + 5 * MS;
    int64_t wake = 0;
    CHECK(!gf_step(&st, &c, report, &wake) && !st.on && wake == report + GF_SETTLE_MS * MS, "the crop settles first");
    CHECK(gf_step(&st, &c, wake, &wake) && st.on, "then applies");
    g_crop_on = report + GF_SETTLE_MS * MS;
    c.mode = GF_MODE_ASPECT;
    CHECK(gf_step(&st, &c, g_crop_on + 3000 * MS, &wake) && st.on && st.cur.mode == GF_MODE_ASPECT, "the option at once");
    g_crop_aspect = g_crop_on + 3000 * MS;
    /* the three mappings place the game differently */
    struct scene_map a, b, d;
    game_fill_mapping(0, &a);
    game_fill_mapping(1, &b);
    game_fill_mapping(2, &d);
    CHECK(a.valid && b.valid && d.valid && b.crop && d.crop, "mappings");
    double ya = 0, yb = 0, yd = 0, x = 0;
    CHECK(scene_map_to_output(&a, 0, 1917, &x, &ya) && scene_map_to_output(&b, 0, 1917, &x, &yb) &&
          scene_map_to_output(&d, 0, 1917, &x, &yd), "the window's lower edge on the output");
    CHECK(ya < 1918 && yb > 2271 && yd > 2000 && yd < 2200, "the window's lower edge at %.0f, %.0f and %.0f", ya, yb,
          yd);
    const struct { int mult; int64_t refresh; int64_t gen; } cases[] = {
        {2, 8333333, 6 * MS}, {4, 8333333, 6 * MS}, {3, 11111111, 6 * MS}, {2, 8333333, 50 * MS}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (int old = 0; old < 2; old++) {
            struct sim *m = calloc(1, sizeof(*m));
            sim_init(m, cases[i].refresh, cases[i].gen, 1 * MS);
            m->render_ns = cases[i].gen > 20 * MS ? 40 * MS : 0;
            m->gen_more = 2 * MS;
            m->mult = cases[i].mult;
            m->peak = 144.0f;
            m->map_at = map_game_fill;
            const int64_t changes[2] = {g_crop_on, g_crop_aspect};
            m->map_change = changes;
            m->map_changes = 2;
            m->s_map_at_arrival = old;
            g_p1 = 33333333;
            sim_run(m);
            if (!old) {
                CHECK(m->map_back == 0, "x%d, generation %lld ms: %d refreshes show a place the screen had left",
                      cases[i].mult, (long long)(cases[i].gen / MS), m->map_back);
                CHECK(m->s_across_map >= cases[i].mult - 1, "x%d: %d generated frames waited across a change",
                      cases[i].mult, m->s_across_map);
                CHECK(m->b_lost_after_warmup == 0, "x%d with game fill: %d real frames lost", cases[i].mult,
                      m->b_lost_after_warmup);
            } else {
                /* placed by their real frame's arrival, they would go back to the place left */
                CHECK(m->map_back > 0, "x%d, generation %lld ms, placed at arrival: the old place comes back",
                      cases[i].mult, (long long)(cases[i].gen / MS));
            }
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
    votes_near_30_on_the_tablet();
    vote_steady_through_uneven_frames();
    vote_steps();
    import_formats();
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
    schedule_n_spaces_the_outputs();
    votes_n();
    vote_layers();
    contexts_and_pool();
    multiplier_downgrades();
    sim_xn_at_30();
    sim_xn_downgrades();
    sim_xn_panel_below_the_peak();
    sim_x3_a_little_above_90();
    loop_x3_near_90();
    loop_near_band_edges();
    loop_steps();
    schedule_x2_as_before();
    slots_drop_first();
    pair_times_and_actions();
    multiplier_gpu_model();
    multiplier_flight();
    multiplier_engine_limit();
    ready_median();
    sim_xn_jitter();
    sim_x2_jitter();
    sim_xn_slow_render();
    sim_game_fill_crop();
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("zerofg_pace_test: all passed\n");
    return 0;
}
