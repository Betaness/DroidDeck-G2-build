#include "game_fill_server.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "game_fill.h"
#include "vk_present.h"
#include "vortek_present.h"

void droiddeck_scene_map_changed(void);

#define GF_MAX_GAMES 512
#define GF_MAX_STRAYS 16

static struct wl_event_loop *g_loop;
static int g_conn_fd = -1;
static struct wl_event_source *g_conn_src, *g_timer;
static _Atomic int g_wake_fd = -1;
static struct gf_report g_report;
static int g_have_report, g_bad_said;
static struct gf_state g_state;
static uint32_t g_applied_root[2];
static uint32_t g_strays[GF_MAX_STRAYS];
static int g_nstrays;

static pthread_mutex_t g_modes_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_default_mode = GF_MODE_STRETCH;
static uint32_t g_apps[GF_MAX_GAMES];
static int g_modes[GF_MAX_GAMES];
static int g_ngames;
static int g_said_default = -1, g_said_games = -1;

static int valid_mode(int m) { return m == GF_MODE_OFF || m == GF_MODE_STRETCH || m == GF_MODE_ASPECT; }

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int mode_for(uint32_t app) {
    pthread_mutex_lock(&g_modes_lock);
    const int m = gf_mode_for(g_default_mode, g_apps, g_modes, g_ngames, app);
    pthread_mutex_unlock(&g_modes_lock);
    return m;
}

uint32_t game_fill_window(void) { return g_state.on ? g_state.cur.window : 0; }

static const char *class_of(const struct gf_window *w) { return w->cls[0] ? w->cls : "no WM_CLASS"; }

/* Windows above the game window that reach outside it are cut off: each is logged once per crop. */
static void log_strays(void) {
    int idx[GF_MAX_STRAYS];
    const int n = gf_strays(&g_report, &g_state.cur, idx, GF_MAX_STRAYS);
    for (int i = 0; i < n; i++) {
        const struct gf_window *w = &g_report.win[idx[i]];
        int seen = 0;
        for (int j = 0; j < g_nstrays && !seen; j++) seen = g_strays[j] == w->id;
        if (seen) continue;
        if (g_nstrays < GF_MAX_STRAYS) g_strays[g_nstrays++] = w->id;
        droiddeck_log("game-fill", "window 0x%x (%s%s, app %u) at %dx%d%+d%+d reaches outside the game window: "
                      "the part outside is cut off", w->id, class_of(w),
                      w->flags & GF_WIN_OVERRIDE ? ", override-redirect" : "", w->app, w->w, w->h, w->x, w->y);
    }
}

static void apply(int reason) {
    static const int none[4];
    if (g_state.on) {
        const struct gf_choice *c = &g_state.cur;
        vkp_set_scene_crop(1, c->rect, (int)g_report.root_w, (int)g_report.root_h,
                           c->mode == GF_MODE_ASPECT ? VKP_MODE_FIT : VKP_MODE_STRETCH);
        g_applied_root[0] = g_report.root_w;
        g_applied_root[1] = g_report.root_h;
        droiddeck_log("game-fill", "%s: window 0x%x (%s, app %u) %dx%d%+d%+d of the %ux%u X screen %s, the rest cut off",
                      gf_mode_name(c->mode), c->window, c->cls[0] ? c->cls : "no WM_CLASS", c->app, c->rect[2],
                      c->rect[3], c->rect[0], c->rect[1], g_report.root_w, g_report.root_h,
                      c->mode == GF_MODE_ASPECT ? "fitted into the output with black bars" : "stretched over the output");
        g_nstrays = 0;
        log_strays();
    } else {
        vkp_set_scene_crop(0, none, 0, 0, 0);
        droiddeck_log("game-fill", "off: %s", gf_reason_text(reason));
        g_nstrays = 0;
    }
    vortek_present_remap();
    droiddeck_scene_map_changed();
}

static void evaluate(void) {
    struct gf_choice c;
    if (g_conn_fd < 0 || !g_have_report) {
        memset(&c, 0, sizeof(c));
        c.index = -1;
        c.reason = GF_OFF_NO_HELPER;
    } else {
        gf_pick(&g_report, g_state.on ? g_state.cur.window : 0, &c);
        if (c.reason == GF_CROP) {
            const int mode = mode_for(c.app);
            if (mode == GF_MODE_OFF) c.reason = GF_OFF_OPTION;
            else c.mode = mode;
        }
    }
    const int64_t now = now_ns();
    int64_t wake = 0;
    if (gf_step(&g_state, &c, now, &wake)) apply(c.reason);
    else if (g_state.on && (g_applied_root[0] != g_report.root_w || g_applied_root[1] != g_report.root_h)) apply(c.reason);
    else if (g_state.on) log_strays();
    if (g_timer) {
        int ms = 0;
        if (wake) {
            const int64_t d = wake - now;
            ms = d <= 0 ? 1 : (int)((d + 999999) / 1000000);
        }
        wl_event_source_timer_update(g_timer, ms);
    }
}

static void drop_conn(const char *why) {
    if (g_conn_src) wl_event_source_remove(g_conn_src);
    g_conn_src = NULL;
    if (g_conn_fd >= 0) close(g_conn_fd);
    g_conn_fd = -1;
    g_have_report = 0;
    droiddeck_log("game-fill", "window reports stopped: %s", why);
    evaluate();
}

static int on_conn(int fd, uint32_t mask, void *data) {
    (void)data;
    unsigned char buf[GF_HEADER_BYTES + GF_MAX_WINDOWS * GF_WINDOW_BYTES + 1];
    int got = 0;
    for (int i = 0; i < 64; i++) {
        const ssize_t r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (r <= 0) {
            drop_conn(r == 0 ? "the helper closed its connection" : strerror(errno));
            return 0;
        }
        struct gf_report rep;
        if (gf_decode(buf, (size_t)r, &rep) == 0) {
            g_report = rep;
            g_have_report = 1;
            got = 1;
        } else if (!g_bad_said) {
            g_bad_said = 1;
            droiddeck_log("game-fill", "a window report of %zd bytes could not be read: ignored", r);
        }
    }
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        drop_conn("the helper hung up");
        return 0;
    }
    if (got) evaluate();
    return 0;
}

static int on_listen(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    const int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (c < 0) return 0;
    struct ucred cred = {0};
    socklen_t len = sizeof(cred);
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) cred.pid = 0;
    /* One helper: a new connection (the helper started again) replaces the old one; the crop stays
     * until its first report. */
    if (g_conn_src) wl_event_source_remove(g_conn_src);
    if (g_conn_fd >= 0) close(g_conn_fd);
    g_conn_fd = c;
    g_have_report = 0;
    g_conn_src = wl_event_loop_add_fd(g_loop, c, WL_EVENT_READABLE, on_conn, NULL);
    droiddeck_log("game-fill", "window reports from pid %d", (int)cred.pid);
    return 0;
}

static int on_timer(void *data) {
    (void)data;
    evaluate();
    return 0;
}

static int on_wake(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    uint64_t v;
    while (read(fd, &v, sizeof(v)) == (ssize_t)sizeof(v)) {}
    pthread_mutex_lock(&g_modes_lock);
    const int def = g_default_mode, games = g_ngames;
    pthread_mutex_unlock(&g_modes_lock);
    if (def != g_said_default || games != g_said_games) {
        g_said_default = def;
        g_said_games = games;
        droiddeck_log("game-fill", "option: %s, %d game%s with a setting of their own", gf_mode_name(def), games,
                      games == 1 ? "" : "s");
    }
    evaluate();
    return 0;
}

void game_fill_set_modes(int default_mode, const uint32_t *apps, const int *modes, int n) {
    if (!apps || !modes || n < 0) n = 0;
    if (n > GF_MAX_GAMES) n = GF_MAX_GAMES;
    pthread_mutex_lock(&g_modes_lock);
    g_default_mode = valid_mode(default_mode) ? default_mode : GF_MODE_STRETCH;
    g_ngames = 0;
    for (int i = 0; i < n; i++) {
        if (!apps[i] || !valid_mode(modes[i])) continue;
        g_apps[g_ngames] = apps[i];
        g_modes[g_ngames] = modes[i];
        g_ngames++;
    }
    pthread_mutex_unlock(&g_modes_lock);
    const int fd = atomic_load(&g_wake_fd);
    if (fd >= 0) {
        const uint64_t one = 1;
        ssize_t r;
        do r = write(fd, &one, sizeof(one)); while (r < 0 && errno == EINTR);
    }
}

void game_fill_init(struct wl_event_loop *loop) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!loop || !rt || !*rt) return;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", rt, GF_SOCKET_NAME) >= sizeof(addr.sun_path))
        return;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        droiddeck_log("game-fill", "socket %s unavailable: %s", addr.sun_path, strerror(errno));
        close(fd);
        return;
    }
    g_loop = loop;
    wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, on_listen, NULL);
    g_timer = wl_event_loop_add_timer(loop, on_timer, NULL);
    const int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake >= 0) {
        wl_event_loop_add_fd(loop, wake, WL_EVENT_READABLE, on_wake, NULL);
        atomic_store(&g_wake_fd, wake);
        const uint64_t one = 1;
        ssize_t r = write(wake, &one, sizeof(one));
        (void)r;
    }
}
