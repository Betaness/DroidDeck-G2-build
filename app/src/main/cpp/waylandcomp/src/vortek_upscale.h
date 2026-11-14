#ifndef VORTEK_UPSCALE_H
#define VORTEK_UPSCALE_H
#include <stddef.h>
#include <stdint.h>

/* [mali-g2] Spatial upscaling on Vortek zero-copy layers: when a scaling filter is selected, the
 * game's frame (an AHardwareBuffer) goes through one fragment pass into a buffer of the layer's own
 * pool, sized to the window on the screen (Sharpen: to the frame), and that buffer is shown instead.
 * The game's image goes back as soon as the pass has read it. Event-loop thread only. */

struct AHardwareBuffer;
struct ASurfaceTransaction;
struct wl_event_loop;

struct vku_frame {
    int slot, z;                 /* Vortek layer and its z */
    uint32_t chain, index;
    struct AHardwareBuffer *ahb;
    int buf_w, buf_h;
    int x, y, w, h;              /* the window in scene coordinates */
    int scene_w, scene_h;
    int main_window;             /* the largest zero-copy window, covering at least half of the X root */
};

enum {
    VKU_SHOWN = 0,   /* the upscaled frame is in the batch */
    VKU_DIRECT = 1,  /* not upscaled: acquire_fd untouched, the caller shows the game's buffer */
    VKU_TAKEN = 2,   /* upscaled later (when the frame is ready) or dropped: nothing in the batch */
};
/* SHOWN and TAKEN consume acquire_fd and send the game image's release. DIRECT with a filter
 * selected sets *why (static text). */
int vku_present(const struct vku_frame *f, int acquire_fd, struct ASurfaceTransaction *batch, const char **why);

void vku_init(struct wl_event_loop *loop);
/* The layer is hidden or goes to another window: a frame still waiting for its game is dropped. */
void vku_layer_hidden(int slot);
/* Whether layer `slot` has a frame upscaled once its fences signal (vku_present returned VKU_TAKEN
 * without dropping it); it is shown unless the next frame for the layer overtakes it. */
int vku_pending(int slot);
void vku_chain_gone(uint32_t chain);
/* Waits for every pass in flight (the layer pools are about to go). */
void vku_idle(void);
/* From the 10 s stats timer: frees pools that showed no upscaled frame for a while. */
void vku_tick(void);
/* " | zero-copy upscale ..." for the perf line, or "" when nothing was upscaled; resets the counters. */
void vku_perf_take(char *out, size_t n);
/* [mali-g2] For ZeroFG (zerofg_zc.c): whether a filter is selected that zero-copy upscaling runs (its name in
 * *name), and whether layer `slot` showed an upscaled frame lately. */
int vku_filter_selected(const char **name);
int vku_layer_recent(int slot);

#endif
