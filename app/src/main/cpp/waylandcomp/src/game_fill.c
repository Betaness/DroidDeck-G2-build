#include "game_fill.h"

#include <string.h>

#define GF_MAX_DIM 32768

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int gf_decode(const void *buf, size_t len, struct gf_report *out) {
    const unsigned char *p = buf;
    if (!buf || !out || len < GF_HEADER_BYTES) return -1;
    if (rd32(p) != GF_MAGIC || rd32(p + 4) != GF_VERSION) return -1;
    const uint32_t root_w = rd32(p + 8), root_h = rd32(p + 12), n = rd32(p + 24);
    if (root_w > GF_MAX_DIM || root_h > GF_MAX_DIM || n > GF_MAX_WINDOWS) return -1;
    if (len != GF_HEADER_BYTES + (size_t)n * GF_WINDOW_BYTES) return -1;
    memset(out, 0, sizeof(*out));
    out->root_w = root_w;
    out->root_h = root_h;
    out->focus = rd32(p + 16);
    out->flags = rd32(p + 20);
    out->n = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *q = p + GF_HEADER_BYTES + (size_t)i * GF_WINDOW_BYTES;
        struct gf_window *w = &out->win[i];
        w->id = rd32(q);
        w->app = rd32(q + 4);
        w->x = (int32_t)rd32(q + 8);
        w->y = (int32_t)rd32(q + 12);
        w->w = (int32_t)rd32(q + 16);
        w->h = (int32_t)rd32(q + 20);
        w->flags = rd32(q + 24);
        memcpy(w->cls, q + 28, GF_CLASS_LEN);
        w->cls[GF_CLASS_LEN - 1] = 0;
        if (w->w < 0 || w->h < 0 || w->w > GF_MAX_DIM || w->h > GF_MAX_DIM || w->x < -GF_MAX_DIM ||
            w->x > GF_MAX_DIM || w->y < -GF_MAX_DIM || w->y > GF_MAX_DIM)
            return -1;
    }
    return 0;
}

int gf_big_enough(int w, int h, int root_w, int root_h, int kept) {
    if (root_w <= 0 || root_h <= 0) return 0;
    const long long permille = kept ? GF_KEEP_PERMILLE : GF_MIN_PERMILLE;
    long long need_w = root_w * permille / 1000, need_h = root_h * permille / 1000;
    const long long floor_w = GF_MIN_W < root_w / 2 ? GF_MIN_W : root_w / 2;
    const long long floor_h = GF_MIN_H < root_h / 2 ? GF_MIN_H : root_h / 2;
    if (need_w < floor_w) need_w = floor_w;
    if (need_h < floor_h) need_h = floor_h;
    return w >= need_w && h >= need_h;
}

/* The part of w on the root: x0, y0, x1, y1. 0 when none. */
static int on_root(const struct gf_window *w, int root_w, int root_h, long long r[4]) {
    r[0] = w->x < 0 ? 0 : w->x;
    r[1] = w->y < 0 ? 0 : w->y;
    r[2] = (long long)w->x + w->w;
    r[3] = (long long)w->y + w->h;
    if (r[2] > root_w) r[2] = root_w;
    if (r[3] > root_h) r[3] = root_h;
    return r[2] > r[0] && r[3] > r[1];
}

static int managed(const struct gf_window *w) { return !(w->flags & GF_WIN_OVERRIDE); }

/* A window that can be the game's main window: not a dialog, menu or splash, not owned by another. */
static int main_kind(const struct gf_window *w) {
    return !(w->flags & (GF_WIN_OVERRIDE | GF_WIN_TRANSIENT | GF_WIN_DIALOG));
}

static long long area_on_root(const struct gf_window *w, int root_w, int root_h) {
    long long r[4];
    return on_root(w, root_w, root_h, r) ? (r[2] - r[0]) * (r[3] - r[1]) : 0;
}

static int counts(const struct gf_window *w, int root_w, int root_h, uint32_t keep) {
    long long r[4];
    return on_root(w, root_w, root_h, r) &&
           gf_big_enough((int)(r[2] - r[0]), (int)(r[3] - r[1]), root_w, root_h, keep && w->id == keep);
}

static int listed(uint32_t id, const uint32_t *ids, int n) {
    for (int i = 0; ids && i < n; i++)
        if (ids[i] == id) return 1;
    return 0;
}

static void name_window(struct gf_choice *out, const struct gf_report *r, int i) {
    out->window = r->win[i].id;
    out->index = i;
    memcpy(out->cls, r->win[i].cls, GF_CLASS_LEN);
}

void gf_pick(const struct gf_report *r, uint32_t keep, const uint32_t *presenting, int n_presenting,
             struct gf_choice *out) {
    memset(out, 0, sizeof(*out));
    out->index = -1;
    out->mode = GF_MODE_STRETCH;
    out->reason = GF_OFF_NO_FOCUS;
    if (!r || r->n <= 0 || r->root_w == 0 || r->root_h == 0) return;
    if (r->flags & GF_REPORT_STEAM_MENU) {
        out->reason = GF_OFF_STEAM;
        return;
    }
    const int RW = (int)r->root_w, RH = (int)r->root_h, n = r->n < GF_MAX_WINDOWS ? r->n : GF_MAX_WINDOWS;
    int f = -1;
    for (int i = n - 1; i >= 0 && f < 0 && r->focus; i--)
        if (managed(&r->win[i]) && r->win[i].id == r->focus) f = i;
    for (int i = n - 1; i >= 0 && f < 0; i--)
        if (managed(&r->win[i]) && !(r->win[i].flags & GF_WIN_INERT)) f = i;
    for (int i = n - 1; i >= 0 && f < 0; i--)
        if (managed(&r->win[i])) f = i;
    if (f < 0) return;
    const uint32_t app = r->win[f].app;
    out->app = app;
    name_window(out, r, f);
    if (app == GF_STEAM_APP) {
        out->reason = GF_OFF_STEAM;
        return;
    }
    if (!app) {
        out->reason = GF_OFF_NOT_GAME;
        return;
    }
    /* From the top: of equal ones the upper wins. */
    int pick = -1, pick_presents = 0;
    long long pick_area = 0;
    for (int i = n - 1; i >= 0; i--) {
        const struct gf_window *w = &r->win[i];
        if (w->app != app || !main_kind(w) || !counts(w, RW, RH, keep)) continue;
        const int presents = listed(w->id, presenting, n_presenting);
        const long long area = area_on_root(w, RW, RH);
        if (pick < 0 || presents > pick_presents || (presents == pick_presents && area > pick_area)) {
            pick = i;
            pick_presents = presents;
            pick_area = area;
        }
    }
    if (pick < 0) {
        out->reason = GF_OFF_SMALL;
        return;
    }
    name_window(out, r, pick);
    long long v[4];
    on_root(&r->win[pick], RW, RH, v);
    out->rect[0] = (int)v[0];
    out->rect[1] = (int)v[1];
    out->rect[2] = (int)(v[2] - v[0]);
    out->rect[3] = (int)(v[3] - v[1]);
    out->reason = v[0] <= 0 && v[1] <= 0 && v[2] >= RW && v[3] >= RH ? GF_OFF_FULLSCREEN : GF_CROP;
}

int gf_mode_for(int default_mode, const uint32_t *apps, const int *modes, int n, uint32_t app) {
    for (int i = 0; app && apps && modes && i < n; i++)
        if (apps[i] == app) return modes[i];
    return default_mode;
}

static int near(long long a, long long b) { return a - b <= GF_SLOP && b - a <= GF_SLOP; }

static int same_place(const struct gf_choice *a, const struct gf_choice *b) {
    return a->window == b->window && near(a->rect[0], b->rect[0]) && near(a->rect[1], b->rect[1]) &&
           near((long long)a->rect[0] + a->rect[2], (long long)b->rect[0] + b->rect[2]) &&
           near((long long)a->rect[1] + a->rect[3], (long long)b->rect[1] + b->rect[3]);
}

int gf_step(struct gf_state *s, const struct gf_choice *c, int64_t now_ns, int64_t *wake_ns) {
    const int64_t settle = GF_SETTLE_MS * 1000000LL;
    *wake_ns = 0;
    if (c->reason != GF_CROP) {
        s->pending = 0;
        if (!s->on) return 0;
        s->on = 0;
        return 1;
    }
    if (s->on && same_place(&s->cur, c)) {
        s->pending = 0;
        s->cur.index = c->index;
        if (c->mode == s->cur.mode) return 0;
        s->cur.mode = c->mode;
        s->last = s->cur;
        return 1;
    }
    /* Another window: what was cropped to is gone or no longer the game window. */
    int changed = 0;
    if (s->on && s->cur.window != c->window) {
        s->on = 0;
        changed = 1;
    }
    /* Back to the window and place shown last (after a Steam menu, out of fullscreen): no settling. */
    if (!(s->has_last && same_place(&s->last, c))) {
        if (!s->pending || !same_place(&s->pend, c)) {
            s->pending = 1;
            s->pend = *c;
            s->pend_ns = now_ns;
        }
        if (now_ns - s->pend_ns < settle) {
            *wake_ns = s->pend_ns + settle;
            return changed;
        }
    }
    s->on = 1;
    s->cur = *c;
    s->last = *c;
    s->has_last = 1;
    s->pending = 0;
    return 1;
}

int gf_strays(const struct gf_report *r, const struct gf_choice *c, int *idx, int max) {
    if (!r || !c || c->reason != GF_CROP || c->index < 0 || c->index >= r->n) return 0;
    const long long x0 = c->rect[0], y0 = c->rect[1], x1 = x0 + c->rect[2], y1 = y0 + c->rect[3];
    int n = 0;
    for (int i = c->index + 1; i < r->n && n < max; i++) {
        long long v[4];
        if (r->win[i].id == c->window || !on_root(&r->win[i], (int)r->root_w, (int)r->root_h, v)) continue;
        if (v[0] < x0 || v[1] < y0 || v[2] > x1 || v[3] > y1) idx[n++] = i;
    }
    return n;
}

const char *gf_reason_text(int reason) {
    switch (reason) {
    case GF_CROP: return "a game window smaller than the screen";
    case GF_OFF_OPTION: return "the option is off for this game";
    case GF_OFF_NO_HELPER: return "no window reports from droiddeck-vortek-focus";
    case GF_OFF_STEAM: return "Steam is in front";
    case GF_OFF_FULLSCREEN: return "the game window covers the screen";
    case GF_OFF_NO_FOCUS: return "no window in front";
    case GF_OFF_NOT_GAME: return "the window in front is no Steam game's";
    case GF_OFF_SMALL: return "the game's windows are too small to fill the screen with";
    default: return "?";
    }
}

const char *gf_mode_name(int mode) {
    switch (mode) {
    case GF_MODE_STRETCH: return "stretch";
    case GF_MODE_ASPECT: return "keep aspect";
    case GF_MODE_OFF: return "off";
    default: return "?";
    }
}
