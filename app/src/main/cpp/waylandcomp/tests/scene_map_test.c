/* Tests of src/scene_map.c: cc -O2 -Wall -Wextra -Werror -Isrc tests/scene_map_test.c src/scene_map.c -lm */
#include "scene_map.h"

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

static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static void out_of(const struct scene_map *m, double sx, double sy, double *ox, double *oy) {
    if (!scene_map_to_output(m, sx, sy, ox, oy)) *ox = *oy = NAN;
}

static void check_rect(const char *what, const float r[4], double l, double t, double rr, double b) {
    CHECK(near(r[0], l, 0.01) && near(r[1], t, 0.01) && near(r[2], rr, 0.01) && near(r[3], b, 0.01),
          "%s: %.2f,%.2f-%.2f,%.2f, want %.2f,%.2f-%.2f,%.2f", what, r[0], r[1], r[2], r[3], l, t, rr, b);
}

/* Scene points inside the crop go to the output and back, and the output's corners land on the crop's. */
static void round_trips(const char *what, const struct scene_map *m) {
    const double x0 = m->crop_x, y0 = m->crop_y, x1 = m->crop_x + m->crop_w, y1 = m->crop_y + m->crop_h;
    for (int i = 0; i <= 8; i++)
        for (int j = 0; j <= 8; j++) {
            const double sx = x0 + (x1 - x0) * i / 8.0, sy = y0 + (y1 - y0) * j / 8.0;
            double ox, oy, bx, by;
            CHECK(scene_map_to_output(m, sx, sy, &ox, &oy), "%s: to output", what);
            CHECK(ox >= m->clip[0] - 0.01 && ox <= m->clip[2] + 0.01 && oy >= m->clip[1] - 0.01 && oy <= m->clip[3] + 0.01,
                  "%s: scene %.1f,%.1f lands at %.2f,%.2f outside the clip", what, sx, sy, ox, oy);
            CHECK(scene_map_to_scene(m, ox, oy, &bx, &by), "%s: to scene", what);
            CHECK(near(bx, sx, 1e-3) && near(by, sy, 1e-3), "%s: %.2f,%.2f came back as %.4f,%.4f", what, sx, sy, bx, by);
        }
    double sx, sy;
    scene_map_to_scene(m, m->picture[0], m->picture[1], &sx, &sy);
    CHECK(near(sx, x0, 1e-3) && near(sy, y0, 1e-3), "%s: picture origin touches %.3f,%.3f, want %.0f,%.0f", what, sx, sy, x0, y0);
    scene_map_to_scene(m, m->picture[2], m->picture[3], &sx, &sy);
    CHECK(near(sx, x1, 1e-3) && near(sy, y1, 1e-3), "%s: picture corner touches %.3f,%.3f, want %.0f,%.0f", what, sx, sy, x1, y1);
}

static void uncropped_is_unchanged(void) {
    struct scene_map m;
    scene_map_compute(&m, 1280, 720, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, NULL, 0);
    CHECK(m.valid && !m.crop, "720p fit: valid, not cropped");
    CHECK(near(m.kx, 2.6625, 1e-5) && near(m.ky, 2.6625, 1e-5), "720p fit: k %.5f %.5f", m.kx, m.ky);
    CHECK(m.off_x == 0.0f && m.off_y == 177.0f, "720p fit: shown at %.1f,%.1f, want 0,177", m.off_x, m.off_y);
    check_rect("720p fit clip (the whole region, as before)", m.clip, 0, 0, 3408, 2272);
    check_rect("720p fit picture", m.picture, 0, 177, 3408, 2094);

    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, NULL, 0);
    CHECK(m.kx == 1.0f && m.ky == 1.0f && m.off_x == 0.0f && m.off_y == 0.0f, "screen-sized scene: identity");

    scene_map_compute(&m, 1280, 720, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_TOP, NULL, 0);
    check_rect("top half region clip", m.clip, 0, 0, 3408, 1136);
    scene_map_compute(&m, 1280, 720, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_PICTURE_BOTTOM, NULL, 0);
    CHECK(near(m.off_y, 2272 - 720 * 2.6625, 1e-3), "picture bottom: %.2f", m.off_y);

    scene_map_compute(&m, 0, 720, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, NULL, 0);
    double ox, oy;
    CHECK(!m.valid && !scene_map_to_output(&m, 1, 1, &ox, &oy), "no scene: no mapping");
}

/* Richman 11 on the owner's tablet: 3408x1917 at 0,0 of a 3408x2272 X screen, Steam below it. */
static void full_width_window(void) {
    const int crop[4] = {0, 0, 3408, 1917};
    struct scene_map m;
    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, crop, SCENE_MODE_STRETCH);
    CHECK(m.valid && m.crop && m.crop_mode == SCENE_MODE_STRETCH, "3408x1917: cropped, stretched");
    CHECK(near(m.kx, 1.0, 1e-6) && near(m.ky, 2272.0 / 1917.0, 1e-5), "3408x1917: k %.5f %.5f", m.kx, m.ky);
    check_rect("3408x1917 picture", m.picture, 0, 0, 3408, 2272);
    check_rect("3408x1917 clip", m.clip, 0, 0, 3408, 2272);
    double ox, oy;
    out_of(&m, 0, 1917, &ox, &oy);
    CHECK(near(oy, 2272, 0.01), "the window's bottom edge goes to the output's: %.2f", oy);
    out_of(&m, 100, 2000, &ox, &oy);
    CHECK(oy > m.clip[3], "Steam below the window (y 2000) is cut off: lands at %.1f", oy);
    round_trips("3408x1917 stretch", &m);

    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, crop, SCENE_MODE_FIT);
    CHECK(m.crop_mode == SCENE_MODE_FIT && m.kx == 1.0f && m.ky == 1.0f, "keep aspect: 1:1");
    check_rect("3408x1917 keep aspect picture (bars above and below)", m.picture, 0, 177, 3408, 2094);
    check_rect("3408x1917 keep aspect clip", m.clip, 0, 177, 3408, 2094);
    out_of(&m, 100, 2000, &ox, &oy);
    CHECK(oy > m.clip[3], "keep aspect: Steam below the window is cut off (black bars, not Steam): %.1f", oy);
    round_trips("3408x1917 keep aspect", &m);
}

static void small_window(void) {
    struct scene_map m;
    const int centred[4] = {1064, 776, 1280, 720};
    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, centred, SCENE_MODE_STRETCH);
    CHECK(near(m.kx, 3408.0 / 1280, 1e-5) && near(m.ky, 2272.0 / 720, 1e-5), "1280x720 centred: k %.5f %.5f", m.kx, m.ky);
    double ox, oy;
    out_of(&m, 1064, 776, &ox, &oy);
    CHECK(near(ox, 0, 0.01) && near(oy, 0, 0.01), "1280x720 centred: its corner at the output's: %.2f,%.2f", ox, oy);
    out_of(&m, 2344, 1496, &ox, &oy);
    CHECK(near(ox, 3408, 0.01) && near(oy, 2272, 0.01), "1280x720 centred: far corner %.2f,%.2f", ox, oy);
    out_of(&m, 1000, 776, &ox, &oy);
    CHECK(ox < m.clip[0], "1280x720 centred: what is left of it is cut off");
    round_trips("1280x720 centred stretch", &m);

    const int corner[4] = {0, 0, 1280, 720};
    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, corner, SCENE_MODE_STRETCH);
    CHECK(m.off_x == 0.0f && m.off_y == 0.0f, "1280x720 at 0,0: no offset");
    check_rect("1280x720 at 0,0 picture", m.picture, 0, 0, 3408, 2272);
    round_trips("1280x720 at 0,0 stretch", &m);

    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, centred, SCENE_MODE_FIT);
    CHECK(near(m.kx, 2.6625, 1e-5) && m.kx == m.ky, "1280x720 keep aspect: %.5f", m.kx);
    check_rect("1280x720 keep aspect picture", m.picture, 0, 177, 3408, 2094);
    check_rect("1280x720 keep aspect clip", m.clip, 0, 177, 3408, 2094);
    out_of(&m, 1064, 776, &ox, &oy);
    CHECK(near(ox, 0, 0.01) && near(oy, 177, 0.01), "1280x720 keep aspect: corner at 0,177: %.2f,%.2f", ox, oy);
    round_trips("1280x720 keep aspect", &m);

    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_PICTURE_TOP, centred, SCENE_MODE_FIT);
    check_rect("keep aspect, picture top", m.picture, 0, 0, 3408, 1917);
    round_trips("1280x720 keep aspect, picture top", &m);
    scene_map_compute(&m, 3408, 2272, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_PICTURE_TOP, centred, SCENE_MODE_STRETCH);
    check_rect("stretch fills the output whatever the picture position", m.picture, 0, 0, 3408, 2272);
}

static void half_resolution(void) {
    struct scene_map m;
    scene_map_compute(&m, 1704, 1136, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, NULL, 0);
    CHECK(m.kx == 2.0f && m.ky == 2.0f && m.off_x == 0.0f && m.off_y == 0.0f, "half: 2x, no bars");
    const int crop[4] = {0, 0, 1704, 958};
    scene_map_compute(&m, 1704, 1136, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, crop, SCENE_MODE_STRETCH);
    CHECK(near(m.kx, 2.0, 1e-6) && near(m.ky, 2272.0 / 958, 1e-5), "half, 1704x958: k %.5f %.5f", m.kx, m.ky);
    check_rect("half, 1704x958 picture", m.picture, 0, 0, 3408, 2272);
    round_trips("half resolution stretch", &m);
    scene_map_compute(&m, 1704, 1136, 3408, 2272, SCENE_MODE_FIT, SCENE_ALIGN_CENTER, crop, SCENE_MODE_FIT);
    check_rect("half, 1704x958 keep aspect", m.picture, 0, 178, 3408, 2094);
    round_trips("half resolution keep aspect", &m);

    /* The X screen at half the scene's size (Xwayland scaling): window rectangles come in X px. */
    const int x_rect[4] = {0, 0, 852, 479};
    int scene_rect[4];
    CHECK(scene_map_root_rect(x_rect, 852, 568, 1704, 1136, scene_rect) && scene_rect[0] == 0 && scene_rect[1] == 0 &&
          scene_rect[2] == 1704 && scene_rect[3] == 958, "X px to scene px: %d,%d %dx%d", scene_rect[0], scene_rect[1],
          scene_rect[2], scene_rect[3]);
}

static void root_rects(void) {
    int r[4];
    const int inside[4] = {10, 20, 300, 200};
    CHECK(scene_map_root_rect(inside, 0, 0, 1280, 720, r) && r[0] == 10 && r[1] == 20 && r[2] == 300 && r[3] == 200,
          "same size: unchanged");
    const int over[4] = {-50, 600, 400, 400};
    CHECK(scene_map_root_rect(over, 1280, 720, 1280, 720, r) && r[0] == 0 && r[1] == 600 && r[2] == 350 && r[3] == 120,
          "clipped to the scene: %d,%d %dx%d", r[0], r[1], r[2], r[3]);
    const int off[4] = {1300, 0, 100, 100};
    CHECK(!scene_map_root_rect(off, 1280, 720, 1280, 720, r), "off the scene: nothing");
}

int main(void) {
    uncropped_is_unchanged();
    full_width_window();
    small_window();
    half_resolution();
    root_rects();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("scene_map: all passed\n");
    return 0;
}
