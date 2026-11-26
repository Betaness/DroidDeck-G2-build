/* Tests of src/game_fill.c: cc -O2 -Wall -Wextra -Werror -Isrc tests/game_fill_test.c src/game_fill.c */
#include "game_fill.h"

#include <stdio.h>
#include <string.h>

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

#define MS 1000000LL
#define STEAM 0x200001u
#define GAME 0x1a00003u
#define APP 2074800u

static struct gf_report screen(int w, int h, uint32_t focus) {
    struct gf_report r;
    memset(&r, 0, sizeof(r));
    r.root_w = (uint32_t)w;
    r.root_h = (uint32_t)h;
    r.focus = focus;
    return r;
}

static void add(struct gf_report *r, uint32_t id, uint32_t app, int x, int y, int w, int h, uint32_t flags,
                const char *cls) {
    struct gf_window *v = &r->win[r->n++];
    v->id = id; v->app = app; v->x = x; v->y = y; v->w = w; v->h = h; v->flags = flags;
    snprintf(v->cls, sizeof(v->cls), "%s", cls);
}

/* Richman 11 on the owner's tablet: Steam Big Picture 3408x2272, the game above it at 3408x1917+0+0. */
static struct gf_report richman(void) {
    struct gf_report r = screen(3408, 2272, GAME);
    add(&r, STEAM, GF_STEAM_APP, 0, 0, 3408, 2272, 0, "steamwebhelper/steam");
    add(&r, GAME, APP, 0, 0, 3408, 1917, 0, "richman11/steam_app_2074800");
    return r;
}

static int rect_is(const struct gf_choice *c, int x, int y, int w, int h) {
    return c->rect[0] == x && c->rect[1] == y && c->rect[2] == w && c->rect[3] == h;
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static size_t encode(const struct gf_report *r, unsigned char *buf) {
    put32(buf, GF_MAGIC); put32(buf + 4, GF_VERSION); put32(buf + 8, r->root_w); put32(buf + 12, r->root_h);
    put32(buf + 16, r->focus); put32(buf + 20, r->flags); put32(buf + 24, (uint32_t)r->n);
    for (int i = 0; i < r->n; i++) {
        unsigned char *q = buf + GF_HEADER_BYTES + (size_t)i * GF_WINDOW_BYTES;
        const struct gf_window *w = &r->win[i];
        put32(q, w->id); put32(q + 4, w->app); put32(q + 8, (uint32_t)w->x); put32(q + 12, (uint32_t)w->y);
        put32(q + 16, (uint32_t)w->w); put32(q + 20, (uint32_t)w->h); put32(q + 24, w->flags);
        memset(q + 28, 0, GF_CLASS_LEN);
        memcpy(q + 28, w->cls, strlen(w->cls));
    }
    return GF_HEADER_BYTES + (size_t)r->n * GF_WINDOW_BYTES;
}

static void decode(void) {
    struct gf_report r = richman(), back;
    r.win[1].x = -3;
    r.flags = GF_REPORT_STEAM_MENU;
    unsigned char buf[GF_HEADER_BYTES + GF_MAX_WINDOWS * GF_WINDOW_BYTES];
    size_t n = encode(&r, buf);
    CHECK(gf_decode(buf, n, &back) == 0, "decodes");
    CHECK(back.root_w == 3408 && back.root_h == 2272 && back.focus == GAME && back.flags == GF_REPORT_STEAM_MENU &&
          back.n == 2, "header");
    CHECK(back.win[1].id == GAME && back.win[1].app == APP && back.win[1].x == -3 && back.win[1].w == 3408 &&
          back.win[1].h == 1917 && !strcmp(back.win[1].cls, "richman11/steam_app_2074800"), "window");
    CHECK(gf_decode(buf, n - 1, &back) != 0, "short message refused");
    buf[0] ^= 1;
    CHECK(gf_decode(buf, n, &back) != 0, "bad magic refused");
    buf[0] ^= 1;
    put32(buf + 24, GF_MAX_WINDOWS + 1);
    CHECK(gf_decode(buf, n, &back) != 0, "too many windows refused");
    put32(buf + 24, 2);
    memset(buf + 28 + 28, 'x', GF_CLASS_LEN);
    CHECK(gf_decode(buf, n, &back) == 0 && strlen(back.win[0].cls) == GF_CLASS_LEN - 1, "class always ends");
}

static void measured_case(void) {
    struct gf_report r = richman();
    struct gf_choice c;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP, "3408x1917 on 3408x2272: cropped (%s)", gf_reason_text(c.reason));
    CHECK(c.window == GAME && c.app == APP && c.index == 1 && rect_is(&c, 0, 0, 3408, 1917), "the game's window");
    CHECK(!strcmp(c.cls, "richman11/steam_app_2074800"), "its class: %s", c.cls);
    int idx[4];
    CHECK(gf_strays(&r, &c, idx, 4) == 0, "Steam below is not a stray");
}

static void small_windows(void) {
    struct gf_report r = screen(3408, 2272, GAME);
    add(&r, STEAM, GF_STEAM_APP, 0, 0, 3408, 2272, 0, "steam");
    add(&r, GAME, APP, 1064, 776, 1280, 720, 0, "game");
    struct gf_choice c;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && rect_is(&c, 1064, 776, 1280, 720), "1280x720 centred: cropped (%s)", gf_reason_text(c.reason));
    r.win[1].x = r.win[1].y = 0;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && rect_is(&c, 0, 0, 1280, 720), "1280x720 at 0,0: cropped");
    r.win[1].x = -10;
    r.win[1].y = 2000;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_SMALL, "only 272 rows of it on the screen: too small (%s)", gf_reason_text(c.reason));

    struct gf_report splash = screen(3408, 2272, 0x300u);
    add(&splash, STEAM, GF_STEAM_APP, 0, 0, 3408, 2272, 0, "steam");
    add(&splash, 0x300u, APP, 1384, 956, 640, 360, 0, "splash");
    gf_pick(&splash, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_SMALL && c.window == 0x300u, "640x360 splash on 3408x2272: ignored (%s)", gf_reason_text(c.reason));
    struct gf_report small_screen = screen(1280, 720, 0x300u);
    add(&small_screen, 0x300u, APP, 340, 210, 600, 300, 0, "splash");
    gf_pick(&small_screen, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_SMALL, "600x300 splash on 1280x720: ignored");
    small_screen.win[0].w = 800;
    small_screen.win[0].h = 600;
    gf_pick(&small_screen, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP, "800x600 on 1280x720: cropped");

    /* The splash in front of the game window: the game window is cropped to, the splash inside it stays. */
    struct gf_report both = screen(3408, 2272, 0x300u);
    add(&both, GAME, APP, 0, 0, 3408, 1917, 0, "game");
    add(&both, 0x300u, APP, 1384, 778, 640, 360, 0, "splash");
    gf_pick(&both, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME, "small focused window: its game's big one");
    int idx[4];
    CHECK(gf_strays(&both, &c, idx, 4) == 0, "the splash inside the game window is no stray");
    both.win[1].y = 1800;
    CHECK(gf_strays(&both, &c, idx, 4) == 1 && idx[0] == 1, "a popup reaching below the window is a stray");

    CHECK(gf_big_enough(1022, 682, 3408, 2272, 0) && !gf_big_enough(1021, 682, 3408, 2272, 0), "30%% of the width");
    CHECK(!gf_big_enough(1000, 650, 3408, 2272, 0) && gf_big_enough(1000, 650, 3408, 2272, 1), "the shown window keeps counting to 25%%");
    CHECK(gf_big_enough(640, 360, 1280, 720, 0) && !gf_big_enough(639, 360, 1280, 720, 0), "at least 640x360");
    CHECK(gf_big_enough(400, 300, 800, 600, 0), "no more than half of a small screen");
}

static void scene_sized(void) {
    struct gf_report r = richman();
    r.win[1].h = 2272;
    struct gf_choice c;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_FULLSCREEN, "window at the screen's size: no crop (%s)", gf_reason_text(c.reason));
    r.win[1].x = -8;
    r.win[1].w = 3500;
    r.win[1].h = 2400;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_FULLSCREEN, "window larger than the screen: no crop");
}

static void two_windows(void) {
    struct gf_report r = screen(1920, 1080, GAME);
    add(&r, STEAM, GF_STEAM_APP, 0, 0, 1920, 1080, 0, "steam");
    add(&r, 0x400u, APP, 0, 0, 1280, 800, 0, "launcher");
    add(&r, GAME, APP, 160, 90, 1600, 900, 0, "game");
    struct gf_choice c;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME && rect_is(&c, 160, 90, 1600, 900), "the larger one");
    r.focus = 0x400u;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME, "the larger one also when the other has the focus");
    r.focus = 0;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME, "no focus given: the game of the topmost window");
    r.win[1].w = 1700;
    r.win[1].h = 1000;
    r.focus = GAME;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == 0x400u, "a launcher left open and larger: it wins without a swapchain");
    const uint32_t presenting[] = {0x777u, GAME};
    gf_pick(&r, 0, presenting, 2, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME, "the window with the zero-copy swapchain wins");
    r.win[1].w = 1280;
    r.win[1].h = 800;
    r.win[2].flags = GF_WIN_OVERRIDE;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == 0x400u, "an override-redirect window is never the game window");
    int idx[4];
    CHECK(gf_strays(&r, &c, idx, 4) == 1 && idx[0] == 2, "but it is a stray above the launcher");
}

/* A modal dialog of the game: Wine makes the game window inert and gives the dialog the focus. */
static void dialogs(void) {
    struct gf_report r = screen(1280, 720, 0x500u);
    add(&r, STEAM, GF_STEAM_APP, 0, 0, 1280, 720, 0, "steam");
    add(&r, GAME, APP, 0, 0, 1280, 720, GF_WIN_INERT, "game");
    add(&r, 0x500u, APP, 240, 60, 800, 600, GF_WIN_TRANSIENT | GF_WIN_DIALOG, "dialog");
    struct gf_choice c;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_FULLSCREEN && c.window == GAME, "800x600 dialog over a 1280x720 game: no crop (%s)",
          gf_reason_text(c.reason));
    r.win[2].flags = 0;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_FULLSCREEN && c.window == GAME, "unmarked dialog: the larger game window still");
    r.win[1].w = 600;
    r.win[1].h = 300;
    r.win[2].flags = GF_WIN_TRANSIENT;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_SMALL, "a transient window is never cropped to, even the largest (%s)", gf_reason_text(c.reason));

    struct gf_report big = screen(3408, 2272, 0x500u);
    add(&big, GAME, APP, 0, 0, 1280, 720, GF_WIN_INERT, "game");
    add(&big, 0x500u, APP, 240, 60, 800, 600, GF_WIN_TRANSIENT | GF_WIN_DIALOG, "dialog");
    gf_pick(&big, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME && rect_is(&c, 0, 0, 1280, 720), "the game window with its dialog in it");
    int idx[4];
    CHECK(gf_strays(&big, &c, idx, 4) == 0, "the dialog inside is no stray");
    big.win[1].flags = GF_WIN_DIALOG;
    big.win[1].w = 1400;
    big.win[1].h = 900;
    gf_pick(&big, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && c.window == GAME, "a dialog-type window larger than the game is not cropped to");

    struct gf_report splash = screen(1280, 720, 0x600u);
    add(&splash, 0x600u, APP, 0, 0, 1024, 600, GF_WIN_DIALOG, "splash");
    gf_pick(&splash, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_SMALL, "a splash-type window alone: no crop");
}

static void steam_and_others(void) {
    struct gf_report r = richman();
    struct gf_choice c;
    r.flags = GF_REPORT_STEAM_MENU;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_STEAM, "Steam menu open");
    r = richman();
    r.focus = STEAM;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_STEAM, "Steam focused");
    r = richman();
    r.win[1].app = 0;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_NOT_GAME, "no app: not a game");
    struct gf_report none = screen(3408, 2272, 0);
    gf_pick(&none, 0, NULL, 0, &c);
    CHECK(c.reason == GF_OFF_NO_FOCUS, "no windows");
}

static void half_resolution(void) {
    struct gf_report r = screen(1704, 1136, GAME);
    add(&r, STEAM, GF_STEAM_APP, 0, 0, 1704, 1136, 0, "steam");
    add(&r, GAME, APP, 0, 0, 1704, 958, 0, "game");
    struct gf_choice c;
    gf_pick(&r, 0, NULL, 0, &c);
    CHECK(c.reason == GF_CROP && rect_is(&c, 0, 0, 1704, 958), "half resolution: cropped");
}

static void modes(void) {
    const uint32_t apps[] = {440u, APP};
    const int m[] = {GF_MODE_OFF, GF_MODE_ASPECT};
    CHECK(gf_mode_for(GF_MODE_STRETCH, apps, m, 2, APP) == GF_MODE_ASPECT, "the game's own");
    CHECK(gf_mode_for(GF_MODE_STRETCH, apps, m, 2, 440u) == GF_MODE_OFF, "off for one game");
    CHECK(gf_mode_for(GF_MODE_STRETCH, apps, m, 2, 70u) == GF_MODE_STRETCH, "the default");
    CHECK(gf_mode_for(GF_MODE_ASPECT, NULL, NULL, 0, 70u) == GF_MODE_ASPECT, "no table");
}

static struct gf_choice crop(uint32_t window, int x, int y, int w, int h, int mode) {
    struct gf_choice c;
    memset(&c, 0, sizeof(c));
    c.reason = GF_CROP;
    c.window = window;
    c.app = APP;
    c.rect[0] = x; c.rect[1] = y; c.rect[2] = w; c.rect[3] = h;
    c.mode = mode;
    return c;
}

static struct gf_choice off(int reason) {
    struct gf_choice c;
    memset(&c, 0, sizeof(c));
    c.reason = reason;
    return c;
}

static void debounce(void) {
    struct gf_state s;
    memset(&s, 0, sizeof(s));
    int64_t wake;
    struct gf_choice a = crop(GAME, 0, 0, 800, 600, GF_MODE_STRETCH);
    CHECK(!gf_step(&s, &a, 0, &wake) && !s.on && wake == GF_SETTLE_MS * MS, "a new window waits");
    /* The window grows during startup: each new size starts over. */
    struct gf_choice b = crop(GAME, 0, 0, 1280, 720, GF_MODE_STRETCH);
    CHECK(!gf_step(&s, &b, 200 * MS, &wake) && wake == 700 * MS, "resized: waits again");
    struct gf_choice b1 = crop(GAME, 1, 0, 1280, 721, GF_MODE_STRETCH);
    CHECK(!gf_step(&s, &b1, 400 * MS, &wake) && wake == 700 * MS, "a pixel or two: the same place");
    CHECK(!gf_step(&s, &b, 699 * MS, &wake) && !s.on, "not yet");
    CHECK(gf_step(&s, &b, 700 * MS, &wake) && s.on && s.cur.window == GAME && s.cur.rect[2] == 1280 && !wake, "applied");
    CHECK(!gf_step(&s, &b1, 800 * MS, &wake) && s.cur.rect[0] == 0, "jitter keeps the rectangle");

    struct gf_choice aspect = b;
    aspect.mode = GF_MODE_ASPECT;
    CHECK(gf_step(&s, &aspect, 900 * MS, &wake) && s.cur.mode == GF_MODE_ASPECT, "a new mode applies at once");

    struct gf_choice moved = crop(GAME, 100, 100, 1280, 720, GF_MODE_ASPECT);
    CHECK(!gf_step(&s, &moved, 1000 * MS, &wake) && s.on && s.cur.rect[0] == 0 && wake == 1500 * MS,
          "the same window moved: its old crop until the new place settles");
    CHECK(!gf_step(&s, &aspect, 1100 * MS, &wake) && !wake && !s.pending, "moved back: nothing changes");

    struct gf_choice steam = off(GF_OFF_STEAM);
    CHECK(gf_step(&s, &steam, 1200 * MS, &wake) && !s.on, "Steam in front: off at once");
    CHECK(gf_step(&s, &aspect, 1250 * MS, &wake) && s.on, "back to the same window: on at once");
    struct gf_choice full = off(GF_OFF_FULLSCREEN);
    CHECK(gf_step(&s, &full, 1300 * MS, &wake) && !s.on, "fullscreen: off at once");
    CHECK(gf_step(&s, &aspect, 1400 * MS, &wake) && s.on, "out of fullscreen: on at once");
    struct gf_choice gone = off(GF_OFF_NO_FOCUS);
    CHECK(gf_step(&s, &gone, 2000 * MS, &wake) && !s.on && !wake, "the window gone: off at once");
    CHECK(gf_step(&s, &aspect, 2100 * MS, &wake) && s.on, "and back at once when it returns to its place");

    /* Another window becomes the game window: the old crop goes at once, the new one settles. */
    struct gf_choice other = crop(0x500u, 0, 0, 1024, 768, GF_MODE_STRETCH);
    CHECK(gf_step(&s, &other, 3000 * MS, &wake) && !s.on && wake == 3500 * MS, "another window: off at once, then waits");
    CHECK(!gf_step(&s, &other, 3200 * MS, &wake) && !s.on && wake == 3500 * MS, "still waiting");
    CHECK(gf_step(&s, &other, 3500 * MS, &wake) && s.on && s.cur.window == 0x500u, "then on it");
    struct gf_choice opt = off(GF_OFF_OPTION);
    CHECK(gf_step(&s, &opt, 3600 * MS, &wake) && !s.on, "option off: off at once");
    CHECK(!gf_step(&s, &opt, 3700 * MS, &wake) && !s.on, "off stays off");

    struct gf_choice third = crop(0x600u, 0, 0, 1024, 768, GF_MODE_STRETCH);
    CHECK(!gf_step(&s, &third, 4000 * MS, &wake) && wake == 4500 * MS, "a third window waits");
    CHECK(!gf_step(&s, &steam, 4100 * MS, &wake) && !s.pending, "Steam in front drops what waits");
    CHECK(!gf_step(&s, &third, 4200 * MS, &wake) && wake == 4700 * MS, "and it starts over");
}

int main(void) {
    decode();
    measured_case();
    small_windows();
    scene_sized();
    two_windows();
    dialogs();
    steam_and_others();
    half_resolution();
    modes();
    debounce();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("game_fill: all passed\n");
    return 0;
}
