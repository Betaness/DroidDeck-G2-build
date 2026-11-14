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

struct zfg_slots {
    int64_t s, b;
};
/* Display targets of the generated frame (s) and of the real frame it precedes (b), for a real frame that
 * arrived at `arrival`. s is half a period after the previous real frame's target (prev_b; 0: none), kept
 * between arrival + lead and arrival + lead + slack; b is half a period after s. */
struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack);

/* The desired present time that puts a frame on the refresh nearest `target`: SurfaceFlinger holds a
 * transaction until a refresh whose expected present time is later than its desired present time. */
int64_t zfg_desired(int64_t target, int64_t refresh_period);

/* The layer's frame-rate vote: twice the real rate, capped at the refresh rate (refresh_hz 0: no cap),
 * rounded to whole Hz; 0 while the period is unknown. */
float zfg_vote(int64_t period, float refresh_hz);

/* Twice the real rate is above the refresh rate (5 % tolerance): no refresh is left for the generated
 * frames. 0 when either is unknown. */
int zfg_too_fast(int64_t period, float refresh_hz);

#endif
