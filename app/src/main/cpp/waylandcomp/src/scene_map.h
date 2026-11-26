#ifndef SCENE_MAP_H
#define SCENE_MAP_H

/* [mali-g2] How the scene (the X screen) is placed on the output. No Vulkan or Wayland:
 * tests/scene_map_test.c. The values match vk_present.h's vkp_scale_mode and vkp_align. */

enum { SCENE_MODE_OFF = 0, SCENE_MODE_FIT = 1, SCENE_MODE_STRETCH = 2, SCENE_MODE_FILL = 3, SCENE_MODE_INTEGER = 4 };
enum { SCENE_ALIGN_CENTER = 0, SCENE_ALIGN_TOP = 1, SCENE_ALIGN_BOTTOM = 2, SCENE_ALIGN_PICTURE_TOP = 3,
       SCENE_ALIGN_PICTURE_BOTTOM = 4, SCENE_ALIGN_PICTURE_LEFT = 5, SCENE_ALIGN_PICTURE_RIGHT = 6 };

struct scene_map {
    int scene_w, scene_h, out_w, out_h, mode, align;
    /* The part of the scene that is shown: all of it unless crop is set. */
    int crop, crop_x, crop_y, crop_w, crop_h, crop_mode;
    /* output px = scene px * k + off */
    float kx, ky, off_x, off_y;
    /* The output region the picture is placed in. */
    int rx, ry, rw, rh;
    /* left, top, right, bottom in output px: nothing is drawn outside. */
    float clip[4];
    /* Where the shown part of the scene lands on the output. */
    float picture[4];
    int valid;
};

/* crop: x, y, w, h in scene px, or NULL for the whole scene. A crop is shown stretched over the region
 * (crop_mode SCENE_MODE_STRETCH) or fitted into it with bars (any other mode), and the scene outside it
 * is clipped away. Without a crop the scene is placed by mode and align. */
void scene_map_compute(struct scene_map *m, int scene_w, int scene_h, int out_w, int out_h, int mode, int align,
                       const int crop[4], int crop_mode);

int scene_map_to_output(const struct scene_map *m, double sx, double sy, double *ox, double *oy);
int scene_map_to_scene(const struct scene_map *m, double ox, double oy, double *sx, double *sy);

/* rect (x, y, w, h in px of a root_w x root_h X screen) in scene px, clipped to the scene. 0 when nothing
 * of it is on the scene. A root size of 0 means the X screen is the scene. */
int scene_map_root_rect(const int rect[4], int root_w, int root_h, int scene_w, int scene_h, int out[4]);

#endif
