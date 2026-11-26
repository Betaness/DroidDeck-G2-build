#ifndef GAME_FILL_SERVER_H
#define GAME_FILL_SERVER_H
#include <stdint.h>

struct wl_event_loop;

/* [mali-g2] Game window fill (game_fill.h): serves $XDG_RUNTIME_DIR/droiddeck-game-fill on the
 * compositor's loop and crops the output mapping (vkp_set_scene_crop) to the game window it picks. */
void game_fill_init(struct wl_event_loop *loop);
/* The default mode (GF_MODE_*) and the games (Steam app ids) with a mode of their own. Any thread. */
void game_fill_set_modes(int default_mode, const uint32_t *apps, const int *modes, int n);
/* The top-level X window the output is cropped to, 0: none. Compositor thread. */
uint32_t game_fill_window(void);

#endif
