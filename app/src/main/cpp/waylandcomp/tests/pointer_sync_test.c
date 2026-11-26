/* Tests of src/pointer_sync.c: cc -O2 -Wall -Wextra -Werror -Isrc tests/pointer_sync_test.c src/pointer_sync.c */
#include "pointer_sync.h"

#include <math.h>
#include <stdio.h>

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

static int near(double a, double b) { return fabs(a - b) < 1e-9; }

static void motion(void) {
    /* locked: relative motion only, as before */
    for (int rel = 0; rel < 2; rel++)
        for (int consumed = 0; consumed < 2; consumed++)
            for (int moved = 0; moved < 2; moved++)
                for (int desktop = 0; desktop < 2; desktop++)
                    CHECK(ptrsync_motion(1, rel, consumed, moved, desktop) == 0,
                          "locked: no motion and no catch-up (rel %d consumed %d moved %d desktop %d)",
                          rel, consumed, moved, desktop);
    CHECK(ptrsync_motion(0, 1, 1, 1, 0) == PTRSYNC_BEHIND, "consumed delta: no motion, the position falls behind");
    CHECK(ptrsync_motion(0, 1, 1, 1, 1) == 0, "consumed delta on the desktop: no motion, nothing marked");
    CHECK(ptrsync_motion(0, 1, 0, 1, 0) == PTRSYNC_MOTION, "delta without a relative pointer: motion");
    CHECK(ptrsync_motion(0, 1, 0, 0, 0) == 0, "button without motion: none");
    CHECK(ptrsync_motion(0, 0, 0, 0, 0) == PTRSYNC_MOTION, "absolute position: motion");
    CHECK(ptrsync_motion(0, 0, 1, 1, 0) == PTRSYNC_MOTION, "absolute position with relative motion: motion");
    CHECK(ptrsync_motion(0, 0, 1, 1, 1) == PTRSYNC_MOTION, "absolute position on the desktop: motion");
}

static void catch_up(void) {
    CHECK(ptrsync_catch_up(0, 1), "behind: caught up");
    CHECK(!ptrsync_catch_up(0, 0), "up to date: nothing");
    CHECK(!ptrsync_catch_up(1, 1), "locked: nothing, also when behind");
}

static void scene_map(void) {
    double m[4] = { 0 };
    /* a 1704x1136 scene stretched over a 3408x2272 output: one scene px is two output px */
    CHECK(ptrsync_scene_map(1, 0, 0, 2, 2, 3408, 2272, 1704, 1136, m), "shown scene");
    CHECK(near(m[0] * 1704, 1.0) && near(m[1], 0) && near(m[2] * 1136, 1.0) && near(m[3], 0),
          "stretched: %g %g %g %g", m[0], m[1], m[2], m[3]);
    CHECK(near((m[0] * 783 + m[1]) * 3408, 1566), "scene x 783 at output x 1566");

    /* 1280x720 fitted into 3408x2272 at 2.6625, 3408x1917 at y 177 */
    double k = 3408.0 / 1280;
    CHECK(ptrsync_scene_map(1, 0, 177, k, 177 + k, 3408, 2272, 1280, 720, m), "letterboxed scene");
    CHECK(near((m[2] * 0 + m[3]) * 2272, 177) && near((m[2] * 720 + m[3]) * 2272, 177 + 720 * k),
          "letterboxed: top %g bottom %g", (m[2] * 0 + m[3]) * 2272, (m[2] * 720 + m[3]) * 2272);
    CHECK(near((m[0] * 640 + m[1]) * 3408, 1704), "letterboxed: centre column");

    CHECK(ptrsync_scene_map(0, 0, 0, 0, 0, 0, 0, 1704, 1136, m), "not shown yet");
    CHECK(near(m[0], 1.0 / 1704) && near(m[1], 0) && near(m[2], 1.0 / 1136) && near(m[3], 0),
          "not shown: the scene fills the output");
    CHECK(ptrsync_scene_map(1, 5, 5, 5, 5, 3408, 2272, 1704, 1136, m) && near(m[0], 1.0 / 1704),
          "a degenerate mapping falls back to the scene");
    CHECK(!ptrsync_scene_map(1, 0, 0, 2, 2, 3408, 2272, 0, 1136, m), "no scene: no mapping");
}

static void confine(void) {
    double b[4] = { -1, -1, -1, -1 };
    CHECK(ptrsync_confine_box(0, 0, 1704, 1136, 0, 0, 0, 0, 0, b), "whole window");
    CHECK(near(b[0], 0) && near(b[1], 0) && near(b[2], 1703) && near(b[3], 1135),
          "whole window: %g %g %g %g", b[0], b[1], b[2], b[3]);
    CHECK(ptrsync_confine_box(100, 50, 800, 600, 1, 200, 100, 300, 200, b), "region of a placed window");
    CHECK(near(b[0], 300) && near(b[1], 150) && near(b[2], 599) && near(b[3], 349),
          "region: %g %g %g %g", b[0], b[1], b[2], b[3]);
    CHECK(ptrsync_confine_box(0, 0, 800, 600, 1, -50, 500, 2000, 400, b), "region past the window");
    CHECK(near(b[0], 0) && near(b[1], 500) && near(b[2], 799) && near(b[3], 599),
          "cut to the window: %g %g %g %g", b[0], b[1], b[2], b[3]);
    b[0] = 7;
    CHECK(!ptrsync_confine_box(0, 0, 800, 600, 1, 900, 0, 10, 10, b) && near(b[0], 7), "empty: no box, unchanged");
    CHECK(!ptrsync_confine_box(0, 0, 0, 600, 0, 0, 0, 0, 0, b), "no window size: no box");
}

static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Clamping into the shown box and then into the confine box ends inside the published box, at the
 * same point as clamping into the published box. */
static void box_within(void) {
    const double shown[4] = {0, 0, 3407, 1916};
    const double confines[][4] = {
        {100, 100, 899, 599},       /* inside */
        {3000, 1500, 3600, 2271},   /* across the crop's edges */
        {0, 2000, 3407, 2271},      /* below it: no overlap */
        {-200, -100, -10, 400},     /* left of it */
        {0, 0, 3407, 2271},         /* the whole scene */
    };
    for (unsigned k = 0; k < sizeof(confines) / sizeof(confines[0]); k++) {
        double out[4];
        ptrsync_box_within(shown, confines[k], out);
        CHECK(out[0] <= out[2] && out[1] <= out[3], "box %u is not empty", k);
        for (double x = -400; x <= 3800; x += 97)
            for (double y = -300; y <= 2600; y += 89) {
                const double cx = clamp(clamp(x, shown[0], shown[2]), confines[k][0], confines[k][2]);
                const double cy = clamp(clamp(y, shown[1], shown[3]), confines[k][1], confines[k][3]);
                CHECK(near(cx, clamp(x, out[0], out[2])) && near(cy, clamp(y, out[1], out[3])),
                      "box %u: %.0f,%.0f clamps to %.0f,%.0f, the box gives %.0f,%.0f", k, x, y, cx, cy,
                      clamp(x, out[0], out[2]), clamp(y, out[1], out[3]));
            }
    }
    double out[4];
    ptrsync_box_within(shown, confines[1], out);
    CHECK(near(out[0], 3000) && near(out[1], 1500) && near(out[2], 3407) && near(out[3], 1916), "overlap");
    ptrsync_box_within(shown, confines[2], out);
    CHECK(near(out[1], 2000) && near(out[3], 2000), "below: the confine box's top edge");
    double same[4] = {0, 0, 3407, 1916};
    ptrsync_box_within(same, confines[0], same);
    CHECK(near(same[0], 100) && near(same[2], 899), "in place");
}

int main(void) {
    box_within();
    motion();
    catch_up();
    scene_map();
    confine();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("pointer_sync_test: ok\n");
    return 0;
}
