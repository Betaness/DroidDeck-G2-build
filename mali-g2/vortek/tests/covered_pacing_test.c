/* [mali-g2] The pacing of entirely hidden windows (src/covered_pacing.h) on a simulated clock: the
 * VORTEK_COVERED_FPS values, the frame rate a hidden window gets, how soon a wait ends when the
 * window shows again (with and without a restack), how often the coverage is checked during a wait,
 * and the rate kept when a shown window is drawn on the same thread. Prints COVERED-PASS or
 * COVERED-FAIL.
 *
 * Build (from mali-g2/vortek):
 *   cc -O2 -Wall -Isrc -o out/covered_pacing_test tests/covered_pacing_test.c -lm && out/covered_pacing_test */
#include <math.h>
#include <stdio.h>

#include "covered_pacing.h"

static int failures;

#define EXPECT(cond, ...)                \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL %s: ", #cond);  \
            printf(__VA_ARGS__);         \
            printf("\n");                \
            failures++;                  \
        }                                \
    } while (0)

typedef struct Sim {
    double t;
    double exposeAt;       /* the window shows from then on */
    double restackEvery;   /* the root's children change this often (0: never) */
    double nextRestack;
    bool restackOnExpose;  /* the change that shows it is a restack */
    bool exposeRestackSeen;
    int checks, restacks;
} Sim;

static double simNow(void* ctx) {
    return ((Sim*)ctx)->t;
}

static void simSleep(void* ctx, double seconds) {
    ((Sim*)ctx)->t += seconds + 0.0002; /* nanosleep oversleeps a little */
}

static bool simRestacked(void* ctx) {
    Sim* s = ctx;
    s->t += 0.0001;
    if (s->restackOnExpose && !s->exposeRestackSeen && s->t >= s->exposeAt) {
        s->exposeRestackSeen = true;
        s->restacks++;
        return true;
    }
    if (s->restackEvery > 0 && s->t >= s->nextRestack) {
        while (s->nextRestack <= s->t) s->nextRestack += s->restackEvery;
        s->restacks++;
        return true;
    }
    return false;
}

static bool simHidden(void* ctx) {
    Sim* s = ctx;
    s->checks++;
    s->t += 0.0004;
    return s->t < s->exposeAt;
}

static const CoveredWaitOps simOps = {simNow, simSleep, simRestacked, simHidden};

typedef struct Run {
    int frames;        /* hidden frames presented */
    double longest;    /* the longest present */
    double endedAt;    /* when the present that saw the window show returned, 0: none did */
    int checks, restacks;
} Run;

/* An application presenting a hidden window back to back from `start` for `seconds`, each frame
 * 2 ms of CPU and 6 ms of GPU work; the window shows at exposeAt. As presentCovered does it: the
 * coverage was checked when the present began, then the GPU wait, then the pacing wait. */
static Run simulate(int fps, bool shared, double start, double seconds, double exposeAt, bool restackOnExpose,
                    double restackEvery) {
    Sim s = {.t = start, .exposeAt = exposeAt, .restackEvery = restackEvery, .nextRestack = start + restackEvery,
             .restackOnExpose = restackOnExpose};
    Run r = {0};
    double last = 0;
    while (s.t < start + seconds && s.t < exposeAt) {
        s.t += 0.002;
        double begin = s.t, checked = s.t;
        s.t += 0.006;
        last = covered_wait(&simOps, &s, last + covered_interval(fps, shared), checked);
        double took = s.t - begin;
        if (took > r.longest) r.longest = took;
        r.frames++;
        if (s.t >= exposeAt && !r.endedAt) r.endedAt = s.t;
    }
    r.checks = s.checks;
    r.restacks = s.restacks;
    return r;
}

int main(void) {
    EXPECT(covered_fps_parse(NULL) == 1, "unset");
    EXPECT(covered_fps_parse("") == 1, "empty");
    EXPECT(covered_fps_parse("10") == 10, "10");
    EXPECT(covered_fps_parse("1") == 1, "1");
    EXPECT(covered_fps_parse("0") == 1, "0");
    EXPECT(covered_fps_parse("-5") == 1, "-5");
    EXPECT(covered_fps_parse("2x") == 1, "2x");
    EXPECT(covered_fps_parse("fast") == 1, "fast");
    EXPECT(covered_fps_parse("99999") == COVERED_MAX_FPS, "99999");

    EXPECT(fabs(covered_interval(1, false) - 1.0) < 1e-9, "1 fps");
    EXPECT(fabs(covered_interval(10, false) - 0.1) < 1e-9, "10 fps");
    EXPECT(fabs(covered_interval(1, true) - 1.0 / COVERED_SHARED_FPS) < 1e-9, "1 fps shared");
    EXPECT(fabs(covered_interval(30, true) - 1.0 / 30) < 1e-9, "30 fps shared");
    EXPECT(fabs(covered_interval(0, false) - 1.0) < 1e-9, "0 fps");

    EXPECT(covered_shown_recently(10.0, 9.8, true), "0.2 s ago");
    EXPECT(!covered_shown_recently(10.0, 9.4, true), "0.6 s ago");
    EXPECT(!covered_shown_recently(10.0, 9.9, false), "another thread");
    EXPECT(!covered_shown_recently(10.0, 0, true), "never");

    /* The default: 1 frame a second, a present blocks for at most a second and its GPU work. */
    Run r = simulate(1, false, 100, 10, 1e9, false, 0);
    printf("1 fps: %d frames in 10 s, longest present %.3f s, %d checks\n", r.frames, r.longest, r.checks);
    EXPECT(r.frames >= 10 && r.frames <= 11, "%d frames", r.frames);
    EXPECT(r.longest <= 1.0 + COVERED_SLICE + 0.01, "longest %.3f", r.longest);
    EXPECT(r.checks >= 90 && r.checks <= 105, "%d checks", r.checks);

    /* VORTEK_COVERED_FPS=10: the old rate, and no extra checks (each frame was checked anyway). */
    r = simulate(10, false, 100, 10, 1e9, false, 0);
    printf("10 fps: %d frames in 10 s, longest present %.3f s, %d checks\n", r.frames, r.longest, r.checks);
    EXPECT(r.frames >= 99 && r.frames <= 101, "%d frames", r.frames);
    EXPECT(r.checks == 0, "%d checks", r.checks);

    /* A shown window drawn on the same thread: at least COVERED_SHARED_FPS. */
    r = simulate(1, true, 100, 10, 1e9, false, 0);
    printf("1 fps, shared thread: %d frames in 10 s\n", r.frames);
    EXPECT(r.frames >= 99 && r.frames <= 101, "%d frames", r.frames);

    /* Windows created, destroyed or restacked over it while it stays hidden: still 1 frame a second. */
    r = simulate(1, false, 100, 10, 1e9, false, 0.3);
    printf("1 fps, restacks every 0.3 s: %d frames in 10 s, %d restacks, %d checks\n", r.frames, r.restacks, r.checks);
    EXPECT(r.frames >= 10 && r.frames <= 11, "%d frames", r.frames);
    EXPECT(r.restacks >= 30, "%d restacks", r.restacks);

    /* Shown again by an unmap or a move (no restack): the wait ends within a check interval. */
    for (double at = 103.0; at < 104.0; at += 0.07) {
        r = simulate(1, false, 100, 10, at, false, 0);
        EXPECT(r.endedAt > 0 && r.endedAt - at <= COVERED_CHECK_EVERY + COVERED_SLICE + 0.002, "shown at %.2f, ended %.4f",
               at, r.endedAt);
    }
    /* Shown by a restack (the game lowered, the Steam window raised): within a slice. */
    for (double at = 103.0; at < 104.0; at += 0.07) {
        r = simulate(1, false, 100, 10, at, true, 0);
        EXPECT(r.endedAt > 0 && r.endedAt - at <= COVERED_SLICE + 0.002, "shown at %.2f (restack), ended %.4f", at, r.endedAt);
    }
    r = simulate(1, false, 100, 10, 103.55, true, 0);
    printf("1 fps, shown by a restack at 103.550: the present returned at %.4f\n", r.endedAt);

    /* A frame that is already late does not wait or check. */
    Sim s = {.t = 50, .exposeAt = 1e9};
    double counted = covered_wait(&simOps, &s, 49.5, 49.0);
    EXPECT(counted == 50 && s.checks == 0 && s.t == 50, "late frame counted %.3f, %d checks, t %.3f", counted, s.checks, s.t);
    /* A wait that runs to its end counts the due time (no drift from oversleeping). */
    s = (Sim){.t = 50, .exposeAt = 1e9};
    counted = covered_wait(&simOps, &s, 50.5, 50.0);
    EXPECT(counted == 50.5 && s.t >= 50.5, "counted %.4f, t %.4f", counted, s.t);

    printf(failures ? "COVERED-FAIL (%d)\n" : "COVERED-PASS\n", failures);
    return failures != 0;
}
