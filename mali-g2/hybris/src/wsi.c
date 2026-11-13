/*
 * X11 WSI for the in-process Mali driver: VK_KHR_surface, VK_KHR_xcb_surface, VK_KHR_xlib_surface,
 * VK_KHR_get_surface_capabilities2 and VK_KHR_swapchain.
 *
 * This is Vortek's presentation (mali-g2/vortek/src) with client and server in one process:
 *   - swapchain images are AHardwareBuffers imported into the driver
 *     (VK_ANDROID_external_memory_android_hardware_buffer), registered with DroidDeck's compositor
 *     over $VORTEK_PRESENT_SOCKET (zero_copy.c) and shown on a SurfaceControl layer over the X window
 *     with an acquire fence; the compositor hands them back with release fences, which acquire puts
 *     into the application's semaphore / fence (headless_swapchain.c);
 *   - whether a frame may go on that layer is decided from the X window tree as the Vortek client
 *     does it (client_x11_present.c): not covered by another window, not clipped by a parent, no
 *     alpha; otherwise the frame is copied into the window (MIT-SHM or PutImage), and a window that
 *     is entirely hidden gets its frames paced instead of drawn.
 *
 * Environment (the names of the Vortek knobs with the same meaning):
 *   VORTEK_PRESENT_SOCKET  the compositor's socket; VORTEK_ZERO_COPY=0 always copies into the window
 *   VORTEK_AHB_SWAPCHAIN=0 plain device-local images (copied into the window)
 *   VORTEK_MIN_IMAGES      surface minImageCount, 1..6 (default 3)
 *   VORTEK_RELEASE_WAIT    import (default) / batch / cpu: how acquire waits for a release fence
 *   VORTEK_X11_SHM=0       PutImage instead of MIT-SHM for copies
 *   VORTEK_X11_STATS=N     frames and fps per swapchain every N seconds
 *   VORTEK_RECREATE_LIMIT  swapchains per second with the same parameters for a window before a size
 *                          mismatch stops being reported as VK_SUBOPTIMAL_KHR (default 10, 0: never;
 *                          ../vortek/src/swapchain_loop.h)
 *   VORTEK_COVERED_FPS     frames a second of a window that is entirely hidden (default 1;
 *                          ../vortek/src/covered_pacing.h)
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Vortek and Winlator's vortekrenderer (C) brunodev85, LGPL-2.1.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include "icd.h"

#include <dlfcn.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include <xcb/shm.h>

#include "covered_pacing.h"
#include "swapchain_loop.h"
#include "zero_copy.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

const VkExtensionProperties wsi_instance_extensions[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_SPEC_VERSION},
    {VK_KHR_XLIB_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_SPEC_VERSION},
    {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_SPEC_VERSION},
};
const uint32_t wsi_instance_extension_count = ARRAY_LEN(wsi_instance_extensions);

static double nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- surfaces ---- */

typedef struct Surface {
    xcb_connection_t* conn;
    xcb_window_t window;
} Surface;

static Surface* surfaceOf(VkSurfaceKHR handle) {
    return (Surface*)(uintptr_t)handle;
}

static VkResult newSurface(xcb_connection_t* conn, xcb_window_t window, VkSurfaceKHR* out) {
    if (!conn || !window) return VK_ERROR_INITIALIZATION_FAILED;
    Surface* surface = calloc(1, sizeof(*surface));
    if (!surface) return VK_ERROR_OUT_OF_HOST_MEMORY;
    surface->conn = conn;
    surface->window = window;
    *out = (VkSurfaceKHR)(uintptr_t)surface;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* info,
                                               const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    return newSurface(info->connection, info->window, surface);
}

static VkResult VKAPI_CALL CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR* info,
                                                const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    static xcb_connection_t* (*getXCBConnection)(Display*);
    if (!getXCBConnection) {
        void* lib = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (lib) getXCBConnection = (xcb_connection_t * (*)(Display*)) dlsym(lib, "XGetXCBConnection");
        if (!getXCBConnection) {
            fprintf(stderr, "hybris-wsi: libX11-xcb.so.1 not found, no Xlib surfaces\n");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return newSurface(info->dpy ? getXCBConnection(info->dpy) : NULL, (xcb_window_t)info->window, surface);
}

static void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
    free(surfaceOf(surface));
}

static VkBool32 VKAPI_CALL GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physical, uint32_t family, xcb_connection_t* conn,
                                                                      xcb_visualid_t visual) {
    return VK_TRUE;
}

static VkBool32 VKAPI_CALL GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physical, uint32_t family, Display* dpy,
                                                                       VisualID visual) {
    return VK_TRUE;
}

static VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface,
                                                              VkBool32* supported) {
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

static bool windowGeometry(Surface* surface, uint32_t* width, uint32_t* height) {
    xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(surface->conn, xcb_get_geometry(surface->conn, surface->window), NULL);
    if (!reply) return false;
    *width = reply->width;
    *height = reply->height;
    free(reply);
    return true;
}

/* Zero-copy keeps one image on screen; with fewer than two the app could never acquire, with two it
 * renders into the image the compositor just released. */
static uint32_t minImageCount(void) {
    static int minImages;
    if (!minImages) {
        int value = icd_env_int("VORTEK_MIN_IMAGES", 3);
        minImages = value < 1 ? 1 : value > 6 ? 6 : value;
    }
    return (uint32_t)minImages;
}

#define MAX_IMAGES 8

static VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR handle,
                                                                   VkSurfaceCapabilitiesKHR* caps) {
    uint32_t width, height;
    if (!windowGeometry(surfaceOf(handle), &width, &height)) return VK_ERROR_SURFACE_LOST_KHR;
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = minImageCount();
    caps->maxImageCount = MAX_IMAGES;
    caps->currentExtent = (VkExtent2D){width, height};
    /* The window's size, as Mesa's X11 WSI and Winlator's vortekrenderer report it: Proton's fullscreen
     * hack (a game's emulated display mode) creates its host swapchain with minImageExtent and scales
     * the game's frames into it. With {1, 1} that was a 1x1 swapchain, every present of it SUBOPTIMAL
     * (the window is larger) and DXVK recreated it every frame. */
    caps->minImageExtent = caps->currentExtent;
    caps->maxImageExtent = (VkExtent2D){MAX(width, 16384), MAX(height, 16384)};
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                                    VkSurfaceCapabilities2KHR* caps) {
    VkResult result = GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info->surface, &caps->surfaceCapabilities);
    for (VkBaseOutStructure* s = caps->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR)
            ((VkSurfaceProtectedCapabilitiesKHR*)s)->supportsProtected = VK_FALSE;
    }
    return result;
}

static const VkFormat surfaceFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_FORMAT_R8G8B8A8_SRGB};

static VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t* count,
                                                              VkSurfaceFormatKHR* formats) {
    uint32_t total = ARRAY_LEN(surfaceFormats);
    if (!formats) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t n = MIN(*count, total);
    for (uint32_t i = 0; i < n; i++) formats[i] = (VkSurfaceFormatKHR){surfaceFormats[i], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                               uint32_t* count, VkSurfaceFormat2KHR* formats) {
    uint32_t total = ARRAY_LEN(surfaceFormats);
    if (!formats) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t n = MIN(*count, total);
    for (uint32_t i = 0; i < n; i++) formats[i].surfaceFormat = (VkSurfaceFormatKHR){surfaceFormats[i], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Presents are never held back for the display: every mode behaves like MAILBOX on the compositor's
 * layer (it shows the newest buffer) and like IMMEDIATE when copied. */
static const VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

static VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t* count,
                                                                   VkPresentModeKHR* modes) {
    uint32_t total = ARRAY_LEN(presentModes);
    if (!modes) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t n = MIN(*count, total);
    memcpy(modes, presentModes, n * sizeof(*modes));
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t* count,
                                                                 VkRect2D* rects) {
    if (!rects) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1) return VK_INCOMPLETE;
    uint32_t width, height;
    if (!windowGeometry(surfaceOf(surface), &width, &height)) return VK_ERROR_SURFACE_LOST_KHR;
    rects[0] = (VkRect2D){{0, 0}, {width, height}};
    *count = 1;
    return VK_SUCCESS;
}

/* ---- swapchains ---- */

typedef struct AcquireWait {
    VkSemaphore semaphore;
    VkFence fence;
    bool pending;
} AcquireWait;

#define ACQUIRE_WAITS 8
#define SHM_BUFFER_COUNT 2

typedef struct ShmBuffer {
    uint8_t* data;
    uint32_t seg;
    int shmId;           /* SysV id, or -1 for memfd */
    bool pending;
    xcb_get_input_focus_cookie_t sync;
} ShmBuffer;

typedef struct Swapchain {
    Device* device;
    Surface* surface;
    VkFormat format;
    VkExtent2D extent;
    VkImageUsageFlags usage;
    uint32_t imageCount;
    VkImage* images;
    VkDeviceMemory* memories;
    AHardwareBuffer** hardwareBuffers;
    bool retired;
    uint32_t nextImage;
    uint32_t lastAcquired;
    uint64_t presentCount;

    /* The compositor side (headless_swapchain.c): who holds each image, guarded by zcLock. */
    uint32_t chainId;
    bool zcUsable;               /* registered, not refused */
    bool shown;
    bool announced;              /* CHAIN_CREATE sent */
    uint8_t* owner;
    int* releaseFd;
    bool stealLogged;
    bool semaphoreImportRefused;
    bool fenceImportRefused;
    bool batchImportRefused;
    AcquireWait acquireWaits[ACQUIRE_WAITS];
    uint32_t nextAcquireWait;
    double lastPresentTime;
    double frameInterval;
    bool slowAcquireLogged;
    bool declineLogged;

    /* The X11 side (client_x11_present.c). */
    bool swapRB;
    bool copyReady;
    bool copyFailed;
    VkCommandPool commandPool;
    VkCommandBuffer* commandBuffers;
    VkFence fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    bool coherent;
    uint8_t* pixels;
    size_t frameSize;
    xcb_gcontext_t gc;
    bool useShm;
    ShmBuffer shm[SHM_BUFFER_COUNT];
    uint32_t shmIndex;
    uint32_t maxRowsPerPut;
    uint8_t* swizzleRow;
    uint64_t frames, statFrames;
    double statStart;
    int zeroCopy;              /* 0: try, 1: in use, -1: unavailable for this swapchain */
    xcb_window_t root;
    uint32_t rootW, rootH;
    bool occluded;
    bool covered;              /* nothing of the window is visible: frames are dropped, paced */
    uint32_t coverCountdown;
    double lastCoveredFrame;
    bool pacedShared;          /* the last hidden frame kept COVERED_SHARED_FPS (covered_pacing.h) */
    uint32_t rootOrder;        /* the root's children when coverage was last checked (hashed), 0: unknown */
    int chainLength;           /* the window's ancestors up to the root and their origins in root coordinates */
    xcb_window_t chain[16];
    int16_t chainX[16], chainY[16];
    int32_t clipX0, clipY0, clipX1, clipY1;  /* where the ancestors let the window show */
    uint32_t chainAge;
    xcb_window_t blocker;      /* what kept the last frame off the layer, logged when it changes */
    int16_t blockerRect[4];
    int lastPath;
    const char* x11Why;
    bool suboptimalLogged;
    /* Recreated in a loop with the same parameters (swapchain_loop.h): a size mismatch is not reported
     * while the window stays quietW x quietH. */
    uint64_t loopKey;
    bool quiet;
    uint32_t quietW, quietH;
    uint64_t stackKey;         /* X stacking position, root level first: each level's index among its siblings + 1 */
    /* What other swapchains of this process may read, under registryLock. */
    struct {
        bool shown;
        int32_t rect[4];
        xcb_window_t window, root;
        int chainLength;
        xcb_window_t chain[16];
        uint64_t key;
        double shownAt;        /* when a frame of it was last drawn (zero-copy or through X11), 0: never */
        pthread_t shownThread; /* the thread that presented it */
    } pub;
    /* Held across a zero-copy present; the sweep only trylocks it, so a hide or restack never
     * overtakes a present that is on its way. */
    pthread_mutex_t presentLock;
} Swapchain;

static Swapchain* swapchainOf(VkSwapchainKHR handle) {
    return (Swapchain*)(uintptr_t)handle;
}

enum { OWNER_FREE = 0, OWNER_APP = 1, OWNER_COMPOSITOR = 2 };
enum { PATH_ZERO_COPY = 1, PATH_COVERED, PATH_OCCLUDED, PATH_X11 };

/* Every swapchain of this process, to tell which windows are on compositor layers. */
#define MAX_TRACKED_SWAPCHAINS 64
static pthread_mutex_t registryLock = PTHREAD_MUTEX_INITIALIZER;
static Swapchain* registry[MAX_TRACKED_SWAPCHAINS];

/* Swapchains registered for zero-copy. */
#define MAX_ZERO_COPY_CHAINS 64
static pthread_mutex_t zcLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t zcCond = PTHREAD_COND_INITIALIZER;
static Swapchain* zcChains[MAX_ZERO_COPY_CHAINS];
static uint32_t zcNextChain = 1;

static void registryAdd(Swapchain* sc) {
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (!registry[i]) {
            registry[i] = sc;
            break;
        }
    }
    pthread_mutex_unlock(&registryLock);
}

static void registryRemove(Swapchain* sc) {
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) if (registry[i] == sc) registry[i] = NULL;
    pthread_mutex_unlock(&registryLock);
}

static void publishLayer(Swapchain* sc, bool shown, int x, int y, int w, int h) {
    pthread_mutex_lock(&registryLock);
    sc->pub.shown = shown;
    if (shown) {
        sc->pub.rect[0] = x; sc->pub.rect[1] = y; sc->pub.rect[2] = w; sc->pub.rect[3] = h;
        sc->pub.window = sc->surface->window;
        sc->pub.root = sc->root;
        sc->pub.chainLength = sc->chainLength;
        memcpy(sc->pub.chain, sc->chain, sizeof(sc->chain));
        sc->pub.key = sc->stackKey;
    }
    pthread_mutex_unlock(&registryLock);
}

static void noteShown(Swapchain* sc, double now) {
    pthread_mutex_lock(&registryLock);
    sc->pub.shownAt = now;
    sc->pub.shownThread = pthread_self();
    pthread_mutex_unlock(&registryLock);
}

/* Whether this thread drew a frame of another window of the process recently (covered_pacing.h). */
static bool shownOnThisThread(Swapchain* sc, double now) {
    pthread_t self = pthread_self();
    bool shown = false;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS && !shown; i++) {
        Swapchain* other = registry[i];
        if (other && other != sc && other->pub.shownAt > 0)
            shown = covered_shown_recently(now, other->pub.shownAt, pthread_equal(other->pub.shownThread, self));
    }
    pthread_mutex_unlock(&registryLock);
    return shown;
}

/* ---- the compositor side ---- */

static void releaseImageLocked(Swapchain* sc, uint32_t index, int fd) {
    if (index >= sc->imageCount) {
        if (fd >= 0) close(fd);
        return;
    }
    if (sc->releaseFd[index] >= 0) close(sc->releaseFd[index]);
    sc->releaseFd[index] = fd;
    if (sc->owner[index] == OWNER_COMPOSITOR) sc->owner[index] = OWNER_FREE;
}

static void onRelease(uint32_t chain, uint32_t index, int fd) {
    bool hideRefused = false;
    pthread_mutex_lock(&zcLock);
    for (int i = 0; i < MAX_ZERO_COPY_CHAINS; i++) {
        Swapchain* sc = zcChains[i];
        if (!sc) continue;
        if (index == ZC_REFUSED) {
            if (sc->chainId == chain) {
                sc->zcUsable = false;
                sc->shown = false;
                hideRefused = true;
                fprintf(stderr, "hybris-wsi: compositor refused zero-copy swapchain %u, presenting through X11\n", chain);
            }
        }
        else if (index == ZC_RELEASE_ALL) {
            for (uint32_t j = 0; j < sc->imageCount; j++) releaseImageLocked(sc, j, -1);
            sc->zcUsable = false;
            sc->shown = false;
        }
        else if (sc->chainId == chain) {
            releaseImageLocked(sc, index, fd);
            fd = -1;
        }
    }
    pthread_cond_broadcast(&zcCond);
    pthread_mutex_unlock(&zcLock);
    if (fd >= 0) close(fd);
    /* A frame of this chain sent before the refusal arrived may be on a layer by now. */
    if (hideRefused) ZeroCopy_hide(chain);
}

static void registerZeroCopy(Swapchain* sc) {
    if (sc->imageCount < 2 || !sc->device->syncFd) return;
    for (uint32_t i = 0; i < sc->imageCount; i++) if (!sc->hardwareBuffers[i]) return;
    if (!ZeroCopy_available()) return;
    ZeroCopy_setReleaseHandler(onRelease);
    sc->owner = calloc(sc->imageCount, sizeof(uint8_t));
    sc->releaseFd = malloc(sc->imageCount * sizeof(int));
    if (!sc->owner || !sc->releaseFd) {
        free(sc->owner);
        free(sc->releaseFd);
        sc->owner = NULL;
        sc->releaseFd = NULL;
        return;
    }
    for (uint32_t i = 0; i < sc->imageCount; i++) sc->releaseFd[i] = -1;

    pthread_mutex_lock(&zcLock);
    int slot = -1;
    for (int i = 0; i < MAX_ZERO_COPY_CHAINS && slot < 0; i++) if (!zcChains[i]) slot = i;
    if (slot < 0) {
        /* Full of swapchains that never presented zero-copy: the oldest such entry gives up its place. */
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < MAX_ZERO_COPY_CHAINS; i++) {
            if (!zcChains[i]->announced && zcChains[i]->chainId < oldest) {
                oldest = zcChains[i]->chainId;
                slot = i;
            }
        }
        if (slot >= 0) {
            zcChains[slot]->zcUsable = false;
            zcChains[slot] = NULL;
        }
    }
    if (slot >= 0) {
        sc->chainId = zcNextChain++;
        zcChains[slot] = sc;
    }
    pthread_mutex_unlock(&zcLock);
    /* Registered with the compositor on its first zero-copy present. */
    if (slot >= 0) sc->zcUsable = true;
}

static bool announceChain(Swapchain* sc) {
    if (sc->announced) return true;
    sc->announced = ZeroCopy_createChain(sc->chainId, sc->extent.width, sc->extent.height, sc->hardwareBuffers, sc->imageCount);
    if (!sc->announced) sc->zcUsable = false;
    return sc->announced;
}

static void unregisterZeroCopy(Swapchain* sc) {
    if (!sc->owner) return;
    pthread_mutex_lock(&zcLock);
    for (int i = 0; i < MAX_ZERO_COPY_CHAINS; i++) if (zcChains[i] == sc) zcChains[i] = NULL;
    pthread_mutex_unlock(&zcLock);
    if (sc->announced) ZeroCopy_destroyChain(sc->chainId);
    for (uint32_t i = 0; i < sc->imageCount; i++) if (sc->releaseFd[i] >= 0) close(sc->releaseFd[i]);
    free(sc->owner);
    free(sc->releaseFd);
    sc->owner = NULL;
    sc->releaseFd = NULL;
}

static uint32_t findMemoryType(Device* device, uint32_t typeBits, VkMemoryPropertyFlags wanted) {
    for (uint32_t i = 0; i < device->memory.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (device->memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    for (uint32_t i = 0; i < device->memory.memoryTypeCount; i++)
        if (typeBits & (1u << i)) return i;
    return 0;
}

static uint32_t ahbFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: return 5; /* HAL_PIXEL_FORMAT_BGRA_8888 */
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        default: return 0;
    }
}

/* The image is created for the AHB and bound to it as dedicated imported memory, so the driver
 * renders straight into a buffer the compositor can scan out. */
static VkResult createHardwareBufferImage(Swapchain* sc, const VkImageCreateInfo* imageInfo, uint32_t i) {
    Device* device = sc->device;
    uint32_t format = ahbFormat(sc->format);
    if (!format || !device->ahb) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    AHardwareBuffer_Desc desc = {0};
    desc.width = sc->extent.width;
    desc.height = sc->extent.height;
    desc.layers = 1;
    desc.format = format;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (icd_env_int("DROIDDECK_HYBRIS_AHB_CPU_READ", 0)) desc.usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* hardwareBuffer = NULL;
    if (AHardwareBuffer_allocate(&desc, &hardwareBuffer) != 0 || !hardwareBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkExternalMemoryImageCreateInfo externalInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    externalInfo.pNext = imageInfo->pNext;
    VkImageCreateInfo info = *imageInfo;
    info.pNext = &externalInfo;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = device->CreateImage(device->handle, &info, NULL, &image);
    if (result == VK_SUCCESS) {
        VkAndroidHardwareBufferPropertiesANDROID properties = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        result = device->GetAndroidHardwareBufferPropertiesANDROID(device->handle, hardwareBuffer, &properties);
        if (result == VK_SUCCESS) {
            VkImportAndroidHardwareBufferInfoANDROID importInfo = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
            importInfo.buffer = hardwareBuffer;
            VkMemoryDedicatedAllocateInfo dedicatedInfo = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = image;
            VkMemoryAllocateInfo memoryInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            memoryInfo.pNext = &dedicatedInfo;
            memoryInfo.allocationSize = properties.allocationSize;
            memoryInfo.memoryTypeIndex = findMemoryType(device, properties.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            result = device->AllocateMemory(device->handle, &memoryInfo, NULL, &memory);
        }
        if (result == VK_SUCCESS) result = device->BindImageMemory(device->handle, image, memory, 0);
    }
    if (result != VK_SUCCESS) {
        if (memory) device->FreeMemory(device->handle, memory, NULL);
        if (image) device->DestroyImage(device->handle, image, NULL);
        AHardwareBuffer_release(hardwareBuffer);
        return result;
    }
    sc->images[i] = image;
    sc->memories[i] = memory;
    sc->hardwareBuffers[i] = hardwareBuffer;
    return VK_SUCCESS;
}

static VkResult createImage(Swapchain* sc, const VkSwapchainCreateInfoKHR* createInfo, uint32_t i) {
    Device* device = sc->device;
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = sc->format;
    imageInfo.extent = (VkExtent3D){sc->extent.width, sc->extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = sc->usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = createInfo->imageSharingMode;
    imageInfo.queueFamilyIndexCount = createInfo->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? createInfo->queueFamilyIndexCount : 0;
    imageInfo.pQueueFamilyIndices = createInfo->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? createInfo->pQueueFamilyIndices : NULL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (icd_env_int("VORTEK_AHB_SWAPCHAIN", 1)) {
        VkResult result = createHardwareBufferImage(sc, &imageInfo, i);
        if (result == VK_SUCCESS) return result;
        fprintf(stderr, "hybris-wsi: AHardwareBuffer swapchain image failed (%d), using a plain image\n", result);
    }
    VkResult result = device->CreateImage(device->handle, &imageInfo, NULL, &sc->images[i]);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    device->GetImageMemoryRequirements(device->handle, sc->images[i], &requirements);
    VkMemoryAllocateInfo memoryInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = findMemoryType(device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    result = device->AllocateMemory(device->handle, &memoryInfo, NULL, &sc->memories[i]);
    if (result == VK_SUCCESS) result = device->BindImageMemory(device->handle, sc->images[i], sc->memories[i], 0);
    return result;
}

/* Zero-copy: an image the compositor still shows cannot be handed out (like BufferQueue's dequeue).
 * Waits for a free one and hands its release fence to the caller (-1: nothing to wait for). After
 * 2 s without a release an image is taken anyway, as the Vortek server does. */
static VkResult acquireZeroCopy(Swapchain* sc, uint64_t timeout, uint32_t* index, int* releaseFd) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t wait = timeout < 2000000000ull ? timeout : 2000000000ull;
    deadline.tv_sec += (time_t)(wait / 1000000000ull);
    deadline.tv_nsec += (long)(wait % 1000000000ull);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    double waitStart = 0;
    int found = -1;
    bool timedOut = false;
    pthread_mutex_lock(&zcLock);
    while (found < 0) {
        for (uint32_t k = 0; k < sc->imageCount && found < 0; k++) {
            uint32_t i = (sc->nextImage + k) % sc->imageCount;
            if (sc->owner[i] == OWNER_FREE) found = (int)i;
        }
        if (found >= 0 || !sc->zcUsable) break;
        if (!waitStart) waitStart = nowSeconds();
        if (timeout == 0 || pthread_cond_timedwait(&zcCond, &zcLock, &deadline) != 0) {
            timedOut = true;
            break;
        }
    }
    if (found < 0 && timedOut && timeout < 2000000000ull) {
        pthread_mutex_unlock(&zcLock);
        return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
    }
    bool stolen = found < 0;
    if (stolen) found = (int)sc->nextImage;
    *releaseFd = sc->releaseFd[found];
    sc->releaseFd[found] = -1;
    sc->owner[found] = OWNER_APP;
    pthread_mutex_unlock(&zcLock);

    if (stolen && sc->zcUsable) {
        if (!sc->stealLogged) fprintf(stderr, "hybris-wsi: zero-copy swapchain %u: no image released in 2 s, reusing one\n", sc->chainId);
        sc->stealLogged = true;
    }
    else if (waitStart && !sc->slowAcquireLogged && sc->presentCount > (uint64_t)sc->imageCount * 2) {
        double waited = nowSeconds() - waitStart;
        double frame = sc->frameInterval > 0 ? sc->frameInterval : 1.0 / 60;
        if (waited > frame) {
            fprintf(stderr, "hybris-wsi: zero-copy swapchain %u (%u images): acquire waited %.1f ms for the compositor to release an image "
                    "(frame %.1f ms), logged once\n", sc->chainId, sc->imageCount, waited * 1000, frame * 1000);
            sc->slowAcquireLogged = true;
        }
    }
    *index = (uint32_t)found;
    return VK_SUCCESS;
}

enum {
    RELEASE_SEMAPHORE_IMPORT = 1,
    RELEASE_FENCE_IMPORT = 2,
    RELEASE_GPU_WAIT = 4,
    RELEASE_CPU_WAIT = 8,
    RELEASE_WAIT_IMPORT_FAILED = 16,
    RELEASE_SEMAPHORE_IMPORT_FAILED = 32,
    RELEASE_FENCE_IMPORT_FAILED = 64,
};

static void logReleasePath(int path, const char* message, VkResult result) {
    static int logged;
    if (__atomic_fetch_or(&logged, path, __ATOMIC_RELAXED) & path) return;
    if (result != VK_SUCCESS) fprintf(stderr, "hybris-wsi: zero-copy acquire: %s (%d)\n", message, result);
    else fprintf(stderr, "hybris-wsi: zero-copy acquire: %s\n", message);
}

/* Sync fd imports take the fd only on success and must be temporary. */
static VkResult importSemaphore(Device* device, VkSemaphore semaphore, int fd) {
    int copy = dup(fd);
    if (copy < 0) return VK_ERROR_TOO_MANY_OBJECTS;
    VkImportSemaphoreFdInfoKHR info = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    info.semaphore = semaphore;
    info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    info.fd = copy;
    VkResult result = device->ImportSemaphoreFdKHR(device->handle, &info);
    if (result != VK_SUCCESS) close(copy);
    return result;
}

static VkResult importFence(Device* device, VkFence fence, int fd) {
    int copy = dup(fd);
    if (copy < 0) return VK_ERROR_TOO_MANY_OBJECTS;
    VkImportFenceFdInfoKHR info = {VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
    info.fence = fence;
    info.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
    info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    info.fd = copy;
    VkResult result = device->ImportFenceFdKHR(device->handle, &info);
    if (result != VK_SUCCESS) close(copy);
    return result;
}

/* A batch that waits for the release fence (imported into a semaphore of ours) and signals the
 * app's semaphore; the app's fence follows in an empty submission. Our semaphore can be imported
 * into again once its batch ran. */
static bool waitOnGpu(Swapchain* sc, int fd, VkSemaphore signalSemaphore, VkFence fence, VkResult* result) {
    Device* device = sc->device;
    if (sc->batchImportRefused) return false;
    AcquireWait* wait = NULL;
    for (int k = 0; k < ACQUIRE_WAITS && !wait; k++) {
        AcquireWait* candidate = &sc->acquireWaits[(sc->nextAcquireWait + k) % ACQUIRE_WAITS];
        if (!candidate->pending || device->GetFenceStatus(device->handle, candidate->fence) == VK_SUCCESS) wait = candidate;
    }
    if (!wait) {
        wait = &sc->acquireWaits[sc->nextAcquireWait];
        if (device->WaitForFences(device->handle, 1, &wait->fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return false;
    }
    sc->nextAcquireWait = (uint32_t)(wait - sc->acquireWaits + 1) % ACQUIRE_WAITS;

    if (!wait->semaphore) {
        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (device->CreateSemaphore(device->handle, &semaphoreInfo, NULL, &wait->semaphore) != VK_SUCCESS) wait->semaphore = VK_NULL_HANDLE;
        if (device->CreateFence(device->handle, &fenceInfo, NULL, &wait->fence) != VK_SUCCESS) wait->fence = VK_NULL_HANDLE;
        if (!wait->semaphore || !wait->fence) {
            if (wait->semaphore) device->DestroySemaphore(device->handle, wait->semaphore, NULL);
            if (wait->fence) device->DestroyFence(device->handle, wait->fence, NULL);
            wait->semaphore = VK_NULL_HANDLE;
            wait->fence = VK_NULL_HANDLE;
            return false;
        }
    }
    if (wait->pending) {
        device->ResetFences(device->handle, 1, &wait->fence);
        wait->pending = false;
    }
    VkResult imported = importSemaphore(device, wait->semaphore, fd);
    if (imported != VK_SUCCESS) {
        logReleasePath(RELEASE_WAIT_IMPORT_FAILED, "the driver refused a release fence for our wait semaphore", imported);
        sc->batchImportRefused = true;
        return false;
    }
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &wait->semaphore;
    submitInfo.pWaitDstStageMask = &stage;
    if (signalSemaphore) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;
    }
    VkResult submitted = icd_queue_submit(device, device->internalQueue, 1, &submitInfo, wait->fence);
    if (submitted != VK_SUCCESS) {
        device->DestroySemaphore(device->handle, wait->semaphore, NULL);
        device->DestroyFence(device->handle, wait->fence, NULL);
        wait->semaphore = VK_NULL_HANDLE;
        wait->fence = VK_NULL_HANDLE;
        if (submitted == VK_ERROR_DEVICE_LOST) *result = submitted;
        return false;
    }
    wait->pending = true;
    if (fence) {
        submitted = icd_queue_submit(device, device->internalQueue, 0, NULL, fence);
        if (submitted != VK_SUCCESS) *result = submitted;
    }
    return true;
}

enum { RELEASE_WAIT_IMPORT, RELEASE_WAIT_BATCH, RELEASE_WAIT_CPU };

static int releaseWaitMode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* value = getenv("VORTEK_RELEASE_WAIT");
        int parsed = RELEASE_WAIT_IMPORT;
        if (value && strcmp(value, "batch") == 0) parsed = RELEASE_WAIT_BATCH;
        else if (value && strcmp(value, "cpu") == 0) parsed = RELEASE_WAIT_CPU;
        mode = parsed;
    }
    return mode;
}

/* The compositor's release fence of the acquired image goes to the GPU instead of being waited for
 * here. Clears *signalSemaphore / *fence once they are taken care of. */
static VkResult waitForRelease(Swapchain* sc, int fd, VkSemaphore* signalSemaphore, VkFence* fence) {
    Device* device = sc->device;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 0) == 1) return VK_SUCCESS;
    int mode = releaseWaitMode();
    if (mode == RELEASE_WAIT_IMPORT && *signalSemaphore && !sc->semaphoreImportRefused) {
        VkResult result = importSemaphore(device, *signalSemaphore, fd);
        if (result == VK_SUCCESS) {
            *signalSemaphore = VK_NULL_HANDLE;
            logReleasePath(RELEASE_SEMAPHORE_IMPORT, "release fences imported into the acquire semaphore", VK_SUCCESS);
        }
        else {
            sc->semaphoreImportRefused = true;
            logReleasePath(RELEASE_SEMAPHORE_IMPORT_FAILED, "the driver refused a release fence for the app's acquire semaphore", result);
        }
    }
    if (mode == RELEASE_WAIT_IMPORT && *fence && !sc->fenceImportRefused) {
        VkResult result = importFence(device, *fence, fd);
        if (result == VK_SUCCESS) {
            *fence = VK_NULL_HANDLE;
            logReleasePath(RELEASE_FENCE_IMPORT, "release fences imported into the acquire fence", VK_SUCCESS);
        }
        else {
            sc->fenceImportRefused = true;
            logReleasePath(RELEASE_FENCE_IMPORT_FAILED, "the driver refused a release fence for the app's acquire fence", result);
        }
    }
    if (!*signalSemaphore && !*fence) return VK_SUCCESS;
    VkResult result = VK_SUCCESS;
    if (mode != RELEASE_WAIT_CPU && waitOnGpu(sc, fd, *signalSemaphore, *fence, &result)) {
        *signalSemaphore = VK_NULL_HANDLE;
        *fence = VK_NULL_HANDLE;
        logReleasePath(RELEASE_GPU_WAIT, "release fences waited for by a GPU batch", VK_SUCCESS);
        return result;
    }
    if (result == VK_ERROR_DEVICE_LOST) return result;
    logReleasePath(RELEASE_CPU_WAIT, "release fences waited for on the CPU", VK_SUCCESS);
    poll(&p, 1, 2000);
    return VK_SUCCESS;
}

static VkResult acquire(Swapchain* sc, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex) {
    if (sc->retired) return VK_ERROR_OUT_OF_DATE_KHR;
    Device* device = sc->device;
    int releaseFd = -1;
    uint32_t index = sc->nextImage;
    if (sc->owner) {
        VkResult result = acquireZeroCopy(sc, timeout, &index, &releaseFd);
        if (result != VK_SUCCESS) return result;
    }
    if (releaseFd >= 0) {
        VkResult result = waitForRelease(sc, releaseFd, &semaphore, &fence);
        close(releaseFd);
        if (result == VK_ERROR_DEVICE_LOST) return result;
    }
    if (semaphore || fence) {
        VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (semaphore) {
            submitInfo.pSignalSemaphores = &semaphore;
            submitInfo.signalSemaphoreCount = 1;
        }
        VkResult result = icd_queue_submit(device, device->internalQueue, 1, &submitInfo, fence);
        if (result == VK_ERROR_DEVICE_LOST) return result;
    }
    sc->lastAcquired = index;
    sc->nextImage = (index + 1) % sc->imageCount;
    *imageIndex = index;
    return VK_SUCCESS;
}

/* The app's present: wait for its rendering on the GPU, export that as a sync fence and give the image
 * to the compositor. VK_ERROR_FEATURE_NOT_PRESENT (never for this swapchain) and VK_NOT_READY (not this
 * frame) leave the semaphores untouched, so the frame can go through X11 instead. */
static VkResult presentToCompositor(Swapchain* sc, VkQueue queue, uint32_t index, const VkSemaphore* waitSemaphores, uint32_t waitCount,
                                    int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rootW, uint32_t rootH, uint64_t stackKey,
                                    uint32_t topLevel) {
    Device* device = sc->device;
    if (!sc->owner || !sc->zcUsable || index >= sc->imageCount) {
        if (!sc->declineLogged) {
            sc->declineLogged = true;
            fprintf(stderr, "hybris-wsi: swapchain %ux%u presents through X11: %s\n", sc->extent.width, sc->extent.height,
                    !sc->owner ? "not registered for zero-copy (no AHardwareBuffers, sync fds or compositor)"
                    : "zero-copy was turned off for it");
        }
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (!ZeroCopy_accepting()) return VK_NOT_READY;
    if (!announceChain(sc)) return VK_ERROR_FEATURE_NOT_PRESENT;
    double presentTime = nowSeconds();
    if (sc->lastPresentTime > 0) {
        double interval = presentTime - sc->lastPresentTime;
        sc->frameInterval = sc->frameInterval > 0 ? sc->frameInterval * 0.9 + interval * 0.1 : interval;
    }
    sc->lastPresentTime = presentTime;

    VkExportFenceCreateInfo exportInfo = {VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.pNext = &exportInfo;
    VkFence fence = VK_NULL_HANDLE;
    if (device->CreateFence(device->handle, &fenceInfo, NULL, &fence) != VK_SUCCESS) return VK_ERROR_FEATURE_NOT_PRESENT;

    VkPipelineStageFlags stages[waitCount > 0 ? waitCount : 1];
    for (uint32_t i = 0; i < waitCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = stages;
    VkResult result = icd_queue_submit(device, queue, 1, &submitInfo, fence);
    if (result != VK_SUCCESS) {
        device->DestroyFence(device->handle, fence, NULL);
        return result;
    }
    /* A fence that already signalled has no sync file on Mali (the export fails): nothing to wait for. */
    int fd = -1;
    VkFenceGetFdInfoKHR fdInfo = {VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
    fdInfo.fence = fence;
    fdInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    if (device->GetFenceFdKHR(device->handle, &fdInfo, &fd) != VK_SUCCESS) {
        device->WaitForFences(device->handle, 1, &fence, VK_TRUE, 1000000000ull);
        fd = -1;
    }
    device->DestroyFence(device->handle, fence, NULL);

    pthread_mutex_lock(&zcLock);
    sc->owner[index] = OWNER_COMPOSITOR;
    pthread_mutex_unlock(&zcLock);
    if (ZeroCopy_present(sc->chainId, index, x, y, w, h, rootW, rootH, stackKey, topLevel, fd)) {
        sc->shown = true;
    }
    else {
        /* The semaphores are consumed: report success and drop this frame, later ones use X11. */
        pthread_mutex_lock(&zcLock);
        sc->owner[index] = OWNER_FREE;
        sc->zcUsable = false;
        pthread_cond_broadcast(&zcCond);
        pthread_mutex_unlock(&zcLock);
    }
    return VK_SUCCESS;
}

/* After each present of image `index`, zero-copy or through X11. */
static void presentDone(Swapchain* sc, uint32_t index) {
    sc->presentCount++;
    if (index >= sc->imageCount) index = sc->lastAcquired;
    if (!sc->owner) return;
    /* Still the app's: it went through X11. Free it, and take the layer down. */
    bool hide = false;
    pthread_mutex_lock(&zcLock);
    if (sc->owner[index] == OWNER_APP) {
        sc->owner[index] = OWNER_FREE;
        hide = sc->shown;
        sc->shown = false;
        pthread_cond_broadcast(&zcCond);
    }
    pthread_mutex_unlock(&zcLock);
    if (hide) ZeroCopy_hide(sc->chainId);
}

static void hideLayer(Swapchain* sc) {
    if (!sc->owner) return;
    pthread_mutex_lock(&zcLock);
    bool hide = sc->shown;
    sc->shown = false;
    pthread_mutex_unlock(&zcLock);
    if (hide) ZeroCopy_hide(sc->chainId);
}

static void restackLayer(Swapchain* sc, uint64_t stackKey, uint32_t topLevel) {
    if (!sc->owner) return;
    pthread_mutex_lock(&zcLock);
    bool shown = sc->shown;
    pthread_mutex_unlock(&zcLock);
    if (shown) ZeroCopy_restack(sc->chainId, stackKey, topLevel);
}

/* ---- the X11 side: copies ---- */

static uint32_t findHostMemoryType(Device* device, uint32_t typeBits, bool* coherent) {
    const VkMemoryPropertyFlags wanted[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    for (uint32_t w = 0; w < ARRAY_LEN(wanted); w++) {
        for (uint32_t i = 0; i < device->memory.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) && (device->memory.memoryTypes[i].propertyFlags & wanted[w]) == wanted[w]) {
                *coherent = (device->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    return UINT32_MAX;
}

static void recordCopy(Swapchain* sc, VkCommandBuffer cmd, VkImage image) {
    Device* device = sc->device;
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    device->BeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    device->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = (VkExtent3D){sc->extent.width, sc->extent.height, 1};
    device->CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->buffer, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier bufferBarrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = sc->buffer;
    bufferBarrier.size = VK_WHOLE_SIZE;
    device->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                               0, NULL, 1, &bufferBarrier, 1, &barrier);
    device->EndCommandBuffer(cmd);
}

static bool attachShm(Swapchain* sc, ShmBuffer* shm, bool useFd) {
    xcb_connection_t* conn = sc->surface->conn;
    shm->seg = xcb_generate_id(conn);
    xcb_generic_error_t* error;
    if (useFd) {
        int fd = memfd_create("hybris-x11", MFD_CLOEXEC);
        if (fd < 0) return false;
        if (ftruncate(fd, (off_t)sc->frameSize) != 0) {
            close(fd);
            return false;
        }
        void* data = mmap(NULL, sc->frameSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return false;
        }
        /* xcb takes ownership of the fd */
        error = xcb_request_check(conn, xcb_shm_attach_fd_checked(conn, shm->seg, fd, 1));
        if (error) {
            free(error);
            munmap(data, sc->frameSize);
            return false;
        }
        shm->data = data;
        shm->shmId = -1;
        return true;
    }
    int shmId = shmget(IPC_PRIVATE, sc->frameSize, IPC_CREAT | 0600);
    if (shmId < 0) return false;
    void* data = shmat(shmId, NULL, 0);
    if (data == (void*)-1) {
        shmctl(shmId, IPC_RMID, NULL);
        return false;
    }
    error = xcb_request_check(conn, xcb_shm_attach_checked(conn, shm->seg, (uint32_t)shmId, 1));
    shmctl(shmId, IPC_RMID, NULL);
    if (error) {
        free(error);
        shmdt(data);
        return false;
    }
    shm->data = data;
    shm->shmId = shmId;
    return true;
}

static void detachShm(Swapchain* sc, ShmBuffer* shm) {
    if (!shm->data) return;
    xcb_connection_t* conn = sc->surface->conn;
    if (shm->pending) free(xcb_get_input_focus_reply(conn, shm->sync, NULL));
    xcb_shm_detach(conn, shm->seg);
    if (shm->shmId >= 0) shmdt(shm->data);
    else munmap(shm->data, sc->frameSize);
    shm->data = NULL;
}

static const char* setupX11(Swapchain* sc) {
    xcb_connection_t* conn = sc->surface->conn;
    sc->gc = xcb_generate_id(conn);
    xcb_create_gc(conn, sc->gc, sc->surface->window, 0, NULL);
    uint32_t maxBytes = xcb_get_maximum_request_length(conn) * 4;
    sc->maxRowsPerPut = MAX((maxBytes - 64) / (sc->extent.width * 4), 1);
    if (!icd_env_int("VORTEK_X11_SHM", 1)) return "PutImage (VORTEK_X11_SHM=0)";
    xcb_shm_query_version_reply_t* version = xcb_shm_query_version_reply(conn, xcb_shm_query_version(conn), NULL);
    if (!version) return "PutImage (no MIT-SHM)";
    bool hasFd = version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2);
    free(version);
    for (int pass = hasFd ? 0 : 1; pass < 2; pass++) {
        bool ok = true;
        for (int i = 0; i < SHM_BUFFER_COUNT && ok; i++) ok = attachShm(sc, &sc->shm[i], pass == 0);
        if (ok) {
            sc->useShm = true;
            return pass == 0 ? "MIT-SHM (memfd)" : "MIT-SHM (SysV)";
        }
        for (int i = 0; i < SHM_BUFFER_COUNT; i++) detachShm(sc, &sc->shm[i]);
    }
    return "PutImage (MIT-SHM attach failed)";
}

static bool setupCopy(Swapchain* sc, uint32_t family) {
    Device* device = sc->device;
    VkDevice handle = device->handle;
    sc->frameSize = (size_t)sc->extent.width * sc->extent.height * 4;
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = sc->frameSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (device->CreateBuffer(handle, &bufferInfo, NULL, &sc->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    device->GetBufferMemoryRequirements(handle, sc->buffer, &requirements);
    VkMemoryAllocateInfo memoryInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = findHostMemoryType(device, requirements.memoryTypeBits, &sc->coherent);
    if (memoryInfo.memoryTypeIndex == UINT32_MAX) return false;
    if (device->AllocateMemory(handle, &memoryInfo, NULL, &sc->memory) != VK_SUCCESS) return false;
    if (device->BindBufferMemory(handle, sc->buffer, sc->memory, 0) != VK_SUCCESS) return false;
    if (device->MapMemory(handle, sc->memory, 0, VK_WHOLE_SIZE, 0, (void**)&sc->pixels) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = family;
    if (device->CreateCommandPool(handle, &poolInfo, NULL, &sc->commandPool) != VK_SUCCESS) return false;
    sc->commandBuffers = calloc(sc->imageCount, sizeof(VkCommandBuffer));
    if (!sc->commandBuffers) return false;
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = sc->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = sc->imageCount;
    if (device->AllocateCommandBuffers(handle, &allocInfo, sc->commandBuffers) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < sc->imageCount; i++) recordCopy(sc, sc->commandBuffers[i], sc->images[i]);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (device->CreateFence(handle, &fenceInfo, NULL, &sc->fence) != VK_SUCCESS) return false;

    const char* mode = setupX11(sc);
    if (sc->swapRB && !sc->useShm) sc->swizzleRow = malloc((size_t)sc->extent.width * 4 * MIN(sc->maxRowsPerPut, sc->extent.height));
    fprintf(stderr, "hybris-x11: window 0x%x %ux%u format %d, %u images, %s%s\n", sc->surface->window, sc->extent.width,
            sc->extent.height, sc->format, sc->imageCount, mode, sc->swapRB ? ", R/B swizzle" : "");
    sc->statStart = nowSeconds();
    return true;
}

static void releaseCopy(Swapchain* sc) {
    Device* device = sc->device;
    VkDevice handle = device->handle;
    for (int i = 0; i < SHM_BUFFER_COUNT; i++) detachShm(sc, &sc->shm[i]);
    if (sc->gc) {
        xcb_free_gc(sc->surface->conn, sc->gc);
        xcb_flush(sc->surface->conn);
        sc->gc = 0;
    }
    if (sc->fence) device->DestroyFence(handle, sc->fence, NULL);
    if (sc->commandPool) device->DestroyCommandPool(handle, sc->commandPool, NULL);
    if (sc->pixels) device->UnmapMemory(handle, sc->memory);
    if (sc->buffer) device->DestroyBuffer(handle, sc->buffer, NULL);
    if (sc->memory) device->FreeMemory(handle, sc->memory, NULL);
    sc->fence = VK_NULL_HANDLE;
    sc->commandPool = VK_NULL_HANDLE;
    sc->pixels = NULL;
    sc->buffer = VK_NULL_HANDLE;
    sc->memory = VK_NULL_HANDLE;
    free(sc->commandBuffers);
    free(sc->swizzleRow);
    sc->commandBuffers = NULL;
    sc->swizzleRow = NULL;
}

/* Copy rows from the (possibly uncached) GPU mapping, swapping R and B. */
static void swizzleRows(uint32_t* restrict dst, const uint32_t* restrict src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t v = src[i];
        dst[i] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
    }
}

static void putFrame(Swapchain* sc, uint8_t depth) {
    xcb_connection_t* conn = sc->surface->conn;
    uint32_t width = sc->extent.width, height = sc->extent.height;
    size_t stride = (size_t)width * 4;
    if (sc->useShm) {
        ShmBuffer* shm = &sc->shm[sc->shmIndex];
        sc->shmIndex = (sc->shmIndex + 1) % SHM_BUFFER_COUNT;
        if (shm->pending) {
            free(xcb_get_input_focus_reply(conn, shm->sync, NULL));
            shm->pending = false;
        }
        if (sc->swapRB) swizzleRows((uint32_t*)shm->data, (const uint32_t*)sc->pixels, (size_t)width * height);
        else memcpy(shm->data, sc->pixels, sc->frameSize);
        xcb_shm_put_image(conn, sc->surface->window, sc->gc, (uint16_t)width, (uint16_t)height, 0, 0, (uint16_t)width, (uint16_t)height, 0, 0,
                          depth, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, shm->seg, 0);
        shm->sync = xcb_get_input_focus(conn);
        shm->pending = true;
    }
    else {
        uint32_t rowsPerPut = MIN(sc->maxRowsPerPut, height);
        for (uint32_t y = 0; y < height; y += rowsPerPut) {
            uint32_t rows = MIN(rowsPerPut, height - y);
            const uint8_t* src = sc->pixels + y * stride;
            if (sc->swapRB) {
                swizzleRows((uint32_t*)sc->swizzleRow, (const uint32_t*)src, (size_t)width * rows);
                src = sc->swizzleRow;
            }
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, sc->surface->window, sc->gc, (uint16_t)width, (uint16_t)rows, 0, (int16_t)y, 0,
                          depth, (uint32_t)(rows * stride), src);
        }
        /* PutImage data is copied into the socket; a round trip keeps frames from queueing up. */
        free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
    }
    xcb_flush(conn);
}

/* ---- the X11 side: where the window is and what covers it ---- */

static bool rectsOverlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static bool rectContains(int ox, int oy, int ow, int oh, int x, int y, int w, int h) {
    return ox <= x && oy <= y && ox + ow >= x + w && oy + oh >= y + h;
}

/* chain[0] is the window, chain[i + 1] the parent of chain[i]; the last entry is a child of the root. */
static bool findAncestors(Swapchain* sc) {
    xcb_connection_t* conn = sc->surface->conn;
    xcb_window_t win = sc->surface->window;
    sc->chainLength = 0;
    while (sc->chainLength < 16) {
        xcb_query_tree_reply_t* tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, win), NULL);
        if (!tree) return false;
        xcb_window_t parent = tree->parent;
        sc->root = tree->root;
        free(tree);
        sc->chain[sc->chainLength++] = win;
        if (parent == sc->root || parent == XCB_WINDOW_NONE) break;
        win = parent;
    }
    sc->clipX0 = sc->clipY0 = INT32_MIN;
    sc->clipX1 = sc->clipY1 = INT32_MAX;
    for (int i = 0; i < sc->chainLength; i++) {
        xcb_window_t parent = i + 1 < sc->chainLength ? sc->chain[i + 1] : sc->root;
        xcb_translate_coordinates_reply_t* pos =
            xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, parent, sc->root, 0, 0), NULL);
        if (!pos) return false;
        sc->chainX[i] = pos->dst_x;   /* origin of chain[i]'s parent */
        sc->chainY[i] = pos->dst_y;
        free(pos);
        if (i + 1 < sc->chainLength) {
            xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, parent), NULL);
            if (!g) return false;
            if (sc->chainX[i] > sc->clipX0) sc->clipX0 = sc->chainX[i];
            if (sc->chainY[i] > sc->clipY0) sc->clipY0 = sc->chainY[i];
            if (sc->chainX[i] + g->width < sc->clipX1) sc->clipX1 = sc->chainX[i] + g->width;
            if (sc->chainY[i] + g->height < sc->clipY1) sc->clipY1 = sc->chainY[i] + g->height;
            free(g);
        }
    }
    sc->chainAge = 0;
    return true;
}

/* The top-level window holding the window (the root's child), sent with the stacking key. */
static uint32_t topLevelOf(const xcb_window_t* chain, int chainLength) {
    return chainLength > 0 ? chain[chainLength - 1] : 0;
}

/* Whether another window of this process shows its frames on a compositor layer that covers all of
 * `win` (at ox,oy ow x oh in root coordinates): `win` is that window or one of its ancestors. */
static bool coveredByOwnLayer(Swapchain* sc, xcb_window_t win, int ox, int oy, int ow, int oh) {
    bool found = false;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS && !found; i++) {
        Swapchain* other = registry[i];
        if (!other || other == sc || other->pub.chainLength == 0) continue;
        if (!other->pub.shown && (other->zeroCopy != 1 || other->lastPath == PATH_OCCLUDED || other->lastPath == PATH_X11)) continue;
        /* Stacking keys only tell the four root-most levels apart. */
        if (sc->chainLength > 4 || other->pub.chainLength > 4) continue;
        if (!rectContains(other->pub.rect[0], other->pub.rect[1], other->pub.rect[2], other->pub.rect[3], ox, oy, ow, oh)) continue;
        if (other->pub.window == win) found = true;
        for (int j = 0; j < other->pub.chainLength && !found; j++) if (other->pub.chain[j] == win) found = true;
    }
    pthread_mutex_unlock(&registryLock);
    return found;
}

/* The root's children in stacking order, hashed. Never 0. */
static uint32_t rootOrderOf(const xcb_query_tree_reply_t* tree) {
    const xcb_window_t* children = xcb_query_tree_children(tree);
    int count = xcb_query_tree_children_length(tree);
    uint32_t hash = 2166136261u;
    for (int k = 0; k < count; k++) hash = (hash ^ children[k]) * 16777619u;
    return hash ? hash : 1;
}

/* A parent smaller than the window clips it. Wine puts a game's Vulkan window in a 1x1 parent
 * when it shows the frames elsewhere (A.V.A.: a 1024x768 client window at 0,0, its frames copied
 * into the game window in the middle of the screen). */
static bool clippedByParent(const Swapchain* sc, int x, int y, int w, int h) {
    return x < sc->clipX0 || y < sc->clipY0 || x + w > sc->clipX1 || y + h > sc->clipY1;
}

/* At every level from the window up to its top-level window, a mapped sibling stacked above it that
 * overlaps the window hides part of it, and so does a mapped child of the window. */
/* The first requests of an occlusion check, sent ahead so that they share a round trip with the
 * window's geometry (presentZeroCopyLocked); then collected or discarded. */
typedef struct OcclusionQuery {
    int levels;
    xcb_get_window_attributes_cookie_t selfAttrs;
    xcb_query_tree_cookie_t trees[17];
} OcclusionQuery;

static void occlusionSend(Swapchain* sc, OcclusionQuery* q) {
    xcb_connection_t* conn = sc->surface->conn;
    q->levels = sc->chainLength;
    q->selfAttrs = xcb_get_window_attributes(conn, sc->chain[0]);
    for (int i = 0; i < q->levels; i++) q->trees[i] = xcb_query_tree(conn, i + 1 < q->levels ? sc->chain[i + 1] : sc->root);
    q->trees[q->levels] = xcb_query_tree(conn, sc->chain[0]);
}

static void occlusionDiscard(Swapchain* sc, const OcclusionQuery* q) {
    xcb_connection_t* conn = sc->surface->conn;
    xcb_discard_reply(conn, q->selfAttrs.sequence);
    for (int i = 0; i <= q->levels; i++) xcb_discard_reply(conn, q->trees[i].sequence);
}

static bool occlusionCollect(Swapchain* sc, const OcclusionQuery* q, int x, int y, int w, int h, bool* stale, bool* covered) {
    xcb_connection_t* conn = sc->surface->conn;
    int levels = q->levels;
    xcb_get_window_attributes_cookie_t selfAttrs = q->selfAttrs;
    xcb_query_tree_reply_t* replies[17];
    bool ok = true;
    for (int i = 0; i <= levels; i++) {
        replies[i] = xcb_query_tree_reply(conn, q->trees[i], NULL);
        if (!replies[i]) ok = false;
    }
    sc->rootOrder = levels > 0 && replies[levels - 1] ? rootOrderOf(replies[levels - 1]) : 0;
    *stale = false;
    int total = 0;
    xcb_window_t candidates[256];
    int16_t originX[256], originY[256];
    uint64_t key = 0;
    for (int i = 0; ok && i < levels; i++) {
        xcb_window_t* children = xcb_query_tree_children(replies[i]);
        int count = xcb_query_tree_children_length(replies[i]), mine = -1;
        for (int k = 0; k < count; k++) if (children[k] == sc->chain[i]) mine = k;
        if (mine < 0) {
            *stale = true;
            break;
        }
        /* 16 bits per level for the four levels nearest the root (levels - 1 is the root's child). */
        int fromRoot = levels - 1 - i;
        if (fromRoot < 4) key |= (uint64_t)(mine + 1 > 0xffff ? 0xffff : mine + 1) << (48 - 16 * fromRoot);
        for (int k = mine + 1; k < count && total < 256; k++) {
            candidates[total] = children[k];
            originX[total] = sc->chainX[i];
            originY[total] = sc->chainY[i];
            total++;
        }
    }
    if (ok && !*stale) {
        xcb_window_t* children = xcb_query_tree_children(replies[levels]);
        int count = xcb_query_tree_children_length(replies[levels]);
        for (int k = 0; k < count && total < 256; k++) {
            candidates[total] = children[k];
            originX[total] = (int16_t)x;
            originY[total] = (int16_t)y;
            total++;
        }
    }
    for (int i = 0; i <= levels; i++) free(replies[i]);
    if (ok && !*stale) sc->stackKey = key;
    xcb_get_window_attributes_reply_t* self = xcb_get_window_attributes_reply(conn, selfAttrs, NULL);
    /* Viewable means the window and all its ancestors are mapped. */
    *covered = !self || self->map_state != XCB_MAP_STATE_VIEWABLE;
    free(self);
    if (!ok || *stale) return true;

    bool occluded = *covered;
    sc->blocker = XCB_NONE;
    if (total > 0) {
        xcb_get_window_attributes_cookie_t attrs[total];
        xcb_get_geometry_cookie_t geos[total];
        for (int k = 0; k < total; k++) {
            attrs[k] = xcb_get_window_attributes(conn, candidates[k]);
            geos[k] = xcb_get_geometry(conn, candidates[k]);
        }
        for (int k = 0; k < total; k++) {
            xcb_get_window_attributes_reply_t* a = xcb_get_window_attributes_reply(conn, attrs[k], NULL);
            xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(conn, geos[k], NULL);
            /* Input-only windows draw nothing; a 1x1 window (wine and Chromium keep such helpers mapped)
             * would cost a whole-screen copy per frame to show one pixel. */
            if (a && g && a->map_state == XCB_MAP_STATE_VIEWABLE && a->_class != XCB_WINDOW_CLASS_INPUT_ONLY && (g->width > 1 || g->height > 1)) {
                int ox = originX[k] + g->x, oy = originY[k] + g->y;
                int ow = g->width + 2 * g->border_width, oh = g->height + 2 * g->border_width;
                if (rectsOverlap(x, y, w, h, ox, oy, ow, oh) && !coveredByOwnLayer(sc, candidates[k], ox, oy, ow, oh)) {
                    if (!occluded) {
                        sc->blocker = candidates[k];
                        sc->blockerRect[0] = (int16_t)ox; sc->blockerRect[1] = (int16_t)oy;
                        sc->blockerRect[2] = (int16_t)ow; sc->blockerRect[3] = (int16_t)oh;
                    }
                    occluded = true;
                }
                /* What lies over a clipped window's place says nothing: its frames are shown elsewhere
                 * (Steam raised over Wine's 1x1 parent froze A.V.A.'s picture after a menu). */
                if (rectContains(ox, oy, ow, oh, x, y, w, h) && !clippedByParent(sc, x, y, w, h)) *covered = true;
            }
            free(a);
            free(g);
        }
    }
    return occluded;
}

static bool checkOccludedOnce(Swapchain* sc, int x, int y, int w, int h, bool* stale, bool* covered) {
    OcclusionQuery q;
    occlusionSend(sc, &q);
    return occlusionCollect(sc, &q, x, y, w, h, stale, covered);
}

/* Whether checkOccluded will use the ancestors it has (a query sent now stays valid). */
static bool ancestorsCurrent(Swapchain* sc) {
    return sc->chainLength > 0 && sc->chainAge < 120;
}

/* `sent`: the first requests, already sent with sc's ancestors (ancestorsCurrent), or NULL. */
static bool checkOccluded(Swapchain* sc, int x, int y, int w, int h, const OcclusionQuery* sent) {
    sc->covered = false;
    if (sc->chainLength == 0 || ++sc->chainAge > 120) {
        if (sent) occlusionDiscard(sc, sent);
        sent = NULL;
        if (!findAncestors(sc)) return true;
    }
    bool stale, covered;
    bool occluded = sent ? occlusionCollect(sc, sent, x, y, w, h, &stale, &covered) : checkOccludedOnce(sc, x, y, w, h, &stale, &covered);
    if (stale) {
        if (!findAncestors(sc)) return true;
        occluded = checkOccludedOnce(sc, x, y, w, h, &stale, &covered);
    }
    sc->covered = !stale && covered;
    return occluded;
}

/* Whether the window is entirely hidden now, in sc->covered. */
static void checkCoverage(Swapchain* sc) {
    xcb_connection_t* conn = sc->surface->conn;
    xcb_get_geometry_reply_t* geo = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, sc->surface->window), NULL);
    if (!geo) {
        sc->covered = false;
        return;
    }
    xcb_translate_coordinates_reply_t* pos =
        xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, sc->surface->window, geo->root, 0, 0), NULL);
    if (pos) {
        sc->root = geo->root;
        checkOccluded(sc, pos->dst_x, pos->dst_y, geo->width, geo->height, NULL);
    }
    else sc->covered = false;
    free(pos);
    free(geo);
}

/* For swapchains on the X11 path: whether the window is entirely hidden, re-checked every frame
 * while it is and every 10 frames otherwise. */
static void updateCoverage(Swapchain* sc) {
    if (sc->coverCountdown > 0 && !sc->covered) {
        sc->coverCountdown--;
        return;
    }
    sc->coverCountdown = 10;
    checkCoverage(sc);
}

/* Whether the root's children changed since the window was found covered; the new order is kept.
 * One round trip. */
static bool rootRestacked(Swapchain* sc) {
    if (!sc->rootOrder || !sc->root) return false;
    xcb_connection_t* conn = sc->surface->conn;
    xcb_query_tree_reply_t* now = xcb_query_tree_reply(conn, xcb_query_tree(conn, sc->root), NULL);
    if (!now) return false;
    uint32_t order = rootOrderOf(now);
    free(now);
    bool changed = order != sc->rootOrder;
    sc->rootOrder = order;
    return changed;
}

static double waitNow(void* ctx) {
    return nowSeconds();
}

static void waitSleep(void* ctx, double seconds) {
    struct timespec ts = {(time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9)};
    nanosleep(&ts, NULL);
}

static bool waitRestacked(void* ctx) {
    return rootRestacked(ctx);
}

static void sweepLayers(void);

/* The sweep of the other windows' layers runs at the pace a present used to give it (one per frame). */
static bool waitHidden(void* ctx) {
    Swapchain* sc = ctx;
    sweepLayers();
    checkCoverage(sc);
    return sc->covered;
}

static const CoveredWaitOps coveredWaitOps = {waitNow, waitSleep, waitRestacked, waitHidden};

static int coveredFps(void) {
    static int fps;
    if (!fps) {
        fps = covered_fps_parse(getenv("VORTEK_COVERED_FPS"));
        fprintf(stderr, "hybris-x11: frames of hidden windows paced to %d a second (VORTEK_COVERED_FPS)\n", fps);
    }
    return fps;
}

/* A hidden window's frame: its semaphores are waited for and nothing is copied or drawn, and the
 * present returns when the window's next frame is due (../vortek/src/covered_pacing.h). */
static VkResult presentCovered(Swapchain* sc, VkQueue queue, const VkPresentInfoKHR* presentInfo, bool* waitedSemaphores) {
    Device* device = sc->device;
    double checked = nowSeconds();
    VkPipelineStageFlags waitStages[presentInfo->waitSemaphoreCount + 1];
    for (uint32_t s = 0; s < presentInfo->waitSemaphoreCount; s++) waitStages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!*waitedSemaphores) {
        submitInfo.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = presentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        *waitedSemaphores = true;
    }
    VkResult result = icd_queue_submit(device, queue, 1, &submitInfo, sc->fence);
    if (result == VK_SUCCESS) result = device->WaitForFences(device->handle, 1, &sc->fence, VK_TRUE, UINT64_MAX);
    if (result == VK_SUCCESS) device->ResetFences(device->handle, 1, &sc->fence);
    int fps = coveredFps();
    bool shared = fps < COVERED_SHARED_FPS && shownOnThisThread(sc, nowSeconds());
    if (shared != sc->pacedShared) {
        sc->pacedShared = shared;
        fprintf(stderr, "hybris-x11: window 0x%x %ux%u hidden, %d frames a second%s (t=%.3f)\n", sc->surface->window, sc->extent.width,
                sc->extent.height, shared ? COVERED_SHARED_FPS : fps, shared ? ": a shown window is drawn on the same thread" : "",
                nowSeconds());
    }
    sc->lastCoveredFrame = covered_wait(&coveredWaitOps, sc, sc->lastCoveredFrame + covered_interval(fps, shared), checked);
    return result;
}

/* Whether a present to a w x h window reports VK_SUBOPTIMAL_KHR (swapchain_loop.h). */
static bool reportSuboptimal(Swapchain* sc, uint32_t w, uint32_t h) {
    int report = swapchain_loop_suboptimal(&sc->quiet, &sc->quietW, &sc->quietH, sc->extent, w, h);
    if (report < 0) {
        swapchain_loop_reset(sc->surface->window, sc->loopKey);
        fprintf(stderr, "hybris-wsi: window 0x%x is now %ux%u (was %ux%u): swapchain %ux%u reports VK_SUBOPTIMAL_KHR again\n",
                sc->surface->window, w, h, sc->quietW, sc->quietH, sc->extent.width, sc->extent.height);
    }
    return report != 0;
}

/* VK_ERROR_FEATURE_NOT_PRESENT (never again for this swapchain) or VK_NOT_READY (not this frame):
 * the frame must take the X11 path, the semaphores are untouched. */
static VkResult presentZeroCopyLocked(Swapchain* sc, VkQueue queue, uint32_t imageIndex, const VkSemaphore* waits, uint32_t waitCount,
                                      bool* suboptimal) {
    if (sc->zeroCopy < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    sc->x11Why = "a window query failed";
    xcb_connection_t* conn = sc->surface->conn;
    /* The window's geometry, its position (relative to the root it had last frame) and the first
     * requests of the occlusion check go out together: one round trip on an unchanged window tree. */
    xcb_get_geometry_cookie_t geoCookie = xcb_get_geometry(conn, sc->surface->window);
    xcb_window_t lastRoot = sc->root;
    xcb_translate_coordinates_cookie_t posCookie = {0};
    if (lastRoot) posCookie = xcb_translate_coordinates(conn, sc->surface->window, lastRoot, 0, 0);
    OcclusionQuery query;
    bool sent = lastRoot && sc->rootW && ancestorsCurrent(sc);
    if (sent) occlusionSend(sc, &query);
    xcb_get_geometry_reply_t* geo = xcb_get_geometry_reply(conn, geoCookie, NULL);
    xcb_translate_coordinates_reply_t* pos = NULL;
    if (geo && lastRoot && geo->root == lastRoot) pos = xcb_translate_coordinates_reply(conn, posCookie, NULL);
    else {
        if (lastRoot) xcb_discard_reply(conn, posCookie.sequence);
        if (sent) occlusionDiscard(sc, &query);
        sent = false;
        if (geo) pos = xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, sc->surface->window, geo->root, 0, 0), NULL);
    }
    if (!geo || !pos || geo->depth == 32 || geo->width <= 0 || geo->height <= 0) {
        if (sent) occlusionDiscard(sc, &query);
        VkResult result = VK_NOT_READY;
        if (geo && pos && geo->depth == 32) {
            /* A window with alpha is composited with what lies below; the compositor's layer is opaque. */
            sc->zeroCopy = -1;
            sc->x11Why = "the window has alpha";
            result = VK_ERROR_FEATURE_NOT_PRESENT;
        }
        free(pos);
        free(geo);
        return result;
    }
    int x = pos->dst_x, y = pos->dst_y, w = geo->width, h = geo->height;
    *suboptimal = reportSuboptimal(sc, geo->width, geo->height);
    xcb_window_t root = geo->root;
    free(pos);
    free(geo);
    if (!sc->rootW || root != sc->root) {
        xcb_get_geometry_reply_t* rootGeo = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, root), NULL);
        if (!rootGeo) return VK_NOT_READY;
        sc->root = root;
        sc->rootW = rootGeo->width;
        sc->rootH = rootGeo->height;
        free(rootGeo);
    }
    /* Fully covered (also by a window of ours on a layer above): paced on the covered path. */
    bool occluded = checkOccluded(sc, x, y, w, h, sent ? &query : NULL) || sc->covered;
    /* A layer of a clipped window at its own place would show Wine's frames in the wrong place, over
     * the game window that takes the clicks. The X11 path lets Wine copy them. */
    if (!sc->covered && clippedByParent(sc, x, y, w, h)) {
        sc->occluded = false;
        sc->x11Why = "a smaller parent window clips it (Wine shows its frames in another window)";
        return VK_NOT_READY;
    }
    sc->occluded = occluded;
    if (sc->occluded) return VK_NOT_READY;
    VkResult result = presentToCompositor(sc, queue, imageIndex, waits, waitCount, x, y, w, h, sc->rootW, sc->rootH, sc->stackKey,
                                          topLevelOf(sc->chain, sc->chainLength));
    if (result == VK_SUCCESS) publishLayer(sc, true, x, y, w, h);
    if (result == VK_ERROR_FEATURE_NOT_PRESENT) {
        if (sc->zeroCopy == 1) fprintf(stderr, "hybris-x11: zero-copy stopped, presenting through X11\n");
        sc->zeroCopy = -1;
        sc->x11Why = !sc->owner ? "no zero-copy for this swapchain" : "the compositor declined it";
    }
    else if (result == VK_SUCCESS && sc->zeroCopy == 0) {
        sc->zeroCopy = 1;
        fprintf(stderr, "hybris-x11: window 0x%x %ux%u presented zero-copy by the compositor\n", sc->surface->window, sc->extent.width,
                sc->extent.height);
    }
    return result;
}

static VkResult presentZeroCopy(Swapchain* sc, VkQueue queue, uint32_t imageIndex, const VkSemaphore* waits, uint32_t waitCount,
                                bool* suboptimal) {
    pthread_mutex_lock(&sc->presentLock);
    VkResult result = presentZeroCopyLocked(sc, queue, imageIndex, waits, waitCount, suboptimal);
    pthread_mutex_unlock(&sc->presentLock);
    return result;
}

/* Path changes carry a CLOCK_MONOTONIC time, to line them up with the compositor's trace. */
static void notePath(Swapchain* sc, int path, int depth) {
    if (path == sc->lastPath) return;
    sc->lastPath = path;
    if (path != PATH_ZERO_COPY) publishLayer(sc, false, 0, 0, 0, 0);
    const char* what = path == PATH_ZERO_COPY ? "zero-copy"
                     : path == PATH_COVERED   ? "hidden, frames paced"
                     : path == PATH_OCCLUDED  ? "through X11, partly covered"
                                              : "through X11, the compositor's layer is not available";
    double t = nowSeconds();
    if (path == PATH_OCCLUDED && sc->blocker)
        fprintf(stderr, "hybris-x11: window 0x%x %ux%u now %s by 0x%x at %d,%d %dx%d (t=%.3f)\n", sc->surface->window, sc->extent.width,
                sc->extent.height, what, sc->blocker, sc->blockerRect[0], sc->blockerRect[1], sc->blockerRect[2], sc->blockerRect[3], t);
    else if (path == PATH_X11)
        fprintf(stderr, "hybris-x11: window 0x%x %ux%u now %s: %s, depth %d (t=%.3f)\n", sc->surface->window, sc->extent.width,
                sc->extent.height, what, sc->x11Why ? sc->x11Why : "zero-copy not tried", depth, t);
    else
        fprintf(stderr, "hybris-x11: window 0x%x %ux%u now %s (t=%.3f)\n", sc->surface->window, sc->extent.width, sc->extent.height, what, t);
}

static void countFrame(Swapchain* sc) {
    sc->frames++;
    sc->statFrames++;
    static int statEvery = -1;
    if (statEvery < 0) statEvery = icd_env_int("VORTEK_X11_STATS", 0);
    if (statEvery > 0) {
        double now = nowSeconds();
        if (sc->statStart == 0) sc->statStart = now;
        if (now - sc->statStart >= statEvery) {
            fprintf(stderr, "hybris-x11: %llu frames, %.1f fps%s\n", (unsigned long long)sc->frames, sc->statFrames / (now - sc->statStart),
                    sc->covered ? " (hidden, paced)" : sc->zeroCopy == 1 && !sc->occluded ? " (zero-copy)" : "");
            sc->statStart = now;
            sc->statFrames = 0;
        }
    }
}

/* The stacking key of a window from its ancestors, encoded as in checkOccludedOnce. */
static bool stackKeyOf(xcb_connection_t* conn, const xcb_window_t* chain, int levels, xcb_window_t root, uint64_t* key) {
    if (levels <= 0 || levels > 16) return false;
    xcb_query_tree_cookie_t trees[16];
    for (int i = 0; i < levels; i++) trees[i] = xcb_query_tree(conn, i + 1 < levels ? chain[i + 1] : root);
    bool ok = true;
    *key = 0;
    for (int i = 0; i < levels; i++) {
        xcb_query_tree_reply_t* tree = xcb_query_tree_reply(conn, trees[i], NULL);
        if (!tree) {
            ok = false;
            continue;
        }
        xcb_window_t* children = xcb_query_tree_children(tree);
        int count = xcb_query_tree_children_length(tree), mine = -1;
        for (int k = 0; k < count; k++) if (children[k] == chain[i]) mine = k;
        free(tree);
        if (mine < 0) {
            ok = false;
            continue;
        }
        int fromRoot = levels - 1 - i;
        if (fromRoot < 4) *key |= (uint64_t)(mine + 1 > 0xffff ? 0xffff : mine + 1) << (48 - 16 * fromRoot);
    }
    return ok;
}

/* Idle windows on compositor layers are kept right from any present of this process: an unmapped
 * one's layer is taken down (checked every present), a restacked one gets its new stacking key
 * (checked at most every 100 ms). A swapchain in the middle of its own present is left for later. */
static void sweepLayers(void) {
    static double lastRestack;
    double now = nowSeconds();
    bool restack = now - lastRestack >= 0.1;
    if (restack) lastRestack = now;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        Swapchain* other = registry[i];
        if (!other || !other->pub.shown) continue;
        if (pthread_mutex_trylock(&other->presentLock) != 0) continue;
        xcb_connection_t* conn = other->surface->conn;
        xcb_get_window_attributes_reply_t* a = xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, other->surface->window), NULL);
        bool viewable = a && a->map_state == XCB_MAP_STATE_VIEWABLE;
        free(a);
        uint64_t key;
        if (!viewable) {
            hideLayer(other);
            other->pub.shown = false;
            fprintf(stderr, "hybris-x11: window 0x%x %ux%u is no longer viewable, its layer was taken down (t=%.3f)\n",
                    other->surface->window, other->extent.width, other->extent.height, nowSeconds());
        }
        else if (restack && stackKeyOf(conn, other->pub.chain, other->pub.chainLength, other->pub.root, &key) && key != other->pub.key) {
            restackLayer(other, key, topLevelOf(other->pub.chain, other->pub.chainLength));
            other->pub.key = key;
        }
        pthread_mutex_unlock(&other->presentLock);
    }
    pthread_mutex_unlock(&registryLock);
}

/* One swapchain of a present; *waitedSemaphores tells whether the present's semaphores were taken. */
static VkResult presentOne(Swapchain* sc, VkQueue queue, uint32_t family, const VkPresentInfoKHR* presentInfo, uint32_t i,
                           bool* waitedSemaphores) {
    Device* device = sc->device;
    uint32_t imageIndex = presentInfo->pImageIndices[i];
    if (*waitedSemaphores && sc->zeroCopy >= 0) sc->x11Why = "another swapchain of this present took the semaphores";
    if (!*waitedSemaphores && sc->zeroCopy >= 0) {
        bool suboptimal = false;
        VkResult zc = presentZeroCopy(sc, queue, imageIndex, presentInfo->pWaitSemaphores, presentInfo->waitSemaphoreCount, &suboptimal);
        if (zc != VK_ERROR_FEATURE_NOT_PRESENT && zc != VK_NOT_READY) {
            if (zc == VK_SUCCESS) {
                *waitedSemaphores = true;
                notePath(sc, PATH_ZERO_COPY, 0);
                noteShown(sc, nowSeconds());
                countFrame(sc);
                if (suboptimal) zc = VK_SUBOPTIMAL_KHR;
            }
            return zc;
        }
    }
    if (!sc->copyReady && !sc->copyFailed) {
        sc->copyReady = setupCopy(sc, family);
        if (!sc->copyReady) {
            fprintf(stderr, "hybris-x11: copy setup failed, frames will not be shown\n");
            releaseCopy(sc);
            sc->copyFailed = true;
        }
    }
    if (!sc->copyReady) {
        /* Nothing can show the frame; its semaphores still have to be waited for. */
        if (*waitedSemaphores) return VK_SUCCESS;
        VkPipelineStageFlags stages[presentInfo->waitSemaphoreCount + 1];
        for (uint32_t s = 0; s < presentInfo->waitSemaphoreCount; s++) stages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = presentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = stages;
        *waitedSemaphores = true;
        return icd_queue_submit(device, queue, 1, &submitInfo, VK_NULL_HANDLE);
    }
    if (sc->zeroCopy < 0 || sc->occluded) {
        if (sc->zeroCopy < 0) updateCoverage(sc);
        if (sc->covered) {
            notePath(sc, PATH_COVERED, 0);
            VkResult result = presentCovered(sc, queue, presentInfo, waitedSemaphores);
            countFrame(sc);
            return result;
        }
    }
    xcb_get_geometry_cookie_t geometryCookie = xcb_get_geometry(sc->surface->conn, sc->surface->window);
    xcb_flush(sc->surface->conn);

    VkPipelineStageFlags waitStages[presentInfo->waitSemaphoreCount + 1];
    for (uint32_t s = 0; s < presentInfo->waitSemaphoreCount; s++) waitStages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!*waitedSemaphores) {
        submitInfo.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = presentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        *waitedSemaphores = true;
    }
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &sc->commandBuffers[imageIndex % sc->imageCount];
    VkResult result = icd_queue_submit(device, queue, 1, &submitInfo, sc->fence);
    if (result == VK_SUCCESS) result = device->WaitForFences(device->handle, 1, &sc->fence, VK_TRUE, UINT64_MAX);
    if (result == VK_SUCCESS) device->ResetFences(device->handle, 1, &sc->fence);

    xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(sc->surface->conn, geometryCookie, NULL);
    if (!geometry) {
        if (result == VK_SUCCESS) result = VK_ERROR_SURFACE_LOST_KHR;
    }
    else if (result == VK_SUCCESS) {
        if (!sc->coherent) {
            VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = sc->memory;
            range.size = VK_WHOLE_SIZE;
            device->InvalidateMappedMemoryRanges(device->handle, 1, &range);
        }
        putFrame(sc, geometry->depth);
        notePath(sc, sc->zeroCopy >= 0 && sc->occluded ? PATH_OCCLUDED : PATH_X11, geometry->depth);
        noteShown(sc, nowSeconds());
        if (reportSuboptimal(sc, geometry->width, geometry->height)) {
            result = VK_SUBOPTIMAL_KHR;
            if (!sc->suboptimalLogged) {
                sc->suboptimalLogged = true;
                fprintf(stderr, "hybris-x11: window 0x%x is %ux%u, swapchain %ux%u: suboptimal\n", sc->surface->window, geometry->width,
                        geometry->height, sc->extent.width, sc->extent.height);
            }
        }
    }
    free(geometry);
    countFrame(sc);
    return result;
}

/* ---- swapchain entry points ---- */

static void destroySwapchain(Swapchain* sc) {
    Device* device = sc->device;
    registryRemove(sc);
    unregisterZeroCopy(sc);
    if (sc->frames)
        fprintf(stderr, "hybris-wsi: swapchain %ux%u destroyed after %llu presents\n", sc->extent.width, sc->extent.height,
                (unsigned long long)sc->presentCount);
    for (int i = 0; i < ACQUIRE_WAITS; i++) {
        AcquireWait* wait = &sc->acquireWaits[i];
        if (wait->pending) device->WaitForFences(device->handle, 1, &wait->fence, VK_TRUE, 2000000000ull);
        if (wait->semaphore) device->DestroySemaphore(device->handle, wait->semaphore, NULL);
        if (wait->fence) device->DestroyFence(device->handle, wait->fence, NULL);
    }
    releaseCopy(sc);
    for (uint32_t i = 0; i < sc->imageCount; i++) {
        if (sc->images[i]) device->DestroyImage(device->handle, sc->images[i], NULL);
        if (sc->memories[i]) device->FreeMemory(device->handle, sc->memories[i], NULL);
        if (sc->hardwareBuffers[i]) AHardwareBuffer_release(sc->hardwareBuffers[i]);
    }
    pthread_mutex_destroy(&sc->presentLock);
    free(sc->images);
    free(sc->memories);
    free(sc->hardwareBuffers);
    free(sc);
}

static int recreateLimit(void) {
    static int limit = -1;
    if (limit < 0) {
        int value = icd_env_int("VORTEK_RECREATE_LIMIT", 10);
        limit = value < 0 ? 0 : value;
    }
    return limit;
}

static VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice handle, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator,
                                              VkSwapchainKHR* out) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (info->oldSwapchain) swapchainOf(info->oldSwapchain)->retired = true;
    Swapchain* sc = calloc(1, sizeof(*sc));
    uint32_t count = info->minImageCount < 1 ? 1 : info->minImageCount > MAX_IMAGES ? MAX_IMAGES : info->minImageCount;
    if (sc) {
        sc->images = calloc(count, sizeof(VkImage));
        sc->memories = calloc(count, sizeof(VkDeviceMemory));
        sc->hardwareBuffers = calloc(count, sizeof(AHardwareBuffer*));
    }
    if (!sc || !sc->images || !sc->memories || !sc->hardwareBuffers) {
        if (sc) {
            free(sc->images);
            free(sc->memories);
            free(sc->hardwareBuffers);
            free(sc);
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    sc->device = device;
    sc->surface = surfaceOf(info->surface);
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;
    sc->usage = info->imageUsage;
    sc->imageCount = count;
    for (int i = 0; i < SHM_BUFFER_COUNT; i++) sc->shm[i].shmId = -1;
    pthread_mutex_init(&sc->presentLock, NULL);
    switch (sc->format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: sc->swapRB = false; break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: sc->swapRB = true; break;
        default: sc->copyFailed = true; break;
    }
    for (uint32_t i = 0; i < count; i++) {
        VkResult result = createImage(sc, info, i);
        if (result != VK_SUCCESS) {
            pthread_mutex_destroy(&sc->presentLock);
            for (uint32_t j = 0; j <= i; j++) {
                if (sc->images[j]) device->DestroyImage(handle, sc->images[j], NULL);
                if (sc->memories[j]) device->FreeMemory(handle, sc->memories[j], NULL);
                if (sc->hardwareBuffers[j]) AHardwareBuffer_release(sc->hardwareBuffers[j]);
            }
            free(sc->images);
            free(sc->memories);
            free(sc->hardwareBuffers);
            free(sc);
            return result;
        }
    }
    registerZeroCopy(sc);
    registryAdd(sc);
    uint32_t loopCount;
    bool justQuiet;
    sc->loopKey = swapchain_loop_key(info);
    sc->quiet = swapchain_loop_created(sc->surface->window, sc->loopKey, nowSeconds(), recreateLimit(), &loopCount, &justQuiet);
    /* A swapchain recreated in a loop is logged once, not each time. */
    if (!sc->quiet || justQuiet)
        fprintf(stderr, "hybris-wsi: swapchain %ux%u format %d, %u images, window 0x%x%s, present mode %d\n", sc->extent.width,
                sc->extent.height, sc->format, sc->imageCount, sc->surface->window,
                sc->zcUsable ? " (zero-copy)" : sc->hardwareBuffers[0] ? " (AHardwareBuffer)" : "", info->presentMode);
    if (justQuiet)
        fprintf(stderr, "hybris-wsi: swapchain %ux%u for window 0x%x created %u times within a second with the same parameters: "
                "its size mismatch is no longer reported as VK_SUBOPTIMAL_KHR while the window keeps its size\n",
                sc->extent.width, sc->extent.height, sc->surface->window, loopCount);
    *out = (VkSwapchainKHR)(uintptr_t)sc;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroySwapchainKHR(VkDevice handle, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
    if (swapchain) destroySwapchain(swapchainOf(swapchain));
}

static VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice handle, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) {
    Swapchain* sc = swapchainOf(swapchain);
    if (!images) {
        *count = sc->imageCount;
        return VK_SUCCESS;
    }
    uint32_t n = MIN(*count, sc->imageCount);
    memcpy(images, sc->images, n * sizeof(VkImage));
    *count = n;
    return n < sc->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice handle, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                               VkFence fence, uint32_t* index) {
    return acquire(swapchainOf(swapchain), timeout, semaphore, fence, index);
}

static VkResult VKAPI_CALL AcquireNextImage2KHR(VkDevice handle, const VkAcquireNextImageInfoKHR* info, uint32_t* index) {
    return acquire(swapchainOf(info->swapchain), info->timeout, info->semaphore, info->fence, index);
}

static VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    uint32_t family = 0;
    Device* device = icd_device_of_queue(queue, &family);
    if (!device) return VK_ERROR_DEVICE_LOST;
    sweepLayers();
    VkResult overall = VK_SUCCESS;
    bool waitedSemaphores = false;
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        Swapchain* sc = swapchainOf(info->pSwapchains[i]);
        VkResult result = presentOne(sc, queue, family, info, i, &waitedSemaphores);
        presentDone(sc, info->pImageIndices[i]);
        if (info->pResults) info->pResults[i] = result;
        if (result < 0 || (overall == VK_SUCCESS && result != VK_SUCCESS)) {
            if (overall >= 0) overall = result;
        }
    }
    mali_presented();
    return overall;
}

static VkResult VKAPI_CALL GetDeviceGroupPresentCapabilitiesKHR(VkDevice handle, VkDeviceGroupPresentCapabilitiesKHR* caps) {
    memset(caps->presentMask, 0, sizeof(caps->presentMask));
    caps->presentMask[0] = 1;
    caps->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL GetDeviceGroupSurfacePresentModesKHR(VkDevice handle, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR* modes) {
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

void wsi_device_destroyed(Device* device) {
    Swapchain* left[MAX_TRACKED_SWAPCHAINS];
    int count = 0;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++)
        if (registry[i] && registry[i]->device == device) left[count++] = registry[i];
    pthread_mutex_unlock(&registryLock);
    for (int i = 0; i < count; i++) destroySwapchain(left[i]);
}

#define PROC(name) {"vk" #name, (PFN_vkVoidFunction)name}
static const struct {
    const char* name;
    PFN_vkVoidFunction func;
} instanceProcs[] = {
    PROC(CreateXcbSurfaceKHR),
    PROC(CreateXlibSurfaceKHR),
    PROC(DestroySurfaceKHR),
    PROC(GetPhysicalDeviceXcbPresentationSupportKHR),
    PROC(GetPhysicalDeviceXlibPresentationSupportKHR),
    PROC(GetPhysicalDeviceSurfaceSupportKHR),
    PROC(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    PROC(GetPhysicalDeviceSurfaceCapabilities2KHR),
    PROC(GetPhysicalDeviceSurfaceFormatsKHR),
    PROC(GetPhysicalDeviceSurfaceFormats2KHR),
    PROC(GetPhysicalDeviceSurfacePresentModesKHR),
    PROC(GetPhysicalDevicePresentRectanglesKHR),
}, deviceProcs[] = {
    PROC(CreateSwapchainKHR),
    PROC(DestroySwapchainKHR),
    PROC(GetSwapchainImagesKHR),
    PROC(AcquireNextImageKHR),
    PROC(AcquireNextImage2KHR),
    PROC(QueuePresentKHR),
    PROC(GetDeviceGroupPresentCapabilitiesKHR),
    PROC(GetDeviceGroupSurfacePresentModesKHR),
};
#undef PROC

PFN_vkVoidFunction wsi_instance_proc(const char* name) {
    for (size_t i = 0; i < ARRAY_LEN(instanceProcs); i++)
        if (!strcmp(name, instanceProcs[i].name)) return instanceProcs[i].func;
    return NULL;
}

PFN_vkVoidFunction wsi_device_proc(const char* name) {
    for (size_t i = 0; i < ARRAY_LEN(deviceProcs); i++)
        if (!strcmp(name, deviceProcs[i].name)) return deviceProcs[i].func;
    return NULL;
}
