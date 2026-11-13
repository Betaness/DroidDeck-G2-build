/*
 * Software X11 presentation for the standalone Vortek client (glibc side).
 *
 * The server keeps rendering into its own swapchain images (headless_swapchain.c).
 * On vkQueuePresentKHR the client records, through the normal Vortek forwarding,
 * a copy of the presented image into a host-visible buffer whose memory is
 * already shared with this process (vkMapMemory hands us the DMA-BUF / memfd),
 * submits it waiting on the app's present semaphores, waits for the fence and
 * writes the pixels into the X window: MIT-SHM (memfd via ShmAttachFd, or SysV)
 * with xcb_shm_put_image when available, otherwise xcb_put_image in large bands.
 * Pixels cross the CPU once (GPU-shared buffer -> SHM segment, swizzling R/B for
 * RGBA swapchains); with plain PutImage they are written straight from the
 * mapping into the socket.
 *
 * No protocol change: only existing Vulkan requests are used. The server just
 * has to accept any swapchain extent (patches/winlator-0002-x11-present.patch).
 *
 * Environment:
 *   VORTEK_X11_SHM=0     disable MIT-SHM (use PutImage)
 *   VORTEK_X11_STATS=N   print presented frames / fps every N seconds to stderr
 *   VORTEK_RECREATE_LIMIT=N  [mali-g2] swapchains per second with the same parameters for a window
 *                        before a size mismatch stops being reported as VK_SUBOPTIMAL_KHR (default 10,
 *                        0: never; swapchain_loop.h)
 *   VORTEK_COVERED_FPS=N [mali-g2] frames a second of a window that is entirely hidden (default 1;
 *                        covered_pacing.h)
 *
 * [mali-g2] Zero-copy: when the server backs the swapchain with AHardwareBuffers and DroidDeck's
 * compositor is reachable, a present that no other X window covers is handed to the server
 * (REQUEST_CODE_DD_PRESENT_ZERO_COPY) with the window's root rectangle, and the compositor shows the
 * image itself; nothing is copied. Otherwise the frame takes the X11 path below.
 * The compositor has several such layers, ordered by the windows' X stacking: a window stays
 * zero-copy under another window of this process that is zero-copy itself (Steam's side menu
 * over its main view), since that window's layer covers it.
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Vortek (C) brunodev85, LGPL-2.1.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>

#include "vortek.h"
#include "client_x11_present.h"
#include "covered_pacing.h"
#include "droiddeck_requests.h"
#include "swapchain_loop.h"

extern int droiddeck_request(int requestCode, const void* payload, int size);

#define SHM_BUFFER_COUNT 2

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

typedef struct X11Surface {
    xcb_connection_t* conn;
    xcb_window_t window;
} X11Surface;

typedef struct ShmBuffer {
    uint8_t* data;
    uint32_t seg;
    int shmId;           /* SysV id, or -1 for memfd */
    bool pending;
    xcb_get_input_focus_cookie_t sync;
} ShmBuffer;

typedef struct X11Swapchain {
    X11Surface* surface;
    VkDevice device;
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    bool swapRB;
    bool ready;
    bool failed;

    uint32_t imageCount;
    VkImage* images;
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
    /* the window's ancestors up to the root and their origins in root coordinates */
    int chainLength;
    xcb_window_t chain[16];
    int16_t chainX[16], chainY[16];
    /* where the ancestors let the window show, in root coordinates: their rectangles intersected */
    int32_t clipX0, clipY0, clipX1, clipY1;
    uint32_t chainAge;
    /* what kept the last frame off the zero-copy layer, logged when it changes */
    xcb_window_t blocker;
    int16_t blockerRect[4];
    int lastPath;
    const char* x11Why;        /* why frames take the X11 path while the layer is not available */
    bool suboptimalLogged;
    /* Recreated in a loop with the same parameters (swapchain_loop.h): a size mismatch is not reported
     * while the window stays quietW x quietH. */
    uint64_t loopKey;
    bool quiet;
    uint32_t quietW, quietH;
    /* X stacking position, root level first: each level's index among its siblings + 1 */
    uint64_t stackKey;
    /* What other swapchains of this process may read, under registryLock: whether this window's
     * frames are on a compositor layer, where, and the window's ancestors. */
    struct {
        bool shown;
        int32_t rect[4];
        xcb_window_t window, root;
        int chainLength;
        xcb_window_t chain[16];
        uint64_t key;          /* the stacking key the compositor has */
        double shownAt;        /* when a frame of it was last drawn (zero-copy or through X11), 0: never */
        pthread_t shownThread; /* the thread that presented it */
    } pub;
    /* Held across a zero-copy present (request and publishLayer); the sweep only trylocks it, so a
     * HIDE or RESTACK never overtakes a present that is on its way. */
    pthread_mutex_t presentLock;
} X11Swapchain;

enum { PATH_ZERO_COPY = 1, PATH_COVERED, PATH_OCCLUDED, PATH_X11 };

/* Every X11Swapchain of this process, to tell which windows are on compositor layers. */
#define MAX_TRACKED_SWAPCHAINS 64
static pthread_mutex_t registryLock = PTHREAD_MUTEX_INITIALIZER;
static X11Swapchain* registry[MAX_TRACKED_SWAPCHAINS];

static void registryAdd(X11Swapchain* sc) {
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        if (!registry[i]) {
            registry[i] = sc;
            break;
        }
    }
    pthread_mutex_unlock(&registryLock);
}

static void registryRemove(X11Swapchain* sc) {
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) if (registry[i] == sc) registry[i] = NULL;
    pthread_mutex_unlock(&registryLock);
}

static void publishLayer(X11Swapchain* sc, bool shown, int x, int y, int w, int h) {
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

static void noteShown(X11Swapchain* sc, double now) {
    pthread_mutex_lock(&registryLock);
    sc->pub.shownAt = now;
    sc->pub.shownThread = pthread_self();
    pthread_mutex_unlock(&registryLock);
}

/* Whether this thread drew a frame of another window of the process recently (covered_pacing.h). */
static bool shownOnThisThread(X11Swapchain* sc, double now) {
    pthread_t self = pthread_self();
    bool shown = false;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS && !shown; i++) {
        X11Swapchain* other = registry[i];
        if (other && other != sc && other->pub.shownAt > 0)
            shown = covered_shown_recently(now, other->pub.shownAt, pthread_equal(other->pub.shownThread, self));
    }
    pthread_mutex_unlock(&registryLock);
    return shown;
}

static struct {
    bool loaded;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
} vk;

static void loadFunctions() {
    if (vk.loaded) return;
#define LOAD(name) vk.name = (PFN_vk##name)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name)
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetSwapchainImagesKHR);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdCopyImageToBuffer);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(QueueSubmit);
    LOAD(CreateBuffer);
    LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(UnmapMemory);
    LOAD(InvalidateMappedMemoryRanges);
#undef LOAD
    vk.loaded = true;
}

static int envInt(const char* name, int defaultValue) {
    const char* value = getenv(name);
    return value && *value ? atoi(value) : defaultValue;
}

static double nowSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- surfaces ---- */

void X11Present_initXlibSurface(VkObject* surfaceObject, Display* dpy, Window window) {
    static xcb_connection_t* (*getXCBConnection)(Display*) = NULL;
    if (!dpy) return; /* headless: nothing to show (tests/present_test.c) */
    if (!getXCBConnection) {
        void* lib = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (lib) getXCBConnection = dlsym(lib, "XGetXCBConnection");
        if (!getXCBConnection) {
            fprintf(stderr, "vortek-x11: libX11-xcb.so.1 not found, Xlib surface will not be shown\n");
            return;
        }
    }
    X11Surface* surface = calloc(1, sizeof(X11Surface));
    surface->conn = getXCBConnection(dpy);
    surface->window = (xcb_window_t)window;
    surfaceObject->tag = surface;
}

VkResult vt_call_vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    VkObject* surfaceObject = VkObject_create(VK_OBJECT_TYPE_SURFACE_KHR, (uint64_t)pCreateInfo->window);
    if (pCreateInfo->connection) {
        X11Surface* surface = calloc(1, sizeof(X11Surface));
        surface->conn = pCreateInfo->connection;
        surface->window = pCreateInfo->window;
        surfaceObject->tag = surface;
    }
    *pSurface = VkObject_toHandle(surfaceObject);
    return VK_SUCCESS;
}

VkBool32 vt_call_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t* connection, xcb_visualid_t visual_id) {
    return VK_TRUE;
}

void X11Present_destroySurface(VkObject* surfaceObject) {
    if (!VKOBJECT_IS_NULL(surfaceObject)) MEMFREE(surfaceObject->tag);
}

static bool getWindowGeometry(X11Surface* surface, uint32_t* width, uint32_t* height, uint8_t* depth) {
    xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(surface->conn, xcb_get_geometry(surface->conn, surface->window), NULL);
    if (!reply) return false;
    *width = reply->width;
    *height = reply->height;
    if (depth) *depth = reply->depth;
    free(reply);
    return true;
}

void X11Present_fixSurfaceCapabilities(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities) {
    X11Surface* x11Surface = VkObject_fromHandle(surface)->tag;
    uint32_t width, height;
    if (!x11Surface || !getWindowGeometry(x11Surface, &width, &height, NULL)) return;
    capabilities->currentExtent.width = width;
    capabilities->currentExtent.height = height;
    /* [mali-g2] The window's size, as Mesa's X11 WSI and Winlator's vortekrenderer report it: Proton's
     * fullscreen hack (a game's emulated display mode) creates its host swapchain with minImageExtent
     * and scales the game's frames into it. With 1x1 that was a 1x1 swapchain, every present of it
     * SUBOPTIMAL (the window is larger) and DXVK recreated it every frame. */
    capabilities->minImageExtent.width = width;
    capabilities->minImageExtent.height = height;
    capabilities->maxImageExtent.width = MAX(width, 16384);
    capabilities->maxImageExtent.height = MAX(height, 16384);
    capabilities->supportedCompositeAlpha |= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/* ---- bookkeeping ---- */

void X11Present_onCreateDevice(VkPhysicalDevice physicalDevice, VkDevice device) {
    VkObject* deviceObject = VkObject_fromHandle(device);
    if (!VKOBJECT_IS_NULL(deviceObject)) deviceObject->tag = physicalDevice;
}

void X11Present_onGetDeviceQueue(VkQueue queue, uint32_t queueFamilyIndex) {
    VkObject* queueObject = VkObject_fromHandle(queue);
    if (!VKOBJECT_IS_NULL(queueObject)) queueObject->tag = (void*)(uintptr_t)(queueFamilyIndex + 1);
}

/* ---- swapchains ---- */

void X11Present_onCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo, VkSwapchainKHR swapchain) {
    X11Surface* surface = VkObject_fromHandle(createInfo->surface)->tag;
    VkObject* swapchainObject = VkObject_fromHandle(swapchain);
    if (!surface || VKOBJECT_IS_NULL(swapchainObject)) return;

    X11Swapchain* sc = calloc(1, sizeof(X11Swapchain));
    sc->surface = surface;
    sc->device = device;
    sc->handle = swapchain;
    sc->format = createInfo->imageFormat;
    sc->extent = createInfo->imageExtent;
    for (int i = 0; i < SHM_BUFFER_COUNT; i++) sc->shm[i].shmId = -1;
    pthread_mutex_init(&sc->presentLock, NULL);
    registryAdd(sc);
    uint32_t loopCount;
    bool justQuiet;
    static int recreateLimit = -1;
    if (recreateLimit < 0) {
        int value = envInt("VORTEK_RECREATE_LIMIT", 10);
        recreateLimit = value < 0 ? 0 : value;
    }
    sc->loopKey = swapchain_loop_key(createInfo);
    sc->quiet = swapchain_loop_created(surface->window, sc->loopKey, nowSeconds(), recreateLimit, &loopCount, &justQuiet);
    if (justQuiet)
        fprintf(stderr, "vortek-x11: swapchain %ux%u for window 0x%x created %u times within a second with the same parameters: "
                "its size mismatch is no longer reported as VK_SUBOPTIMAL_KHR while the window keeps its size\n",
                createInfo->imageExtent.width, createInfo->imageExtent.height, surface->window, loopCount);
    /* Chromium (zink kopper) recreates its swapchain on resizes, swap-interval changes and
     * SUBOPTIMAL presents: say which, recreation costs a frame or two. */
    if (envInt("VORTEK_X11_STATS", 0) > 0)
        fprintf(stderr, "vortek-x11: swapchain %ux%u for window 0x%x: present mode %d, %u images%s\n",
                createInfo->imageExtent.width, createInfo->imageExtent.height, surface->window,
                createInfo->presentMode, createInfo->minImageCount, createInfo->oldSwapchain ? ", replaces the old one" : "");

    switch (sc->format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            sc->swapRB = false;
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            sc->swapRB = true;
            break;
        default:
            fprintf(stderr, "vortek-x11: unsupported swapchain format %d, not presenting\n", sc->format);
            sc->failed = true;
            break;
    }
    swapchainObject->tag = sc;
}

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, bool* coherent) {
    VkPhysicalDeviceMemoryProperties props;
    vk.GetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    const VkMemoryPropertyFlags wanted[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    for (int w = 0; w < ARRAY_SIZE(wanted); w++) {
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted[w]) == wanted[w]) {
                *coherent = (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    return UINT32_MAX;
}

static void recordCopy(X11Swapchain* sc, VkCommandBuffer cmd, VkImage image) {
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vk.BeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
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
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = sc->extent.width;
    region.imageExtent.height = sc->extent.height;
    region.imageExtent.depth = 1;
    vk.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->buffer, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier bufferBarrier = {0};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = sc->buffer;
    bufferBarrier.size = VK_WHOLE_SIZE;
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 1, &bufferBarrier, 1, &barrier);
    vk.EndCommandBuffer(cmd);
}

static bool attachShm(X11Swapchain* sc, ShmBuffer* shm, bool useFd) {
    xcb_connection_t* conn = sc->surface->conn;
    shm->seg = xcb_generate_id(conn);
    xcb_generic_error_t* error;

    if (useFd) {
        int fd = memfd_create("vortek-x11", MFD_CLOEXEC);
        if (fd < 0) return false;
        if (ftruncate(fd, sc->frameSize) != 0) { close(fd); return false; }
        void* data = mmap(NULL, sc->frameSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) { close(fd); return false; }
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
    if (data == (void*)-1) { shmctl(shmId, IPC_RMID, NULL); return false; }
    error = xcb_request_check(conn, xcb_shm_attach_checked(conn, shm->seg, shmId, 1));
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

static void detachShm(X11Swapchain* sc, ShmBuffer* shm) {
    if (!shm->data) return;
    xcb_connection_t* conn = sc->surface->conn;
    if (shm->pending) free(xcb_get_input_focus_reply(conn, shm->sync, NULL));
    xcb_shm_detach(conn, shm->seg);
    if (shm->shmId >= 0) shmdt(shm->data);
    else munmap(shm->data, sc->frameSize);
    shm->data = NULL;
}

static const char* setupX11(X11Swapchain* sc) {
    xcb_connection_t* conn = sc->surface->conn;
    sc->gc = xcb_generate_id(conn);
    xcb_create_gc(conn, sc->gc, sc->surface->window, 0, NULL);

    uint32_t maxBytes = xcb_get_maximum_request_length(conn) * 4;
    sc->maxRowsPerPut = MAX((maxBytes - 64) / (sc->extent.width * 4), 1);

    if (!envInt("VORTEK_X11_SHM", 1)) return "PutImage (VORTEK_X11_SHM=0)";

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

static bool setupSwapchain(X11Swapchain* sc, VkQueue queue) {
    loadFunctions();
    VkDevice device = sc->device;
    VkPhysicalDevice physicalDevice = VkObject_fromHandle(device)->tag;
    if (!physicalDevice) {
        fprintf(stderr, "vortek-x11: unknown physical device for VkDevice\n");
        return false;
    }

    if (vk.GetSwapchainImagesKHR(device, sc->handle, &sc->imageCount, NULL) != VK_SUCCESS || sc->imageCount == 0) return false;
    sc->images = calloc(sc->imageCount, sizeof(VkImage));
    if (vk.GetSwapchainImagesKHR(device, sc->handle, &sc->imageCount, sc->images) != VK_SUCCESS) return false;

    sc->frameSize = (size_t)sc->extent.width * sc->extent.height * 4;

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sc->frameSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.CreateBuffer(device, &bufferInfo, NULL, &sc->buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements requirements;
    vk.GetBufferMemoryRequirements(device, sc->buffer, &requirements);
    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits, &sc->coherent);
    if (memoryInfo.memoryTypeIndex == UINT32_MAX) return false;
    if (vk.AllocateMemory(device, &memoryInfo, NULL, &sc->memory) != VK_SUCCESS) return false;
    if (vk.BindBufferMemory(device, sc->buffer, sc->memory, 0) != VK_SUCCESS) return false;
    if (vk.MapMemory(device, sc->memory, 0, VK_WHOLE_SIZE, 0, (void**)&sc->pixels) != VK_SUCCESS) return false;

    uintptr_t family = (uintptr_t)VkObject_fromHandle(queue)->tag;
    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family ? family - 1 : 0;
    if (vk.CreateCommandPool(device, &poolInfo, NULL, &sc->commandPool) != VK_SUCCESS) return false;

    sc->commandBuffers = calloc(sc->imageCount, sizeof(VkCommandBuffer));
    VkCommandBufferAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = sc->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = sc->imageCount;
    if (vk.AllocateCommandBuffers(device, &allocInfo, sc->commandBuffers) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < sc->imageCount; i++) recordCopy(sc, sc->commandBuffers[i], sc->images[i]);

    VkFenceCreateInfo fenceInfo = {0};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk.CreateFence(device, &fenceInfo, NULL, &sc->fence) != VK_SUCCESS) return false;

    const char* mode = setupX11(sc);
    if (sc->swapRB && !sc->useShm) sc->swizzleRow = malloc((size_t)sc->extent.width * 4 * MIN(sc->maxRowsPerPut, sc->extent.height));
    fprintf(stderr, "vortek-x11: window 0x%x %ux%u format %d, %u images, %s%s\n", sc->surface->window,
            sc->extent.width, sc->extent.height, sc->format, sc->imageCount, mode, sc->swapRB ? ", R/B swizzle" : "");
    sc->statStart = nowSeconds();
    return true;
}

static void releaseSwapchain(X11Swapchain* sc) {
    VkDevice device = sc->device;
    for (int i = 0; i < SHM_BUFFER_COUNT; i++) detachShm(sc, &sc->shm[i]);
    if (sc->gc) xcb_free_gc(sc->surface->conn, sc->gc);
    if (sc->gc) xcb_flush(sc->surface->conn);
    if (sc->fence) vk.DestroyFence(device, sc->fence, NULL);
    if (sc->commandPool) vk.DestroyCommandPool(device, sc->commandPool, NULL);
    if (sc->pixels) vk.UnmapMemory(device, sc->memory);
    if (sc->buffer) vk.DestroyBuffer(device, sc->buffer, NULL);
    if (sc->memory) vk.FreeMemory(device, sc->memory, NULL);
    MEMFREE(sc->commandBuffers);
    MEMFREE(sc->images);
    MEMFREE(sc->swizzleRow);
}

void X11Present_onDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    VkObject* swapchainObject = VkObject_fromHandle(swapchain);
    X11Swapchain* sc = swapchainObject->tag;
    if (!sc) return;
    registryRemove(sc);
    if (sc->ready) {
        fprintf(stderr, "vortek-x11: swapchain %ux%u destroyed after %llu frames\n", sc->extent.width, sc->extent.height, (unsigned long long)sc->frames);
    }
    if (vk.loaded) releaseSwapchain(sc);
    pthread_mutex_destroy(&sc->presentLock);
    free(sc);
    swapchainObject->tag = NULL;
}

/* Copy rows from the (possibly uncached) GPU mapping, swapping R and B. */
static void swizzleRows(uint32_t* restrict dst, const uint32_t* restrict src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t v = src[i];
        dst[i] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
    }
}

static void putFrame(X11Swapchain* sc, uint8_t depth) {
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

        xcb_shm_put_image(conn, sc->surface->window, sc->gc, width, height, 0, 0, width, height, 0, 0,
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
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, sc->surface->window, sc->gc, width, rows, 0, y, 0, depth, rows * stride, src);
        }
        /* PutImage data is copied into the socket, so the source can be reused at once;
         * a round trip keeps us from queueing frames faster than X consumes them. */
        free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
    }
    xcb_flush(conn);
}

static bool rectsOverlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* True when a mapped top-level window above ours overlaps it: then the X11 path must draw the
 * frame so that window stays visible. Checked every few frames; a query_tree is a round trip. */
/* chain[0] is the window, chain[i + 1] the parent of chain[i]; the last entry is a child of the root. */
static bool findAncestors(X11Swapchain* sc) {
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

/* The top-level window holding the window (the root's child), sent with the stacking key: the
 * compositor orders different top-level windows that claim the same root-level place by recency. */
static uint64_t topLevelOf(const xcb_window_t* chain, int chainLength) {
    return chainLength > 0 ? chain[chainLength - 1] : 0;
}

/* At every level from the window up to its top-level window, a mapped sibling stacked above it
 * that overlaps the window hides part of it (Steam's menus are such siblings inside its own
 * top-level window), and so does a mapped child of the window. Two round trips per frame. */
static bool rectContains(int ox, int oy, int ow, int oh, int x, int y, int w, int h) {
    return ox <= x && oy <= y && ox + ow >= x + w && oy + oh >= y + h;
}

/* Whether another window of this process shows its frames on a compositor layer that covers all of
 * `win` (at ox,oy ow x oh in root coordinates): `win` is that window or one of its ancestors. That
 * window is stacked above ours (it is `win` or inside it), so its layer is above ours too. A window
 * that was on a layer before and is mapped again (Steam's menu reopening) counts already: its next
 * frame goes zero-copy, and until then ours would otherwise take the X11 path for a frame or two. */
static bool coveredByOwnLayer(X11Swapchain* sc, xcb_window_t win, int ox, int oy, int ow, int oh) {
    bool found = false;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS && !found; i++) {
        X11Swapchain* other = registry[i];
        if (!other || other == sc || other->pub.chainLength == 0) continue;
        /* Not on a layer now: only a window that was hidden (not one drawn through X11) comes back on one. */
        if (!other->pub.shown && (other->zeroCopy != 1 || other->lastPath == PATH_OCCLUDED || other->lastPath == PATH_X11))
            continue;
        /* Stacking keys only tell the four root-most levels apart: deeper, the layers' order is unsure. */
        if (sc->chainLength > 4 || other->pub.chainLength > 4) continue;
        if (!rectContains(other->pub.rect[0], other->pub.rect[1], other->pub.rect[2], other->pub.rect[3], ox, oy, ow, oh))
            continue;
        if (other->pub.window == win) found = true;
        for (int j = 0; j < other->pub.chainLength && !found; j++) if (other->pub.chain[j] == win) found = true;
    }
    pthread_mutex_unlock(&registryLock);
    return found;
}

/* The root's children in stacking order, hashed: a top-level window raised, lowered, created or
 * destroyed changes it (mapping does not: query_tree lists unmapped windows too). Never 0. */
static uint32_t rootOrderOf(const xcb_query_tree_reply_t* tree) {
    const xcb_window_t* children = xcb_query_tree_children(tree);
    int count = xcb_query_tree_children_length(tree);
    uint32_t hash = 2166136261u;
    for (int k = 0; k < count; k++) hash = (hash ^ children[k]) * 16777619u;
    return hash ? hash : 1;
}

/* A parent smaller than the window clips it. Wine puts a game's Vulkan window in a 1x1 parent
 * when it shows the frames elsewhere (A.V.A.: a 1024x768 client window at 0,0, its frames scaled
 * into the game window in the middle of the screen). */
static bool clippedByParent(const X11Swapchain* sc, int x, int y, int w, int h) {
    return x < sc->clipX0 || y < sc->clipY0 || x + w > sc->clipX1 || y + h > sc->clipY1;
}

static bool checkOccludedOnce(X11Swapchain* sc, int x, int y, int w, int h, bool* stale, bool* covered) {
    xcb_connection_t* conn = sc->surface->conn;
    int levels = sc->chainLength;
    xcb_get_window_attributes_cookie_t selfAttrs = xcb_get_window_attributes(conn, sc->chain[0]);
    xcb_query_tree_cookie_t trees[17];
    for (int i = 0; i < levels; i++) trees[i] = xcb_query_tree(conn, i + 1 < levels ? sc->chain[i + 1] : sc->root);
    trees[levels] = xcb_query_tree(conn, sc->chain[0]);
    xcb_query_tree_reply_t* replies[17];
    bool ok = true;
    for (int i = 0; i <= levels; i++) {
        replies[i] = xcb_query_tree_reply(conn, trees[i], NULL);
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
        if (mine < 0) { *stale = true; break; }
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
            /* Input-only windows draw nothing; a 1x1 window (wine and Chromium keep such helpers
             * mapped) would cost a whole-screen copy per frame to show one pixel. */
            if (a && g && a->map_state == XCB_MAP_STATE_VIEWABLE && a->_class != XCB_WINDOW_CLASS_INPUT_ONLY &&
                (g->width > 1 || g->height > 1)) {
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

static bool checkOccluded(X11Swapchain* sc, int x, int y, int w, int h) {
    /* Parents rarely move or change; refresh their list now and then and when it went stale. */
    sc->covered = false;
    if (sc->chainLength == 0 || ++sc->chainAge > 120) {
        if (!findAncestors(sc)) return true;
    }
    bool stale, covered;
    bool occluded = checkOccludedOnce(sc, x, y, w, h, &stale, &covered);
    if (stale) {
        if (!findAncestors(sc)) return true;
        occluded = checkOccludedOnce(sc, x, y, w, h, &stale, &covered);
    }
    sc->covered = !stale && covered;
    return occluded;
}

/* Whether the window is entirely hidden now, in sc->covered. */
static void checkCoverage(X11Swapchain* sc) {
    xcb_connection_t* conn = sc->surface->conn;
    xcb_get_geometry_reply_t* geo = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, sc->surface->window), NULL);
    if (!geo) { sc->covered = false; return; }
    xcb_translate_coordinates_reply_t* pos =
        xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, sc->surface->window, geo->root, 0, 0), NULL);
    if (pos) {
        sc->root = geo->root;
        checkOccluded(sc, pos->dst_x, pos->dst_y, geo->width, geo->height);
    }
    else sc->covered = false;
    free(pos);
    free(geo);
}

/* For swapchains on the X11 path: whether the window is entirely hidden, re-checked every frame
 * while it is (cheap at the paced rate) and every 10 frames otherwise. */
static void updateCoverage(X11Swapchain* sc) {
    if (sc->coverCountdown > 0 && !sc->covered) {
        sc->coverCountdown--;
        return;
    }
    sc->coverCountdown = 10;
    checkCoverage(sc);
}

/* Whether the root's children changed since the window was found covered (the WM raised or lowered
 * a top-level window, or one came or went). The new order is kept, so one change starts one check of
 * the coverage. One round trip. */
static bool rootRestacked(X11Swapchain* sc) {
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
    X11Swapchain* sc = ctx;
    sweepLayers();
    checkCoverage(sc);
    return sc->covered;
}

static const CoveredWaitOps coveredWaitOps = {waitNow, waitSleep, waitRestacked, waitHidden};

static int coveredFps(void) {
    static int fps;
    if (!fps) {
        fps = covered_fps_parse(getenv("VORTEK_COVERED_FPS"));
        fprintf(stderr, "vortek-x11: frames of hidden windows paced to %d a second (VORTEK_COVERED_FPS)\n", fps);
    }
    return fps;
}

/* A hidden window's frame: its semaphores are waited for and nothing is copied or drawn, and the
 * present returns when the window's next frame is due (covered_pacing.h). Windows hidden behind a
 * game (a launcher left running, the Steam client) otherwise render as fast as the GPU allows and take
 * it from the game. */
static VkResult presentCovered(X11Swapchain* sc, VkQueue queue, const VkPresentInfoKHR* presentInfo, bool* waitedSemaphores) {
    double checked = nowSeconds();
    VkPipelineStageFlags waitStages[presentInfo->waitSemaphoreCount + 1];
    for (uint32_t s = 0; s < presentInfo->waitSemaphoreCount; s++) waitStages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (!*waitedSemaphores) {
        submitInfo.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
        submitInfo.pWaitSemaphores = presentInfo->pWaitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        *waitedSemaphores = true;
    }
    VkResult result = vk.QueueSubmit(queue, 1, &submitInfo, sc->fence);
    if (result == VK_SUCCESS) result = vk.WaitForFences(sc->device, 1, &sc->fence, VK_TRUE, UINT64_MAX);
    if (result == VK_SUCCESS) vk.ResetFences(sc->device, 1, &sc->fence);
    int fps = coveredFps();
    bool shared = fps < COVERED_SHARED_FPS && shownOnThisThread(sc, nowSeconds());
    if (shared != sc->pacedShared) {
        sc->pacedShared = shared;
        fprintf(stderr, "vortek-x11: window 0x%x %ux%u hidden, %d frames a second%s (t=%.3f)\n", sc->surface->window,
                sc->extent.width, sc->extent.height, shared ? COVERED_SHARED_FPS : fps,
                shared ? ": a shown window is drawn on the same thread" : "", nowSeconds());
    }
    sc->lastCoveredFrame = covered_wait(&coveredWaitOps, sc, sc->lastCoveredFrame + covered_interval(fps, shared), checked);
    return result;
}

/* Whether a present to a w x h window reports VK_SUBOPTIMAL_KHR (swapchain_loop.h). */
static bool reportSuboptimal(X11Swapchain* sc, uint32_t w, uint32_t h) {
    int report = swapchain_loop_suboptimal(&sc->quiet, &sc->quietW, &sc->quietH, sc->extent, w, h);
    if (report < 0) {
        swapchain_loop_reset(sc->surface->window, sc->loopKey);
        fprintf(stderr, "vortek-x11: window 0x%x is now %ux%u (was %ux%u): swapchain %ux%u reports VK_SUBOPTIMAL_KHR again\n",
                sc->surface->window, w, h, sc->quietW, sc->quietH, sc->extent.width, sc->extent.height);
    }
    return report != 0;
}

/* VK_ERROR_FEATURE_NOT_PRESENT (never again for this swapchain) or VK_NOT_READY (not this frame):
 * the frame must take the X11 path, the semaphores are untouched. */
static VkResult presentZeroCopyLocked(X11Swapchain* sc, VkSwapchainKHR swapchain, uint32_t imageIndex,
                                      const VkSemaphore* waits, uint32_t waitCount, bool* suboptimal) {
    if (sc->zeroCopy < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (waitCount > 16) {
        sc->x11Why = "more than 16 wait semaphores";
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    sc->x11Why = "a window query failed";
    xcb_connection_t* conn = sc->surface->conn;
    xcb_get_geometry_cookie_t geoCookie = xcb_get_geometry(conn, sc->surface->window);
    xcb_get_geometry_reply_t* geo = xcb_get_geometry_reply(conn, geoCookie, NULL);
    if (!geo) return VK_NOT_READY;
    xcb_translate_coordinates_reply_t* pos =
        xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, sc->surface->window, geo->root, 0, 0), NULL);
    if (!pos) {
        free(geo);
        return VK_NOT_READY;
    }
    if (geo->depth == 32) {
        /* A window with alpha (the Steam client's own UI and its popups) is composited with what
         * lies below; the compositor's layer is opaque, so it stays on the X11 path. */
        free(pos);
        free(geo);
        sc->zeroCopy = -1;
        sc->x11Why = "the window has alpha";
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    int x = pos->dst_x, y = pos->dst_y, w = geo->width, h = geo->height;
    *suboptimal = reportSuboptimal(sc, geo->width, geo->height);
    xcb_window_t root = geo->root;
    free(pos);
    free(geo);
    if (w <= 0 || h <= 0) return VK_NOT_READY;
    if (!sc->rootW || root != sc->root) {
        xcb_get_geometry_reply_t* rootGeo = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, root), NULL);
        if (!rootGeo) return VK_NOT_READY;
        sc->root = root;
        sc->rootW = rootGeo->width;
        sc->rootH = rootGeo->height;
        free(rootGeo);
    }

    /* Fully covered (also by a window of ours on a layer above): paced on the covered path. */
    bool occluded = checkOccluded(sc, x, y, w, h) || sc->covered;
    /* A layer of a clipped window at its own place showed Wine's frames in the wrong place, over a
     * black game window that took the clicks. The X11 path lets Wine copy them. */
    if (!sc->covered && clippedByParent(sc, x, y, w, h)) {
        sc->occluded = false;
        sc->x11Why = "a smaller parent window clips it (Wine shows its frames in another window)";
        return VK_NOT_READY;
    }
    sc->occluded = occluded;
    if (sc->occluded) return VK_NOT_READY;

    char payload[sizeof(DDPresentZeroCopy) + 18 * sizeof(uint64_t)];
    DDPresentZeroCopy request = {0};
    request.swapchainId = VkObject_fromHandle(swapchain)->id;
    request.imageIndex = imageIndex;
    request.x = x;
    request.y = y;
    request.w = w;
    request.h = h;
    request.rootW = sc->rootW;
    request.rootH = sc->rootH;
    request.waitCount = waitCount | DD_ZC_FLAG_STACK_KEY | DD_ZC_FLAG_TOPLEVEL;
    memcpy(payload, &request, sizeof(request));
    memcpy(payload + sizeof(request), &sc->stackKey, sizeof(uint64_t));
    for (uint32_t i = 0; i < waitCount; i++) {
        uint64_t id = VkObject_fromHandle(waits[i])->id;
        memcpy(payload + sizeof(request) + (i + 1) * sizeof(uint64_t), &id, sizeof(id));
    }
    uint64_t topLevel = topLevelOf(sc->chain, sc->chainLength);
    memcpy(payload + sizeof(request) + (waitCount + 1) * sizeof(uint64_t), &topLevel, sizeof(topLevel));
    VkResult result = (VkResult)droiddeck_request(REQUEST_CODE_DD_PRESENT_ZERO_COPY, payload,
                                                  (int)(sizeof(request) + (waitCount + 2) * sizeof(uint64_t)));
    if (result == VK_SUCCESS) publishLayer(sc, true, x, y, w, h);
    if (result == VK_ERROR_FEATURE_NOT_PRESENT) {
        if (sc->zeroCopy == 1) fprintf(stderr, "vortek-x11: zero-copy stopped, presenting through X11\n");
        sc->zeroCopy = -1;
        sc->x11Why = "the server or compositor declined it";
    }
    else if (result == VK_SUCCESS && sc->zeroCopy == 0) {
        sc->zeroCopy = 1;
        fprintf(stderr, "vortek-x11: window 0x%x %ux%u presented zero-copy by the compositor\n",
                sc->surface->window, sc->extent.width, sc->extent.height);
    }
    return result;
}

static VkResult presentZeroCopy(X11Swapchain* sc, VkSwapchainKHR swapchain, uint32_t imageIndex,
                                const VkSemaphore* waits, uint32_t waitCount, bool* suboptimal) {
    pthread_mutex_lock(&sc->presentLock);
    VkResult result = presentZeroCopyLocked(sc, swapchain, imageIndex, waits, waitCount, suboptimal);
    pthread_mutex_unlock(&sc->presentLock);
    return result;
}

/* depth: the window's, for the X11 path (0 elsewhere). Path changes carry a CLOCK_MONOTONIC time,
 * to line them up with the compositor's VORTEK_PRESENT_TRACE lines. */
static void notePath(X11Swapchain* sc, int path, int depth) {
    if (path == sc->lastPath) return;
    sc->lastPath = path;
    /* Off the layer: the server takes it down when it sees this frame's present. */
    if (path != PATH_ZERO_COPY) publishLayer(sc, false, 0, 0, 0, 0);
    const char* what = path == PATH_ZERO_COPY ? "zero-copy"
                     : path == PATH_COVERED   ? "hidden, frames paced"
                     : path == PATH_OCCLUDED  ? "through X11, partly covered"
                                              : "through X11, the compositor's layer is not available";
    double t = nowSeconds();
    if (path == PATH_OCCLUDED && sc->blocker)
        fprintf(stderr, "vortek-x11: window 0x%x %ux%u now %s by 0x%x at %d,%d %dx%d (t=%.3f)\n", sc->surface->window,
                sc->extent.width, sc->extent.height, what, sc->blocker, sc->blockerRect[0], sc->blockerRect[1],
                sc->blockerRect[2], sc->blockerRect[3], t);
    else if (path == PATH_X11)
        fprintf(stderr, "vortek-x11: window 0x%x %ux%u now %s: %s, depth %d (t=%.3f)\n", sc->surface->window,
                sc->extent.width, sc->extent.height, what, sc->x11Why ? sc->x11Why : "zero-copy not tried", depth, t);
    else
        fprintf(stderr, "vortek-x11: window 0x%x %ux%u now %s (t=%.3f)\n", sc->surface->window, sc->extent.width,
                sc->extent.height, what, t);
}

static void countFrame(X11Swapchain* sc) {
    sc->frames++;
    sc->statFrames++;
    int statEvery = envInt("VORTEK_X11_STATS", 0);
    if (statEvery > 0) {
        double now = nowSeconds();
        if (sc->statStart == 0) sc->statStart = now;
        if (now - sc->statStart >= statEvery) {
            fprintf(stderr, "vortek-x11: %llu frames, %.1f fps%s\n", (unsigned long long)sc->frames,
                    sc->statFrames / (now - sc->statStart),
                    sc->covered ? " (hidden, paced)" : sc->zeroCopy == 1 && !sc->occluded ? " (zero-copy)" : "");
            sc->statStart = now;
            sc->statFrames = 0;
        }
    }
}

/* The stacking key of a window from its ancestors (chain[0] the window, the last a child of root):
 * the same encoding as checkOccludedOnce. One batch of round trips. */
static bool stackKeyOf(xcb_connection_t* conn, const xcb_window_t* chain, int levels, xcb_window_t root, uint64_t* key) {
    if (levels <= 0 || levels > 16) return false;
    xcb_query_tree_cookie_t trees[16];
    for (int i = 0; i < levels; i++) trees[i] = xcb_query_tree(conn, i + 1 < levels ? chain[i + 1] : root);
    bool ok = true;
    *key = 0;
    for (int i = 0; i < levels; i++) {
        xcb_query_tree_reply_t* tree = xcb_query_tree_reply(conn, trees[i], NULL);
        if (!tree) { ok = false; continue; }
        xcb_window_t* children = xcb_query_tree_children(tree);
        int count = xcb_query_tree_children_length(tree), mine = -1;
        for (int k = 0; k < count; k++) if (children[k] == chain[i]) mine = k;
        free(tree);
        if (mine < 0) { ok = false; continue; }
        int fromRoot = levels - 1 - i;
        if (fromRoot < 4) *key |= (uint64_t)(mine + 1 > 0xffff ? 0xffff : mine + 1) << (48 - 16 * fromRoot);
    }
    return ok;
}

static void sendLayerRequest(X11Swapchain* sc, uint32_t flags, uint64_t key, uint64_t topLevel) {
    char payload[sizeof(DDPresentZeroCopy) + 2 * sizeof(uint64_t)];
    DDPresentZeroCopy request = {0};
    request.swapchainId = VkObject_fromHandle(sc->handle)->id;
    request.waitCount = flags;
    memcpy(payload, &request, sizeof(request));
    size_t size = sizeof(request);
    if (flags & DD_ZC_FLAG_STACK_KEY) {
        memcpy(payload + size, &key, sizeof(key));
        size += sizeof(key);
    }
    if (flags & DD_ZC_FLAG_TOPLEVEL) {
        memcpy(payload + size, &topLevel, sizeof(topLevel));
        size += sizeof(topLevel);
    }
    droiddeck_request(REQUEST_CODE_DD_PRESENT_ZERO_COPY, payload, (int)size);
}

/* Windows on compositor layers that are idle (Chromium presents only on damage) are kept right
 * from any present of this process:
 * - unmapped (a closed menu may never present again): its layer is taken down (DD_ZC_FLAG_HIDE),
 *   checked on every present, else the menu lingers over the view below until the next check;
 * - restacked, or windows below it came or went: the compositor gets its new stacking key
 *   (DD_ZC_FLAG_RESTACK), so layers keep the windows' order; checked at most every 100 ms.
 * A swapchain in the middle of its own present is left for the next round. */
static void sweepLayers(void) {
    static double lastRestack;
    double now = nowSeconds();
    bool restack = now - lastRestack >= 0.1;
    if (restack) lastRestack = now;
    pthread_mutex_lock(&registryLock);
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; i++) {
        X11Swapchain* other = registry[i];
        if (!other || !other->pub.shown) continue;
        if (pthread_mutex_trylock(&other->presentLock) != 0) continue;
        xcb_connection_t* conn = other->surface->conn;
        xcb_get_window_attributes_reply_t* a =
            xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, other->surface->window), NULL);
        bool viewable = a && a->map_state == XCB_MAP_STATE_VIEWABLE;
        free(a);
        uint64_t key;
        if (!viewable) {
            sendLayerRequest(other, DD_ZC_FLAG_HIDE, 0, 0);
            other->pub.shown = false;
            fprintf(stderr, "vortek-x11: window 0x%x %ux%u is no longer viewable, its layer was taken down (t=%.3f)\n",
                    other->surface->window, other->extent.width, other->extent.height, nowSeconds());
        }
        else if (restack && stackKeyOf(conn, other->pub.chain, other->pub.chainLength, other->pub.root, &key) &&
                 key != other->pub.key) {
            sendLayerRequest(other, DD_ZC_FLAG_RESTACK | DD_ZC_FLAG_STACK_KEY | DD_ZC_FLAG_TOPLEVEL, key,
                             topLevelOf(other->pub.chain, other->pub.chainLength));
            other->pub.key = key;
        }
        pthread_mutex_unlock(&other->presentLock);
    }
    pthread_mutex_unlock(&registryLock);
}

extern void droiddeck_presented(void);

VkResult X11Present_present(VkQueue queue, const VkPresentInfoKHR* presentInfo, VkResult* results) {
    VkResult overall = VK_SUCCESS;
    bool waitedSemaphores = false;
    sweepLayers();
    droiddeck_presented();

    for (uint32_t i = 0; i < presentInfo->swapchainCount; i++) {
        VkResult result = VK_SUCCESS;
        X11Swapchain* sc = VkObject_fromHandle(presentInfo->pSwapchains[i])->tag;
        if (sc && waitedSemaphores && sc->zeroCopy >= 0) sc->x11Why = "another swapchain of this present took the semaphores";
        if (sc && !waitedSemaphores && sc->zeroCopy >= 0) {
            bool suboptimal = false;
            VkResult zc = presentZeroCopy(sc, presentInfo->pSwapchains[i], presentInfo->pImageIndices[i],
                                          presentInfo->pWaitSemaphores, presentInfo->waitSemaphoreCount, &suboptimal);
            if (zc != VK_ERROR_FEATURE_NOT_PRESENT && zc != VK_NOT_READY) {
                if (zc == VK_SUCCESS) {
                    waitedSemaphores = true;
                    notePath(sc, PATH_ZERO_COPY, 0);
                    noteShown(sc, nowSeconds());
                    countFrame(sc);
                    if (suboptimal) zc = VK_SUBOPTIMAL_KHR;
                }
                if (results) results[i] = zc;
                if (zc < 0 || (overall == VK_SUCCESS && zc != VK_SUCCESS)) {
                    if (overall >= 0) overall = zc;
                }
                continue;
            }
        }
        if (sc && !sc->ready && !sc->failed) {
            sc->ready = setupSwapchain(sc, queue);
            if (!sc->ready) {
                fprintf(stderr, "vortek-x11: swapchain setup failed, frames will not be shown\n");
                releaseSwapchain(sc);
                sc->failed = true;
            }
        }

        if (sc && sc->ready) {
            if (sc->zeroCopy < 0 || sc->occluded) {
                if (sc->zeroCopy < 0) updateCoverage(sc);
                if (sc->covered) {
                    notePath(sc, PATH_COVERED, 0); /* first: its time is when the window went hidden */
                    result = presentCovered(sc, queue, presentInfo, &waitedSemaphores);
                    countFrame(sc);
                    if (results) results[i] = result;
                    if (result < 0 || (overall == VK_SUCCESS && result != VK_SUCCESS)) {
                        if (overall >= 0) overall = result;
                    }
                    continue;
                }
            }
            uint32_t imageIndex = presentInfo->pImageIndices[i];
            xcb_get_geometry_cookie_t geometryCookie = xcb_get_geometry(sc->surface->conn, sc->surface->window);
            xcb_flush(sc->surface->conn);

            VkPipelineStageFlags waitStages[presentInfo->waitSemaphoreCount + 1];
            for (uint32_t s = 0; s < presentInfo->waitSemaphoreCount; s++) waitStages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submitInfo = {0};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            if (!waitedSemaphores) {
                submitInfo.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
                submitInfo.pWaitSemaphores = presentInfo->pWaitSemaphores;
                submitInfo.pWaitDstStageMask = waitStages;
                waitedSemaphores = true;
            }
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &sc->commandBuffers[imageIndex % sc->imageCount];

            result = vk.QueueSubmit(queue, 1, &submitInfo, sc->fence);
            if (result == VK_SUCCESS) result = vk.WaitForFences(sc->device, 1, &sc->fence, VK_TRUE, UINT64_MAX);
            if (result == VK_SUCCESS) vk.ResetFences(sc->device, 1, &sc->fence);

            xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(sc->surface->conn, geometryCookie, NULL);
            if (!geometry) {
                if (result == VK_SUCCESS) result = VK_ERROR_SURFACE_LOST_KHR;
            }
            else if (result == VK_SUCCESS) {
                if (!sc->coherent) {
                    VkMappedMemoryRange range = {0};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = sc->memory;
                    range.size = VK_WHOLE_SIZE;
                    vk.InvalidateMappedMemoryRanges(sc->device, 1, &range);
                }
                putFrame(sc, geometry->depth);
                notePath(sc, sc->zeroCopy >= 0 && sc->occluded ? PATH_OCCLUDED : PATH_X11, geometry->depth);
                noteShown(sc, nowSeconds());
                if (reportSuboptimal(sc, geometry->width, geometry->height)) {
                    result = VK_SUBOPTIMAL_KHR;
                    if (!sc->suboptimalLogged) {
                        sc->suboptimalLogged = true;
                        fprintf(stderr, "vortek-x11: window 0x%x is %ux%u, swapchain %ux%u: suboptimal\n", sc->surface->window,
                                geometry->width, geometry->height, sc->extent.width, sc->extent.height);
                    }
                }
            }
            free(geometry);

            countFrame(sc);
        }

        if (results) results[i] = result;
        if (result < 0 || (overall == VK_SUCCESS && result != VK_SUCCESS)) {
            if (overall >= 0) overall = result;
        }
    }
    return overall;
}
