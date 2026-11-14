#ifndef SC_LAYER_H
#define SC_LAYER_H
#include <stddef.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

struct vkp_image;
struct vkp_draw;
struct droiddeck_color;

/* [mali-g2] SC_VORTEK_LAYERS layers from SC_LAYER_VORTEK on, one per window shown zero-copy. */
enum sc_layer_id { SC_LAYER_GAME = 0, SC_LAYER_OVERLAY = 1, SC_LAYER_VORTEK = 2, SC_VORTEK_LAYERS = 4,
                   SC_LAYER_COUNT = SC_LAYER_VORTEK + SC_VORTEK_LAYERS };
/* z of the lowest Vortek layer: above GAME (1), the others follow; OVERLAY stays above them all. */
#define SC_VORTEK_Z_BASE 2
#define SC_OVERLAY_Z 16

int sc_layer_available(void);

void sc_layer_probe_dmabuf_fd(int fd);

int sc_layer_can_tag_hdr(void);
void sc_layer_hdr_symbols(char *out, size_t size);


int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h, const struct droiddeck_color *color);

int sc_layer_present_pass(const struct vkp_draw *draws, int n, int scene_w, int scene_h);

struct vkp_hdr_frame;
int sc_layer_present_hdr_scene(const struct vkp_draw *draws, int n, const struct vkp_hdr_frame *hf,
                               int scene_w, int scene_h, const struct droiddeck_color *color);

int sc_layer_present_ahb(AHardwareBuffer *ahb, int w, int h, int acquire_fd, void *token,
                         int scene_w, int scene_h, const struct droiddeck_color *color, uint32_t ahb_format);

void sc_layer_set_frame_rate(float fps);
/* The rate the game layers vote for (the session's frame-rate limit, else the panel's), 0: none. */
float sc_layer_frame_rate_want(void);

int sc_layer_overlay_affordable(void);

int sc_layer_present_overlay(struct vkp_image *src, const int geo[8], int translucent);

void sc_layer_hide(void);
/* [mali-g2] Vortek zero-copy: an AHB shown over the X window at x,y,w,h of the scene, on Vortek
 * layer `slot` (0 .. SC_VORTEK_LAYERS - 1) at z `z`. `batch`: a transaction from sc_layer_tx_begin
 * that the caller applies with sc_layer_tx_apply, so changes to several layers reach the screen in
 * the same frame (one buffer per layer and batch); NULL applies a transaction of its own. */
struct ASurfaceTransaction;
struct ASurfaceTransaction *sc_layer_tx_begin(void);
void sc_layer_tx_apply(struct ASurfaceTransaction *batch);
int sc_layer_present_vortek(int slot, int z, AHardwareBuffer *ahb, int buf_w, int buf_h, int acquire_fd, void *token,
                            int x, int y, int w, int h, int scene_w, int scene_h, struct ASurfaceTransaction *batch);
/* [mali-g2] Zero-copy upscaling: Vortek layer `slot` shows buffers of its own pool (SC_VORTEK_POOL,
 * allocated on first use) that the upscaler renders into. pool_take reserves a free buffer of w x h
 * and returns its index, with the display's release fence for it in *wait_fd (-1: none) to wait for
 * on the GPU; -1: none free, -2: allocation failed. A reserved buffer goes back with pool_put (with
 * the fence that must signal before its next use) unless present_vortek_pooled shows it: r[0..3] is
 * the part of the buffer to show, r[4..7] where on the screen; z < 0 keeps the layer's z. pool_trim
 * frees the buffers that are neither shown nor reserved and returns how many are left. */
#define SC_VORTEK_POOL 3
int sc_layer_vortek_pool_take(int slot, int w, int h, struct vkp_image **img, int *wait_fd);
void sc_layer_vortek_pool_put(int slot, int idx, int release_fd);
int sc_layer_vortek_pool_trim(int slot);
int sc_layer_present_vortek_pooled(int slot, int z, int idx, int acquire_fd, const int r[8],
                                   struct ASurfaceTransaction *batch);
/* [mali-g2] ZeroFG (zerofg_zc.c): the pool buffer's AHardwareBuffer (NULL: none); the layer's frame-rate vote
 * in place of the session's from its next buffer (0: the session's again); and a transaction's desired
 * present time (CLOCK_MONOTONIC ns; -1: no ASurfaceTransaction_setDesiredPresentTime). */
AHardwareBuffer *sc_layer_vortek_pool_ahb(int slot, int idx);
void sc_layer_vortek_frame_rate(int slot, float fps);
int sc_layer_can_desire_present(void);
int sc_layer_tx_desired_present(struct ASurfaceTransaction *tx, int64_t when_ns);
void sc_layer_hide_all_vortek(void);
void sc_layer_hide_vortek_unused(unsigned in_use);
/* [mali-g2] Vortek layer `slot`'s ASurfaceControl (NULL before its first frame) and name. */
const void *sc_layer_vortek_surface(int slot, const char **name);
void sc_layer_hide_vortek(int slot, struct ASurfaceTransaction *batch);
void sc_layer_vortek_z(int slot, int z, struct ASurfaceTransaction *batch);
int sc_layer_base_black(int width, int height);
void sc_layer_hide_base(void);
void sc_layer_hide_overlay(void);

void sc_layer_window_gone(void);

unsigned sc_layer_frames_take(void);
unsigned sc_layer_drops_take(void);

#endif
