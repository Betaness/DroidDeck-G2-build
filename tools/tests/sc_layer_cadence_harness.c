/* [mali-g2] Even frame pacing in sc_layer.c with SurfaceControl stubbed out: turning the toggle off
 * forgets the stream on the compositor thread, the 10 s line says why nothing was paced, and what
 * SurfaceFlinger latched is counted. Built by tools/tests/test_present_cadence.py. */
#include "../../app/src/main/cpp/waylandcomp/src/sc_layer.c"
#include "../../app/src/main/cpp/waylandcomp/src/present_cadence.c"
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>

struct AHardwareBuffer { unsigned char pixels[16 * 16 * 4]; };
static struct AHardwareBuffer blanks[2];
static int n_blanks;

struct fake_tx {
    void *ctx[4];
    sc_complete_fn fn[4];
    int n;
    int64_t latch;
};

static char logged[64][512];
static int n_logged;
static int64_t next_latch = -1;

ANativeWindow *vkp_window(void) { return (ANativeWindow *)1; }
void droiddeck_log(const char *kind, const char *fmt, ...) {
    (void)kind;
    va_list ap;
    va_start(ap, fmt);
    if (n_logged < 64) vsnprintf(logged[n_logged++], sizeof(logged[0]), fmt, ap);
    va_end(ap);
}
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **b) {
    (void)d;
    if (n_blanks >= 2) return -1;
    *b = &blanks[n_blanks++];
    return 0;
}
void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *d) {
    (void)b;
    *d = (AHardwareBuffer_Desc){.width = 16, .height = 16, .stride = 16};
}
int AHardwareBuffer_lock(AHardwareBuffer *b, uint64_t usage, int32_t fd, const ARect *r, void **p) {
    (void)usage; (void)fd; (void)r;
    *p = b->pixels;
    return 0;
}
int AHardwareBuffer_unlock(AHardwareBuffer *b, int32_t *f) { (void)b; (void)f; return 0; }
void AHardwareBuffer_release(AHardwareBuffer *b) { (void)b; }

static ASurfaceControl *create(ANativeWindow *w, const char *n) { (void)w; (void)n; return (ASurfaceControl *)7; }
static ASurfaceTransaction *tx_create(void) {
    struct fake_tx *t = calloc(1, sizeof(*t));
    t->latch = next_latch;
    return (ASurfaceTransaction *)t;
}
static void tx_delete(ASurfaceTransaction *t) { free(t); }
static void tx_apply(ASurfaceTransaction *tx) {
    struct fake_tx *t = (struct fake_tx *)tx;
    for (int i = 0; i < t->n; i++) t->fn[i](t->ctx[i], (ASurfaceTransactionStats *)t);
    t->n = 0;
}
static void on_complete_set(ASurfaceTransaction *tx, void *ctx, sc_complete_fn fn) {
    struct fake_tx *t = (struct fake_tx *)tx;
    assert(t->n < 4);
    t->ctx[t->n] = ctx;
    t->fn[t->n++] = fn;
}
static int64_t latch_of(ASurfaceTransactionStats *stats) { return ((struct fake_tx *)stats)->latch; }
static void set_buffer(ASurfaceTransaction *t, ASurfaceControl *s, AHardwareBuffer *b, int fd) { (void)t; (void)s; (void)b; (void)fd; }
static void visibility(ASurfaceTransaction *t, ASurfaceControl *s, int8_t v) { (void)t; (void)s; (void)v; }
static void z(ASurfaceTransaction *t, ASurfaceControl *s, int32_t v) { (void)t; (void)s; (void)v; }
static void transparency(ASurfaceTransaction *t, ASurfaceControl *s, int8_t v) { (void)t; (void)s; (void)v; }
static void geometry(ASurfaceTransaction *t, ASurfaceControl *s, const ARect *src, const ARect *dst, int32_t r) {
    (void)t; (void)s; (void)src; (void)dst; (void)r;
}
static void reparent(ASurfaceTransaction *t, ASurfaceControl *s, ASurfaceControl *p) { (void)t; (void)s; (void)p; }
static void release(ASurfaceControl *s) { (void)s; }

static int logged_with(const char *text) {
    int n = 0;
    for (int i = 0; i < n_logged; i++) n += strstr(logged[i], text) != NULL;
    return n;
}

static int open_fd(void) {
    int fds[2];
    assert(pipe(fds) == 0);
    close(fds[1]);
    return fds[0];
}

#define P144 6944444LL

static void sleep_until(int64_t t) {
    const struct timespec ts = {(time_t)(t / 1000000000LL), (long)(t % 1000000000LL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* count frames gap_refreshes apart in real time (the timeline goes stale after 100 ms), each latched
 * 3 refreshes after it arrived. Returns the last frame's fence. */
static int feed(int count, int64_t gap_refreshes) {
    static int64_t grid;
    int last_fd = -1;
    int64_t at = now_ns();
    if (!grid) grid = at + 20 * P144;
    for (int i = 0; i < count; i++, at += gap_refreshes * P144) {
        sleep_until(at);
        sc_layer_frame_timeline(grid, grid - 7000000LL, P144);
        next_latch = at + 3 * P144;
        last_fd = open_fd();
        sc_layer_tx_apply_frame(tx_create(), at, last_fd);
    }
    next_latch = -1;
    return last_fd;
}

int main(void) {
    api.state = 1;
    api.createFromWindow = create; api.txCreate = tx_create; api.txDelete = tx_delete; api.txApply = tx_apply;
    api.setBuffer = set_buffer; api.setVisibility = visibility; api.setZOrder = z;
    api.setBufferTransparency = transparency; api.setGeometry = geometry; api.reparent = reparent;
    api.release = release; api.setOnComplete = on_complete_set; api.latchTime = latch_of;

    /* Toggle on, nothing reaches the planner: the line says why, once. */
    sc_layer_set_present_cadence(1);
    sc_layer_cadence_log();
    sc_layer_cadence_log();
    assert(logged_with("even frame pacing waits: no frame of the game's main window came on a zero-copy layer") == 1);
    sc_layer_cadence_note_unpaced(SC_CADENCE_ZEROFG);
    sc_layer_cadence_log();
    assert(logged_with("even frame pacing waits: ZeroFG shows the game's frames") == 1);
    sc_layer_cadence_note_unpaced(SC_CADENCE_FILTER);
    sc_layer_cadence_log();
    assert(logged_with("even frame pacing waits: the game's frames go through the scaling filter") == 1);

    /* A steady 2-refresh stream is paced; the latch times show it 2 refreshes apart. */
    const int kept = feed(12, 2);
    assert(g_cadence.pacing && g_cadence.cadence == 2);
    assert(g_ready_fd == kept && fcntl(kept, F_GETFD) != -1);
    assert(logged_with("even frame pacing: game frames held to a 2-refresh cadence") == 1);
    pthread_mutex_lock(&g_pacer_lock);
    pacer_flush_locked();
    pthread_mutex_unlock(&g_pacer_lock);
    n_logged = 0;
    sc_layer_cadence_log();
    assert(logged_with("frames held to a 2-refresh cadence") == 1);
    assert(logged_with("shown 0/0/11/0/0 refreshes after the frame before") == 1);

    /* Toggle off: the compositor thread forgets the stream and closes the kept fence at its next line. */
    sc_layer_set_present_cadence(0);
    assert(g_ready_fd == kept);
    n_logged = 0;
    sc_layer_cadence_log();
    assert(g_ready_fd == -1 && fcntl(kept, F_GETFD) == -1);
    assert(!g_cadence.pacing && g_cadence.n_gaps == 0 && g_cadence.arrival_ref_ns == 0 && g_cadence.n_waits == 0);
    assert(g_cadence_said == -1 && g_shown_last_ns == 0);

    /* On again: the first frame is not reported as leaving a cadence. */
    sc_layer_set_present_cadence(1);
    n_logged = 0;
    feed(1, 2);
    assert(!logged_with("left a whole-refresh cadence"));
    assert(!g_cadence.pacing);

    /* Off and on again before any frame or line: the next frame still starts over. */
    feed(10, 2);
    assert(g_cadence.pacing);
    sc_layer_set_present_cadence(0);
    sc_layer_set_present_cadence(1);
    n_logged = 0;
    feed(1, 2);
    assert(!g_cadence.pacing && g_cadence.n_gaps == 0);
    assert(!logged_with("left a whole-refresh cadence"));

    /* A held frame whose fence signalled after its refresh's deadline was ready too late. */
    memset(&g_cadence_stats, 0, sizeof(g_cadence_stats));
    const int64_t t = now_ns();
    g_ready_fd = open_fd(); g_ready_ref_ns = t - 50000000LL; g_ready_deadline_ns = t - 10000000LL;
    zero_copy_ready(0, -1, 0);
    assert(g_cadence_stats.late == 1 && g_ready_fd == -1);
    g_ready_fd = open_fd(); g_ready_ref_ns = t - 5000000LL; g_ready_deadline_ns = t + 1000000000LL;
    zero_copy_ready(0, -1, 0);
    assert(g_cadence_stats.late == 1 && g_ready_fd == -1);

    /* SurfaceFlinger latches at a refresh's deadline: a held frame latched a refresh after the deadline
     * of its planned refresh missed it, one latched just after it did not; an unheld frame is not counted. */
    const int64_t deadline = 5000000000LL;
    const struct { int64_t deadline, latch; unsigned late; } latches[] = {
        {deadline, deadline + 1000000LL, 0}, {deadline, deadline + P144 + 500000LL, 1}, {0, deadline + 9 * P144, 1},
    };
    g_shown_late = 0;
    for (unsigned i = 0; i < sizeof(latches) / sizeof(latches[0]); i++) {
        struct fake_tx stats = {.latch = latches[i].latch};
        struct frame_ctx *f = calloc(1, sizeof(*f));
        f->deadline_ns = latches[i].deadline;
        f->period_ns = P144;
        on_frame_complete(f, (ASurfaceTransactionStats *)&stats);
        assert(g_shown_late == latches[i].late);
    }
    sc_layer_set_present_cadence(0);
    sc_layer_cadence_log();
    puts("PASS");
    return 0;
}
