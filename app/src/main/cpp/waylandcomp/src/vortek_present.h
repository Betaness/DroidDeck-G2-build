#ifndef VORTEK_PRESENT_H
#define VORTEK_PRESENT_H
#include <stdint.h>
#include <wayland-server-core.h>

/* [mali-g2] Zero-copy presentation for Vortek (Vulkan forwarded to the Mali driver). The Vortek
 * server renders guest swapchains into AHardwareBuffers and hands them, with acquire fences, to
 * the compositor over $XDG_RUNTIME_DIR/vortek-present; they go straight to a SurfaceControl layer
 * placed over the X window. Wire format shared with mali-g2/vortek/src/headless_swapchain.c. */

#define VORTEK_PRESENT_SOCKET_NAME "vortek-present"

enum {
    VP_MSG_CHAIN_CREATE = 1,  /* server -> compositor, followed by image_count AHB handles */
    VP_MSG_PRESENT = 2,       /* server -> compositor, SCM_RIGHTS: acquire fence or none */
    VP_MSG_HIDE = 3,          /* server -> compositor */
    VP_MSG_CHAIN_DESTROY = 4, /* server -> compositor */
    VP_MSG_RELEASE = 5,       /* compositor -> server, SCM_RIGHTS: release fence or none */
    VP_MSG_REFUSED = 6,       /* compositor -> server: never present this chain zero-copy */
    VP_MSG_MODE = 7,          /* compositor -> server: index 1 accepts presents, 0 pauses them */
    VP_MSG_RESTACK = 8,       /* server -> compositor: a shown chain's stacking key (key_hi/key_lo) changed */
};

struct vp_msg {
    uint32_t type;
    uint32_t chain;
    uint32_t index;
    uint32_t image_count;
    uint32_t width, height;   /* buffer size */
    int32_t x, y, w, h;       /* window rectangle in X root coordinates */
    uint32_t root_w, root_h;  /* X root size, which Xwayland may scale to the scene */
    uint32_t key_hi, key_lo;  /* PRESENT: X stacking key; a higher key gets a higher layer */
    uint32_t toplevel;        /* PRESENT, RESTACK: the window's top-level X window, 0 if unknown */
};

void vortek_present_init(struct wl_event_loop *loop);
void vortek_present_scene_size(int w, int h);
/* Scene effects, frame generation and cardboard need the composited scene: pause zero-copy. */
void vortek_present_set_accept(int accept);
unsigned vortek_present_stats_take(void);
/* From sc_layer's transaction-complete callback (a binder thread). */
void vortek_present_released(void *token, int release_fd);
/* VP_MSG_RELEASE for a game image with its release fence (consumed; -1: none). Any thread. */
void vortek_present_release(uint32_t chain, uint32_t index, int fence_fd);
/* Applies the layer changes batched so far in this drain (ZeroFG shows frames in transactions of its own after them). */
void vortek_present_batch_flush(void);
/* This drain's batch as it is now (NULL outside a drain); a flush ends it, so it is not kept across one. */
struct ASurfaceTransaction;
struct ASurfaceTransaction *vortek_present_batch(void);
/* Inside the compositor a chain is a key that also names its presenter (vortek_present.c); the
 * presenter's own id, as its log lines give it. */
#define VP_CHAIN_ID(key) ((uint32_t)(key) & 0xffffffu)

#endif
