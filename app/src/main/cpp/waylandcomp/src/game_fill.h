#ifndef GAME_FILL_H
#define GAME_FILL_H
#include <stddef.h>
#include <stdint.h>

/* [mali-g2] Game window fill: a Steam game's top-level X window that does not cover the X screen is
 * shown over the whole output, by cropping the scene-to-output mapping to it (scene_map.h). Sessions
 * without gamescope only: droiddeck-vortek-focus reports the X windows over
 * $XDG_RUNTIME_DIR/droiddeck-game-fill (game_fill_server.c). The choice and its debounce here have no
 * Wayland or Vulkan: tests/game_fill_test.c. */

#define GF_SOCKET_NAME "droiddeck-game-fill"
#define GF_STEAM_APP 769u
#define GF_MAX_WINDOWS 24
#define GF_CLASS_LEN 32

/* Report wire format, little endian, one SOCK_SEQPACKET message per report: a header of 7 u32 (magic,
 * version, root width, root height, focused window, flags, window count) and per window, bottom to top,
 * u32 id, u32 Steam app (0: none), i32 x, y, width, height, u32 flags (GF_WIN_*) and GF_CLASS_LEN bytes
 * of WM_CLASS ("instance/class", NUL padded). */
#define GF_MAGIC 0x46474444u /* "DDGF" */
#define GF_VERSION 1u
#define GF_HEADER_BYTES 28
#define GF_WINDOW_BYTES (28 + GF_CLASS_LEN)

/* OVERRIDE: override-redirect; INERT: Wine will not activate it; TRANSIENT: has WM_TRANSIENT_FOR;
 * DIALOG: a _NET_WM_WINDOW_TYPE other than normal (dialog, utility, splash, menus, ...). */
enum { GF_WIN_OVERRIDE = 1u, GF_WIN_INERT = 2u, GF_WIN_TRANSIENT = 4u, GF_WIN_DIALOG = 8u };
enum { GF_REPORT_STEAM_MENU = 1u };
enum { GF_MODE_OFF = 0, GF_MODE_STRETCH = 1, GF_MODE_ASPECT = 2 };

/* A window counts when it covers GF_MIN_PERMILLE of the X screen in each direction and at least
 * GF_MIN_W x GF_MIN_H (no more than half the screen); the window shown now keeps counting down to
 * GF_KEEP_PERMILLE. */
#define GF_MIN_PERMILLE 300
#define GF_KEEP_PERMILLE 250
#define GF_MIN_W 640
#define GF_MIN_H 360
/* A new window, or a new rectangle of the window cropped to, is applied once it has stayed GF_SETTLE_MS
 * (the old rectangle of the same window stays meanwhile); the window and place cropped to last come back
 * at once. A crop whose window is no longer the one picked goes at once. Edges that move by up to
 * GF_SLOP px are kept. */
#define GF_SETTLE_MS 500
#define GF_SLOP 2

struct gf_window {
    uint32_t id, app;
    int32_t x, y, w, h;
    uint32_t flags;
    char cls[GF_CLASS_LEN];
};

struct gf_report {
    uint32_t root_w, root_h, focus, flags;
    int n;
    struct gf_window win[GF_MAX_WINDOWS];
};

enum gf_reason {
    GF_CROP = 0,
    GF_OFF_OPTION,
    GF_OFF_NO_HELPER,
    GF_OFF_STEAM,
    GF_OFF_FULLSCREEN,
    GF_OFF_NO_FOCUS,
    GF_OFF_NOT_GAME,
    GF_OFF_SMALL,
};

struct gf_choice {
    int reason;
    uint32_t window, app;
    int index;        /* in the report, -1: none */
    int rect[4];      /* GF_CROP: x, y, w, h in X root px, inside the root */
    int mode;         /* GF_CROP: GF_MODE_STRETCH or GF_MODE_ASPECT */
    char cls[GF_CLASS_LEN];
};

struct gf_state {
    int on;
    struct gf_choice cur;     /* applied while on */
    int has_last;
    struct gf_choice last;    /* the last crop applied */
    int pending;
    struct gf_choice pend;
    int64_t pend_ns;
};

/* 0, or -1 for a malformed report. */
int gf_decode(const void *buf, size_t len, struct gf_report *out);

int gf_big_enough(int w, int h, int root_w, int root_h, int kept);

/* The game window to crop to: the main window of the focused window's game, that is of its managed
 * top-level windows without WM_TRANSIENT_FOR or a dialog-like type and big enough, one with a zero-copy
 * swapchain (`presenting`: top-level windows, n_presenting of them), else the largest. `keep`: the window
 * cropped to now (0: none). out->mode is GF_MODE_STRETCH; the caller sets the game's mode. */
void gf_pick(const struct gf_report *r, uint32_t keep, const uint32_t *presenting, int n_presenting,
             struct gf_choice *out);

/* The option for `app`: its entry in apps/modes, else default_mode. */
int gf_mode_for(int default_mode, const uint32_t *apps, const int *modes, int n, uint32_t app);

/* One step with the choice of now (ns, a monotonic clock). 1 when s->on or s->cur changed. *wake_ns:
 * when to step again with the same report (0: no need). */
int gf_step(struct gf_state *s, const struct gf_choice *c, int64_t now_ns, int64_t *wake_ns);

/* Windows of the report above the crop's window that reach outside its rectangle (cut off by the
 * crop): their indices into idx, at most max. */
int gf_strays(const struct gf_report *r, const struct gf_choice *c, int *idx, int max);

const char *gf_reason_text(int reason);
const char *gf_mode_name(int mode);

#endif
