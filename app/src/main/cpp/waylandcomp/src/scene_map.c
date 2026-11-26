#include "scene_map.h"

#include <string.h>

void scene_map_compute(struct scene_map *m, int scene_w, int scene_h, int W, int H, int mode, int align,
                       const int crop[4], int crop_mode) {
    memset(m, 0, sizeof(*m));
    m->scene_w = scene_w; m->scene_h = scene_h; m->out_w = W; m->out_h = H;
    m->mode = mode; m->align = align;
    if (scene_w <= 0 || scene_h <= 0 || W <= 0 || H <= 0) return;

    int cx = 0, cy = 0, cw = scene_w, ch = scene_h;
    if (crop && crop[2] > 0 && crop[3] > 0) {
        m->crop = 1;
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
        mode = crop_mode == SCENE_MODE_STRETCH ? SCENE_MODE_STRETCH : SCENE_MODE_FIT;
        m->crop_mode = mode;
    }
    m->crop_x = cx; m->crop_y = cy; m->crop_w = cw; m->crop_h = ch;

    const int half = H / 2;
    switch (align) {
    case SCENE_ALIGN_TOP:    m->rx = 0; m->ry = 0;    m->rw = W; m->rh = half; break;
    case SCENE_ALIGN_BOTTOM: m->rx = 0; m->ry = half; m->rw = W; m->rh = H - half; break;
    default:                 m->rx = 0; m->ry = 0;    m->rw = W; m->rh = H; break;
    }
    const float sx = (float)m->rw / cw, sy = (float)m->rh / ch;
    float ox, oy;
    if (mode == SCENE_MODE_STRETCH) {
        m->kx = sx; m->ky = sy;
        ox = (float)m->rx; oy = (float)m->ry;
    } else {
        float aspect;
        if (mode == SCENE_MODE_FILL) aspect = sx > sy ? sx : sy;
        else if (mode == SCENE_MODE_INTEGER) {
            float k = sx < sy ? sx : sy;
            aspect = (float)(int)k;
            if (aspect < 1.0f) aspect = 1.0f;
        } else aspect = sx < sy ? sx : sy;
        m->kx = m->ky = aspect;
        ox = (float)(m->rx + (int)((m->rw - cw * aspect) * 0.5f));
        oy = (float)(m->ry + (int)((m->rh - ch * aspect) * 0.5f));
    }
    if (align == SCENE_ALIGN_PICTURE_TOP) oy = 0;
    else if (align == SCENE_ALIGN_PICTURE_BOTTOM) oy = H - ch * m->ky;
    else if (align == SCENE_ALIGN_PICTURE_LEFT) ox = 0;
    else if (align == SCENE_ALIGN_PICTURE_RIGHT) ox = W - cw * m->kx;

    m->picture[0] = ox;
    m->picture[1] = oy;
    m->picture[2] = ox + cw * m->kx;
    m->picture[3] = oy + ch * m->ky;
    m->off_x = ox - cx * m->kx;
    m->off_y = oy - cy * m->ky;

    float L = (float)m->rx, T = (float)m->ry, R = (float)(m->rx + m->rw), B = (float)(m->ry + m->rh);
    if (R > (float)W) R = (float)W;
    if (B > (float)H) B = (float)H;
    if (m->crop) {
        if (m->picture[0] > L) L = m->picture[0];
        if (m->picture[1] > T) T = m->picture[1];
        if (m->picture[2] < R) R = m->picture[2];
        if (m->picture[3] < B) B = m->picture[3];
    }
    m->clip[0] = L; m->clip[1] = T; m->clip[2] = R; m->clip[3] = B;
    m->valid = 1;
}

int scene_map_to_output(const struct scene_map *m, double sx, double sy, double *ox, double *oy) {
    if (!m->valid || m->kx <= 0 || m->ky <= 0) return 0;
    *ox = sx * m->kx + m->off_x;
    *oy = sy * m->ky + m->off_y;
    return 1;
}

int scene_map_to_scene(const struct scene_map *m, double ox, double oy, double *sx, double *sy) {
    if (!m->valid || m->kx <= 0 || m->ky <= 0) return 0;
    *sx = (ox - m->off_x) / m->kx;
    *sy = (oy - m->off_y) / m->ky;
    return 1;
}

int scene_map_root_rect(const int rect[4], int root_w, int root_h, int scene_w, int scene_h, int out[4]) {
    if (scene_w <= 0 || scene_h <= 0 || rect[2] <= 0 || rect[3] <= 0) return 0;
    long long x0 = rect[0], y0 = rect[1], x1 = (long long)rect[0] + rect[2], y1 = (long long)rect[1] + rect[3];
    if (root_w > 0 && root_h > 0 && (root_w != scene_w || root_h != scene_h)) {
        x0 = (x0 * scene_w * 2 + root_w) / (2LL * root_w);
        x1 = (x1 * scene_w * 2 + root_w) / (2LL * root_w);
        y0 = (y0 * scene_h * 2 + root_h) / (2LL * root_h);
        y1 = (y1 * scene_h * 2 + root_h) / (2LL * root_h);
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > scene_w) x1 = scene_w;
    if (y1 > scene_h) y1 = scene_h;
    if (x1 <= x0 || y1 <= y0) return 0;
    out[0] = (int)x0; out[1] = (int)y0; out[2] = (int)(x1 - x0); out[3] = (int)(y1 - y0);
    return 1;
}
