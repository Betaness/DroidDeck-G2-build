#ifndef ZEROFG_PACE_H
#define ZEROFG_PACE_H
#include <stdint.h>

/* [mali-g2] Pacing arithmetic of ZeroFG on the zero-copy path (zerofg_zc.c), and the format it imports game
 * buffers as. Times are CLOCK_MONOTONIC nanoseconds. No Android or Vulkan: tests/zerofg_pace_test.c. */

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

/* How long after their real frames' arrival the last generations were done, for the multiplier: the median of
 * the last ZFG_READY_HIST, 0 before there are ZFG_READY_HIST / 2 (one slow pair does not move it). */
#define ZFG_READY_HIST 16
struct zfg_ready {
    int64_t v[ZFG_READY_HIST];
    int n, next;
};
void zfg_ready_reset(struct zfg_ready *r);
void zfg_ready_add(struct zfg_ready *r, int64_t delay);
int64_t zfg_ready_get(const struct zfg_ready *r);

/* How far after B's arrival S's target has to be: the measured delay until S is ready (half a period before
 * the first measurement) plus `margin`, at most two periods; then `latch` (how long before a refresh
 * SurfaceFlinger takes its buffers) and half a refresh (the desired present time may pick the refresh half a
 * refresh before the target). */
int64_t zfg_target_lead(int64_t measured, int64_t period, int64_t margin, int64_t latch, int64_t refresh);

/* How far past arrival + lead S may go to keep the rhythm: a quarter period, at least a refresh (so that where
 * twice the real rate is above the refresh rate, leaving one S out frees a whole refresh). */
int64_t zfg_slack(int64_t period, int64_t refresh);

/* xN: N - 1 generated frames per pair of real frames, at phases 1/N .. (N - 1)/N, then the real one. */
#define ZFG_MULT_MAX 4
/* x3 and x4: outputs are at least this share of their mean interval (a real-frame period / N) apart, and at least
 * a refresh. x2: a refresh. */
#define ZFG_SPACING 0.8

struct zfg_slots {
    int64_t s, b;
    int no_s;     /* no refresh of its own is left for any generated frame: B alone, s 0 */
    int n;        /* the multiplier: outputs of the pair, n - 1 generated then B */
    int64_t step; /* between the outputs */
    int first;    /* the generated frames shown are those of phases first + 1 .. n - 1 (of n); s is the target of
                   * the first of them, each next one a step later, b a step after the last */
};
/* How far apart two outputs must be: x2 a refresh; x3 and x4 ZFG_SPACING of a period / n, at least a refresh. */
int64_t zfg_spacing(int64_t period, int n, int64_t refresh);
/* Display targets of the generated frames and of the real frame they precede, for a real frame that arrived at
 * `arrival`, with n outputs per real frame. The first generated frame (s) is a period / n after the previous real
 * frame's target (prev_b; 0: none), kept between arrival + lead and arrival + lead + slack and at least
 * zfg_spacing after prev_b; the others and b follow a period / n apart (at least a refresh). When s would have to
 * pass arrival + lead + slack to stay that far after prev_b (the real frame came early against the rhythm), the
 * first generated frames are left out, as few as can be (x3 and x4 where n times the real rate is at most 5 %
 * above the refresh rate): the next one goes at least zfg_spacing after prev_b and at least m steps after
 * arrival + lead (m left out), within m steps after arrival + lead + slack, the others and b a step apart, so b
 * comes m steps earlier than the rhythm would have it. When none is left
 * (x2, or no room even for the last), b is (n - 1) periods / n after arrival + lead, at least zfg_spacing after
 * prev_b, but never later than b of a pair with generated frames could be (arrival + lead + slack + (n - 1)
 * steps): real frames that come faster than the refresh rate do not push the targets later and later; one of
 * them is then replaced on the screen by the next. */
struct zfg_slots zfg_schedule_n(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                                int64_t refresh, int n);
/* x2. */
struct zfg_slots zfg_schedule(int64_t arrival, int64_t prev_b, int64_t period, int64_t lead, int64_t slack,
                              int64_t refresh);
/* Target of generated frame k (phase k + 1 of t->n, k 0 .. t->n - 2); 0 for one left out. */
int64_t zfg_slot_s(const struct zfg_slots *t, int k);
/* Leaves out m more of the first generated frames still in t (fewer pool buffers than frames): the others and b
 * keep their targets; with none left, no_s. */
void zfg_slots_drop_first(struct zfg_slots *t, int m);
/* Generated frames of t to record and show. */
int zfg_slots_shown(const struct zfg_slots *t);

/* The desired present time that puts a frame on the refresh nearest `target`: SurfaceFlinger holds a
 * transaction until a refresh whose expected present time is later than its desired present time. */
int64_t zfg_desired(int64_t target, int64_t refresh_period);

/* The latest moment a frame can be ready and still make the refresh zfg_desired picks for `target`. */
int64_t zfg_latch_deadline(int64_t target, int64_t refresh_period, int64_t latch);

/* When a transaction with desired present time `desired` goes to SurfaceFlinger: `latch` plus `margin`
 * before it. SurfaceFlinger keeps every later transaction of the process behind one whose time has not come. */
int64_t zfg_apply_at(int64_t desired, int64_t latch, int64_t margin);

/* The times of a pair's outputs as the event loop hands them over (zerofg_zc.c pend_push): each target's desired
 * present time and apply time, and for generated frame k the last moment it may still go (s_last): handed over
 * after the commit a refresh before the next output's (in the worst phase: latch + margin before its desired
 * present time, less a refresh), SurfaceFlinger would take it with that one and it would never be seen; one ready
 * by its own apply time always goes (at least 1 ms after it). */
struct zfg_pair_times {
    int ns;           /* generated frames shown: t's zfg_slots_shown */
    int64_t s_desired[ZFG_MULT_MAX - 1], s_apply[ZFG_MULT_MAX - 1], s_last[ZFG_MULT_MAX - 1];
    int64_t b_desired, b_apply;
};
void zfg_pair_times(const struct zfg_slots *t, int64_t refresh, int64_t latch, int64_t margin,
                    struct zfg_pair_times *out);

/* What becomes of the next generated frame of the oldest waiting pair at `now`: shown once the generation is
 * complete (ready) and its apply time has come, or at once with `force`, as long as its s_last has not passed;
 * left out once it has, or at once with force when not complete; else it waits. */
enum { ZFG_S_WAIT = 0, ZFG_S_SHOW, ZFG_S_DROP };
int zfg_s_action(int ready, int64_t now, int64_t apply, int64_t last, int force);

/* The layer's frame-rate vote: the lowest of the panel's rates (`modes`, n of them; none given: the peak rate
 * over 1 .. 8) that is a whole multiple of `mult` times the real rate within 3 %, so every output keeps the same
 * number of refreshes; where none is, the highest rate up to the peak (the smallest unevenness). The previous
 * vote (0: none) stays while mult times the real rate is at most 5 % above or 8 % below a whole fraction of it
 * (ZFG_VOTE_KEEP_*) and no lower rate fits within 3 %. Rates above the peak are not voted. 0 while the period
 * or the peak rate is unknown. zfg_vote_pick: mult 2. */
#define ZFG_VOTE_FIT 0.03
#define ZFG_VOTE_KEEP_ABOVE 0.05
#define ZFG_VOTE_KEEP_BELOW 0.08
float zfg_vote_pick_n(int64_t period, int mult, const float *modes, int n, float peak_hz, float prev);
float zfg_vote_pick(int64_t period, const float *modes, int n, float peak_hz, float prev);

/* The vote over the frames. The period is smoothed further (each frame's period moves it an eighth of the way:
 * the period of a few uneven frames hardly moves it). The first vote is zfg_vote_pick_n's choice for it; then
 * that choice (with `hz` as the previous vote) replaces the vote once zfg_vote_pick_n has chosen other rates for
 * ZFG_VOTE_HOLD_NS without a break (also after the multiplier changed). zfg_vote_step: mult 2. */
#define ZFG_VOTE_HOLD_NS 1000000000LL
struct zfg_vote {
    float hz;           /* the vote in force; 0: none */
    int away;           /* zfg_vote_pick_n has chosen another rate since away_since */
    int64_t away_since;
    int64_t period;     /* smoothed; 0: none yet */
};
void zfg_vote_reset(struct zfg_vote *v);
/* The vote for a real frame at `now`; 0 while there is none. */
float zfg_vote_step_n(struct zfg_vote *v, int64_t now, int64_t period, int mult, const float *modes, int n,
                      float peak_hz);
float zfg_vote_step(struct zfg_vote *v, int64_t now, int64_t period, const float *modes, int n, float peak_hz);

/* Twice the real rate is more than 5 % above the peak refresh rate; once it was (was_fast), until it is 3 %
 * above or less. 0 when either is unknown. */
int zfg_too_fast(int64_t period, float peak_hz, int was_fast);

/* `fast`: real frames go to the display as they come, without S. Twice the real rate above the peak rate
 * (zfg_too_fast); or the real rate itself more than 2 % above the running refresh rate (until it is no
 * longer above it): not every real frame can have a refresh of its own; or twice the real rate more than
 * 5 % above the running refresh rate (until 3 % or less) for ZFG_FAST_GRACE_NS: the panel did not come up
 * to the peak rate after the vote (a power or thermal cap, the system's refresh policy). Below that, S frames
 * that find no refresh of their own are left out (zfg_schedule). refresh_hz 0: unknown, the peak rate alone. */
#define ZFG_FAST_GRACE_NS 2000000000LL
struct zfg_fast {
    int on;
    int64_t over_since; /* twice the real rate above the running refresh rate since then; 0: not */
};
int zfg_fast_update(struct zfg_fast *f, int64_t now, int64_t period, float refresh_hz, float peak_hz);

/* Interpolator::Create's frame_context_count. Each Interpolate call takes the next of them and must not reuse one
 * whose submission still runs: a pair records one call per generated frame it shows (up to n - 1), so a pair may
 * be submitted while the calls of the pairs in flight and its own `calls` fit (at least n + 1, as INTEGRATION.md
 * asks for xN). ZFG_JOBS: pairs in flight at most (zerofg_zc.c's jobs). */
#define ZFG_CONTEXTS 8
#define ZFG_JOBS 3
int zfg_calls_fit(int calls_in_flight, int calls);
/* Pairs in flight when a generation is done `ready_ns` after its real frame arrived and real frames come `period`
 * apart: the new one and those of the real frames before it within ready_ns. 0 when either is unknown. */
int zfg_flight_pairs(int64_t ready_ns, int64_t period);

/* The multiplier in force for a wanted one (2 .. ZFG_MULT_MAX), lowered one step at a time, never below 2 (x2's
 * own limits are zfg_fast_update's):
 * - ZFG_CAP_ENGINE while the engine cannot record phases off the middle (limit 2: its phase forms failed);
 * - ZFG_CAP_PEAK at once while mult times the real rate is more than 5 % above the peak refresh rate (3 % to go
 *   back up);
 * - ZFG_CAP_GPU at once while the GPU time of a pair, the first Interpolate call (a whole record) and mult - 2
 *   further ones (the same pair at another phase: what depends on the phase alone), is more than ZFG_GPU_SHARE of
 *   the real-frame period (ZFG_GPU_SHARE * ZFG_GPU_RAISE to go back up; a multiplier lowered for this is not
 *   tried again for ZFG_GPU_BLOCK_NS). An unknown first-call time: no cap; an unknown further-call time (no pair
 *   above x2 measured yet) counts as 0, so the multiplier asked for is tried and judged on its own pairs;
 * - ZFG_CAP_LATE at once while the pairs in flight (zfg_flight_pairs of `ready_ns`, the median time from a
 *   real frame's arrival to its generation's completion) would need more Interpolate calls than ZFG_CONTEXTS, or
 *   more than ZFG_JOBS pairs, so that pairs would be skipped as late (ZFG_FLIGHT_RAISE times ready_ns to go back
 *   up);
 * - ZFG_CAP_REFRESH after mult times the real rate stayed more than 5 % above the running refresh rate (refresh_hz,
 *   0: unknown) for ZFG_FAST_GRACE_NS (3 % to go back up).
 * A higher multiplier comes back once it has fitted for ZFG_MULT_RAISE_NS. vote_n is the multiplier the layer's
 * frame-rate vote is for: n, or the highest one only the running rate holds down, so the panel can come up. */
#define ZFG_GPU_SHARE 0.5
#define ZFG_GPU_RAISE 0.8
#define ZFG_GPU_BLOCK_NS 10000000000LL
#define ZFG_MULT_RAISE_NS 2000000000LL
#define ZFG_FLIGHT_RAISE 1.15
enum { ZFG_CAP_NONE = 0, ZFG_CAP_PEAK, ZFG_CAP_GPU, ZFG_CAP_REFRESH, ZFG_CAP_LATE, ZFG_CAP_ENGINE };
struct zfg_mult_in {
    int want;            /* asked for (2 .. ZFG_MULT_MAX) */
    int limit;           /* the highest the engine records (2: x2 only; 0: ZFG_MULT_MAX) */
    int64_t period;      /* the real-frame period (0: unknown) */
    float refresh_hz;    /* the panel's running rate (0: unknown) */
    float peak_hz;       /* the highest it may run at (0: unknown) */
    double gpu_first_ms; /* GPU time of a pair's first Interpolate call (0: unknown) */
    double gpu_phase_ms; /* of each further call of the pair (0: unknown) */
    int64_t ready_ns;    /* from a real frame's arrival to its generation's completion (zfg_ready_get; 0: unknown) */
};
/* The GPU time of a pair at multiplier `mult` (ms). */
double zfg_pair_gpu_ms(int mult, double first_ms, double phase_ms);
struct zfg_mult {
    int n;               /* in force; 0: none yet */
    int want;            /* the multiplier asked for: a new one is taken at once, then lowered as needed */
    int vote_n;
    int why;             /* ZFG_CAP_*: what keeps n below the wanted multiplier */
    int64_t over_since;  /* n x the real rate above the running refresh rate since then; 0: not */
    int raise;           /* a higher multiplier that has fitted since raise_since; 0: none */
    int64_t raise_since;
    int gpu_block;       /* lowered from this for the GPU time: not tried again before gpu_block_until */
    int64_t gpu_block_until;
};
void zfg_mult_reset(struct zfg_mult *m);
int zfg_mult_update(struct zfg_mult *m, int64_t now, const struct zfg_mult_in *in);
const char *zfg_cap_name(int why);

/* Buffers of the layer's pool for xN: the generated frames of three pairs (x2: 3, SC_VORTEK_POOL, as before). */
#define ZFG_POOL_PAIRS 3
#define ZFG_POOL_MAX ((ZFG_MULT_MAX - 1) * ZFG_POOL_PAIRS)
int zfg_pool_size(int n);

/* The Vulkan format a game's AHardwareBuffer is imported as: the one the driver gives for it
 * (vkGetAndroidHardwareBufferPropertiesANDROID; 0 = VK_FORMAT_UNDEFINED: none of its own); where that is none,
 * VK_FORMAT_B8G8R8A8_UNORM for HAL_PIXEL_FORMAT_BGRA_8888, which the presenters (mali-g2/hybris wsi.c, the Vortek
 * server) allocate for B8G8R8A8 swapchains and create their B8G8R8A8 images on; else 0. */
#define ZFG_AHB_BGRA8 5u          /* HAL_PIXEL_FORMAT_BGRA_8888 */
#define ZFG_VK_B8G8R8A8_UNORM 44u /* VK_FORMAT_B8G8R8A8_UNORM */
uint32_t zfg_import_format(uint32_t ahb_format, uint32_t reported);

#endif
