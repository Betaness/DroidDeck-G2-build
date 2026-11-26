#include "pointer_sync.h"

int ptrsync_motion(int locked, int relative, int rel_consumed, int moved, int desktop) {
    if (locked) return 0;
    if (relative && rel_consumed) return desktop ? 0 : PTRSYNC_BEHIND;
    if (!relative || moved) return PTRSYNC_MOTION;
    return 0;
}

int ptrsync_catch_up(int locked, int behind) {
    return !locked && behind;
}

int ptrsync_scene_map(int shown, double o0x, double o0y, double o1x, double o1y, int ow, int oh,
                      int scene_w, int scene_h, double map[4]) {
    if (scene_w <= 0 || scene_h <= 0) return 0;
    if (shown && ow > 0 && oh > 0 && o1x > o0x && o1y > o0y) {
        map[0] = (o1x - o0x) / ow;
        map[1] = o0x / ow;
        map[2] = (o1y - o0y) / oh;
        map[3] = o0y / oh;
    } else {
        map[0] = 1.0 / scene_w;
        map[1] = 0;
        map[2] = 1.0 / scene_h;
        map[3] = 0;
    }
    return 1;
}

int ptrsync_confine_box(int ox, int oy, int w, int h, int region_set, int rx, int ry, int rw, int rh, double box[4]) {
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (region_set) {
        if ((long long)rx + rw < x1) x1 = rx + rw;
        if ((long long)ry + rh < y1) y1 = ry + rh;
        if (rx > x0) x0 = rx;
        if (ry > y0) y0 = ry;
    }
    if (x1 - x0 <= 0 || y1 - y0 <= 0) return 0;
    box[0] = ox + x0;
    box[1] = oy + y0;
    box[2] = ox + x1 - 1;
    box[3] = oy + y1 - 1;
    return 1;
}

void ptrsync_box_within(const double box[4], const double within[4], double out[4]) {
    double r[4];
    for (int a = 0; a < 2; a++) {
        const double b0 = box[a], b1 = box[a + 2], w0 = within[a], w1 = within[a + 2];
        if (b1 < w0) r[a] = r[a + 2] = w0;
        else if (b0 > w1) r[a] = r[a + 2] = w1;
        else {
            r[a] = b0 > w0 ? b0 : w0;
            r[a + 2] = b1 < w1 ? b1 : w1;
        }
    }
    for (int i = 0; i < 4; i++) out[i] = r[i];
}
