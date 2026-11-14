#ifndef ZEROFG_PACE_H
#define ZEROFG_PACE_H
#include <stdint.h>

/* [mali-g2] Pacing arithmetic of ZeroFG on the zero-copy path (zerofg_zc.c). Times are CLOCK_MONOTONIC
 * nanoseconds. No Android or Vulkan: tests/zerofg_pace_test.c. */

#define ZFG_PERIOD_HIST 8
#define ZFG_GAP_MIN_NS 250000000LL

struct zfg_period {
    int64_t iv[ZFG_PERIOD_HIST];
    int n, next;
};

void zfg_period_reset(struct zfg_period *p);
void zfg_period_add(struct zfg_period *p, int64_t interval);
/* The real-frame period: the mean of the last intervals without the longest and the shortest (once there
 * are four); 0 before the first interval. */
int64_t zfg_period_get(const struct zfg_period *p);

/* An interval that breaks the sequence (a pause, a loading screen, a stall): longer than four periods and
 * than ZFG_GAP_MIN_NS. */
int zfg_is_gap(int64_t interval, int64_t period);

/* How long after a real frame arrives its generated frame is ready: rises to a longer delay at once and
 * moves an eighth of the way towards a shorter one. lead 0: none yet. */
int64_t zfg_lead_update(int64_t lead, int64_t ready_delay);

/* How far after B's arrival S's target has to be: the measured delay until S is ready (half a period before
 * the first measurement) plus `margin`, at most two periods; then `latch` (how long before a refresh
 * SurfaceFlinger takes its buffers) and half a refresh (the desired present time may pick the refresh half a
 * refresh before the target). */
int64_t zfg_target_lead(int64_t measured, int64_t period, int64_t margin, int64_t latch, int64_t refresh);

/* How far past arrival + lead S may go to keep the rhythm: a quarter period, at least a refresh (so that where
 * twice the real rate is above the refresh rate, leaving one S out frees a whole refresh). */
int64_t zfg_slack(int64_t period, int64_t refresh);

struct zfg_slots {
    int64_t s, b;
    int no_s; /* no refresh of its own is left for S: B alone, s 0 */
};
/* Display targets of the generated frame (s) and of the real frame it precedes (b), for a real frame that
 * arrived at `arrival`. s is half a period after the previous real frame's target (prev_b; 0: none), kept
 * between arrival + lead and arrival + lead + slack and at least one refresh after prev_b; b is half a period
 * after s, at least one refresh. When s would have to pass arrival + lead + slack to stay a refresh after
 * prev_b, there is no S: b is half a period after arrival + lead, at least one refresh after prev_b. */
struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                              int64_t refresh);

/* The desired present time that puts a frame on the refresh nearest `target`: SurfaceFlinger holds a
 * transaction until a refresh whose expected present time is later than its desired present time. */
int64_t zfg_desired(int64_t target, int64_t refresh_period);

/* The latest moment a frame can be ready and still make the refresh zfg_desired picks for `target`. */
int64_t zfg_latch_deadline(int64_t target, int64_t refresh_period, int64_t latch);

/* When a transaction with desired present time `desired` goes to SurfaceFlinger: `latch` plus `margin`
 * before it. SurfaceFlinger keeps every later transaction of the process behind one whose time has not come. */
int64_t zfg_apply_at(int64_t desired, int64_t latch, int64_t margin);

/* The layer's frame-rate vote is peak / k: k is the largest whole number for which peak / k still has a
 * refresh for every output at twice the real rate (at least 1). The previous k (0: none) stays while twice
 * the real rate is at most 2 % above its rate, or until the next k fits with 5 % to spare. 0 while the
 * period or the peak rate is unknown. */
int zfg_vote_div(int64_t period, float peak_hz, int prev_k);

/* Twice the real rate is more than 5 % above the peak refresh rate; once it was (was_fast), until it is 3 %
 * above or less. 0 when either is unknown. Between the refresh rate and that, S frames that find no
 * refresh of their own are left out (zfg_schedule). */
int zfg_too_fast(int64_t period, float peak_hz, int was_fast);

#endif
