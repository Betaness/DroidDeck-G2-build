/*
 * [mali-g2] Pacing of windows that are entirely hidden, for the X11 WSIs of the Vortek client
 * (client_x11_present.c) and the in-process libhybris path (mali-g2/hybris/src/wsi.c). A hidden
 * window's frame is not drawn: its present waits for the frame's semaphores, then until the window's
 * next frame is due, so the application renders at most VORTEK_COVERED_FPS frames a second
 * (default 1; it was 10, which VORTEK_COVERED_FPS=10 restores). At 10, Steam's Big Picture window
 * behind a game, animating an image on its library page, cost the game 31.0 -> 29.2 fps.
 *
 * The wait is cut into COVERED_SLICE slices and ends as soon as the window shows again: its coverage
 * is checked at once when the root's children change (a top-level window raised, lowered, created or
 * destroyed; one round trip per slice) and every COVERED_CHECK_EVERY seconds otherwise (a window over
 * it unmapped or moved). The wait never outlasts one frame interval: a present blocking until the
 * window shows would stall the presenting thread indefinitely, and Chromium's GPU watchdog terminates
 * a GPU process whose main thread does not respond within its timeout (several seconds).
 *
 * Chromium presents all its windows from one GPU thread, so a hidden window's wait also holds back the
 * frames of a shown window of the process. A hidden window whose present runs on a thread that
 * presented a shown window within COVERED_SHARED_RECENT seconds keeps at least COVERED_SHARED_FPS.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef MALI_G2_COVERED_PACING_H
#define MALI_G2_COVERED_PACING_H

#include <stdbool.h>
#include <stdlib.h>

#define COVERED_DEFAULT_FPS 1
#define COVERED_MAX_FPS 1000
#define COVERED_SHARED_FPS 10
#define COVERED_SHARED_RECENT 0.5
#define COVERED_CHECK_EVERY 0.1
#define COVERED_SLICE 0.008

/* VORTEK_COVERED_FPS's value: a whole number from 1, at most COVERED_MAX_FPS; anything else is the default. */
static inline int covered_fps_parse(const char* value) {
    if (!value || !*value) return COVERED_DEFAULT_FPS;
    char* end;
    long fps = strtol(value, &end, 10);
    if (*end || fps < 1) return COVERED_DEFAULT_FPS;
    return fps > COVERED_MAX_FPS ? COVERED_MAX_FPS : (int)fps;
}

/* Whether a window last shown at `shownAt` (0: never) from the calling thread (`sameThread`) still
 * counts as being drawn at `now`. */
static inline bool covered_shown_recently(double now, double shownAt, bool sameThread) {
    return sameThread && shownAt > 0 && now - shownAt < COVERED_SHARED_RECENT;
}

/* Seconds between two frames of a hidden window; `shared`: a shown window is drawn on its thread. */
static inline double covered_interval(int fps, bool shared) {
    if (fps < 1) fps = COVERED_DEFAULT_FPS;
    if (shared && fps < COVERED_SHARED_FPS) fps = COVERED_SHARED_FPS;
    return 1.0 / fps;
}

typedef struct CoveredWaitOps {
    double (*now)(void* ctx);
    void (*sleep)(void* ctx, double seconds);
    bool (*restacked)(void* ctx); /* the root's children changed since the last call or coverage check */
    bool (*hidden)(void* ctx);    /* checks the window's coverage again: still entirely hidden */
} CoveredWaitOps;

/* Waits until `due`, the hidden window's next frame time; its coverage was last checked at `checked`.
 * Returns the time to count as this frame's: `due` when the wait ran to it, else the time it ended
 * (the window showed again, or the frame was already late). */
static inline double covered_wait(const CoveredWaitOps* ops, void* ctx, double due, double checked) {
    double now = ops->now(ctx);
    bool slept = false;
    while (now < due) {
        if (ops->restacked(ctx) || now - checked >= COVERED_CHECK_EVERY) {
            checked = now;
            if (!ops->hidden(ctx)) return ops->now(ctx);
            now = ops->now(ctx);
            if (now >= due) break;
        }
        double wait = due - now < COVERED_SLICE ? due - now : COVERED_SLICE;
        ops->sleep(ctx, wait);
        now = ops->now(ctx);
        slept = true;
    }
    return slept ? due : now;
}

#endif
