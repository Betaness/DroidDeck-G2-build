/* Tests of src/zerofg_pace.c: cc -O2 -Wall -Werror -Isrc tests/zerofg_pace_test.c src/zerofg_pace.c -lm */
#include "zerofg_pace.h"

#include <stdio.h>
#include <stdlib.h>

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

/* A 31 fps game on a 120 Hz panel with arrival jitter: generated and real frames alternate half a period
 * apart, and the extra delay stays within lead .. lead + slack. */
static void schedule_keeps_half_periods(void) {
    const int64_t period = 32258064, lead = 20 * MS, slack = period / 4;
    const int64_t jitter[] = {0, 3 * MS, -2 * MS, 1 * MS, -4 * MS, 4 * MS, 0, -1 * MS, 2 * MS, -3 * MS};
    int64_t prev_b = 0, prev_s = 0;
    for (int n = 0; n < 200; n++) {
        const int64_t arrival = 1000 * MS + n * period + jitter[n % 10];
        struct zfg_slots t = zfg_schedule(arrival, prev_b, period, lead, slack);
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

static void schedule_without_jitter_is_even(void) {
    const int64_t period = 32 * MS, lead = 20 * MS, slack = 8 * MS;
    int64_t prev_b = 0, last_b = 0;
    for (int n = 0; n < 50; n++) {
        const int64_t arrival = n * period;
        struct zfg_slots t = zfg_schedule(arrival, prev_b, period, lead, slack);
        if (prev_b) CHECK(t.s - last_b == period / 2, "frame %d: s %lld ms after the previous b", n,
                          (long long)((t.s - last_b) / MS));
        CHECK(t.s == arrival + lead, "steady: no extra delay");
        prev_b = last_b = t.b;
    }
}

static void schedule_follows_a_late_frame_and_pulls_back(void) {
    const int64_t period = 32 * MS, lead = 20 * MS, slack = 8 * MS;
    struct zfg_slots a = zfg_schedule(0, 0, period, lead, slack);
    CHECK(a.s == lead, "first frame: arrival + lead");
    /* 30 ms late: the rhythm would put s before the frame can be ready */
    struct zfg_slots b = zfg_schedule(period + 30 * MS, a.b, period, lead, slack);
    CHECK(b.s == period + 30 * MS + lead, "late frame: arrival + lead");
    /* back on time: the rhythm is 30 ms behind lead, pulled back to lead + slack */
    struct zfg_slots c = zfg_schedule(2 * period, b.b, period, lead, slack);
    CHECK(c.s == 2 * period + lead + slack, "pulled back to lead + slack, got %lld", (long long)c.s);
    struct zfg_slots d = zfg_schedule(3 * period, c.b, period, lead, slack);
    CHECK(d.s == 3 * period + lead + slack, "then even again");
}

static void desired_times(void) {
    const int64_t r120 = 8333333;
    CHECK(zfg_desired(100 * MS, r120) == 100 * MS - r120 / 2, "half a refresh early");
    CHECK(zfg_desired(100 * MS, 0) == 100 * MS, "unknown refresh: the target");
}

static void votes(void) {
    CHECK(zfg_vote(0, 120.0f) == 0.0f, "unknown period: no vote");
    CHECK(zfg_vote(32258064, 120.0f) == 62.0f, "31 fps -> 62, got %.1f", (double)zfg_vote(32258064, 120.0f));
    CHECK(zfg_vote(16 * MS, 120.0f) == 120.0f, "62.5 fps -> capped at 120");
    CHECK(zfg_vote(11 * MS, 144.0f) == 144.0f, "91 fps -> capped at 144");
    CHECK(zfg_vote(25 * MS, 0.0f) == 80.0f, "no refresh known: uncapped");
}

static void too_fast(void) {
    CHECK(!zfg_too_fast(32 * MS, 120.0f), "31 fps on 120 Hz: room");
    CHECK(!zfg_too_fast(16666666, 120.0f), "60 fps on 120 Hz: room");
    CHECK(zfg_too_fast(11235955, 120.0f), "89 fps on 120 Hz: no room");
    CHECK(!zfg_too_fast(13888888, 144.0f), "72 fps on 144 Hz: room");
    CHECK(!zfg_too_fast(11 * MS, 0.0f), "refresh unknown: not judged");
}

int main(void) {
    period_is_a_trimmed_mean();
    gaps();
    lead_rises_at_once_and_falls_slowly();
    schedule_keeps_half_periods();
    schedule_without_jitter_is_even();
    schedule_follows_a_late_frame_and_pulls_back();
    desired_times();
    votes();
    too_fast();
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("zerofg_pace_test: all passed\n");
    return 0;
}
