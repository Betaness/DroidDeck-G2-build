#ifndef ZEROFG_ZC_H
#define ZEROFG_ZC_H
#include <stddef.h>
#include <stdint.h>

/* [mali-g2] ZeroFG frame generation on the zero-copy main window (../ZEROFG.md). For each real frame B of
 * the main window it generates the midpoint S of the previous real frame A and B into a buffer of the
 * layer's pool and shows S, then B (the game's own buffer). Runs on a VkDevice of its own. Event-loop
 * thread unless said otherwise. */

struct vku_frame;
struct ASurfaceTransaction;
struct wl_event_loop;

enum {
    ZFG_DIRECT = 0, /* not handled: acquire_fd untouched, the caller shows the frame */
    ZFG_SHOWN = 1,  /* B (and S before it) went, or go at their times, to the display; acquire_fd consumed */
    ZFG_TAKEN = 2,  /* B could not be shown: acquire_fd consumed, the game image released */
};
/* What goes to the display with this drain goes into vortek_present's batch; ZeroFG applies the batch first
 * when it shows frames in transactions of its own. S and B wait on the event loop for their times (a timer,
 * S's completion); the next frame for the layer, hiding it or its swapchain going settles them first. */
int zfg_present(const struct vku_frame *f, int acquire_fd, void *token);

void zfg_init(struct wl_event_loop *loop);
void zfg_layer_hidden(int slot);
void zfg_chain_gone(uint32_t chain);
/* The pools are about to go: drops the imports of pool buffers, those still written once their jobs are done;
 * the frames waiting for their time never reach the screen. Does not wait. */
void zfg_idle(void);
/* From the 10 s stats timer: frees the engine and the imports after a while without use, and everything,
 * the device too, once ZeroFG is off for the session; never while a generation still runs. */
void zfg_tick(void);
/* Whether ZeroFG wrote into layer `slot`'s pool lately (vku_tick keeps that pool). */
int zfg_layer_recent(int slot);

/* Any thread. The game image's release fence with the end of the generations that read it merged in;
 * consumes fence_fd. */
int zfg_release_fence(uint32_t chain, uint32_t index, int fence_fd);

/* The compositor's device is up: checks what ZeroFG needs on its GPU. */
void zfg_device_ready(void);
/* Any thread. Why ZeroFG cannot run here; NULL while nothing says so. zfg_checked: the GPU was checked. */
const char *zfg_unsupported(void);
int zfg_checked(void);
/* Any thread. ZFG_MODE_* (zerofg_glue.h), from the menu. */
void zfg_set_mode(int mode);
/* Any thread. Real frames and real + generated frames per second over the last stats window (0: off). */
void zfg_rates(float *real, float *shown);
/* Any thread. The highest refresh rate the session lets the panel run at (0: not given; the refresh rate). */
void zfg_set_peak_hz(float hz);
/* Any thread. A new game session: ZeroFG, off after a failure, may run again; the failure is forgotten. */
void zfg_new_session(void);
/* Any thread. Why ZeroFG stopped in this session, or the last size or mode it could not be built for: 1 with
 * the text in out; 0: nothing. */
int zfg_failure(char *out, size_t n);

#endif
