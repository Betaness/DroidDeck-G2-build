#define _GNU_SOURCE
#include "zerofg_zc.h"

#include "framegen_bridge.h"
#include "sc_layer.h"
#include "vk_loader.h"
#include "vk_present.h"
#include "vk_present_dev.h"
#include "vortek_present.h"
#include "vortek_upscale.h"
#include "zerofg_glue.h"
#include "zerofg_pace.h"

#include <android/hardware_buffer.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

void droiddeck_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define ZLOG(...) droiddeck_log("zerofg", __VA_ARGS__)

#define ZJOBS 3
#define ZSRC_MAX 16
#define ZOUT_MAX (SC_VORTEK_LAYERS * SC_VORTEK_POOL)
#define ZREADS 16
#define ZGPU_SAMPLES 512
#define STATS_NS 5000000000LL
#define IDLE_NS 10000000000LL
#define RECENT_NS 5000000000LL
#define WAIT_NS 2000000000ULL
#define AHB_FORMAT_BGRA8 5 /* HAL_PIXEL_FORMAT_BGRA_8888: DXVK's swapchains */
#define OUT_FORMAT VK_FORMAT_R8G8B8A8_UNORM /* the layer pools' AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM */

enum { SK_FIRST, SK_CUT, SK_FAST, SK_LATE, SK_BUSY, SK_UNSUPPORTED, SK_ERROR, SK_PREP, SK_COUNT };
static const char *const skip_names[SK_COUNT] = {"first", "cut", "fast", "late", "busy", "unsupported", "error", "prep"};

/* ---- the device of its own ---- */

static struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam, ts_bits;
    float ts_period;
    PFN_vkGetInstanceProcAddr gipa;
    struct zfg_caps caps;
    VkCommandPool pool;
    VkQueryPool qpool;
#define ZDEV_FUNCS(X)                                                                                              \
    X(GetDeviceQueue) X(CreateCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(CmdPipelineBarrier2) X(CmdWriteTimestamp2) X(CmdResetQueryPool) X(CreateQueryPool)     \
    X(GetQueryPoolResults) X(CreateFence) X(ResetFences) X(WaitForFences) X(GetFenceStatus) X(CreateSemaphore)   \
    X(ImportSemaphoreFdKHR) X(GetSemaphoreFdKHR) X(QueueSubmit) X(QueueWaitIdle)                                 \
    X(GetAndroidHardwareBufferPropertiesANDROID) X(CreateImage) X(DestroyImage) X(AllocateMemory) X(FreeMemory)  \
    X(BindImageMemory) X(CreateImageView) X(DestroyImageView)
#define X(n) PFN_vk##n n;
    ZDEV_FUNCS(X)
#undef X
} D;

/* ---- imports, jobs, state ---- */

struct zimg {
    AHardwareBuffer *ahb; /* a reference of our own */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat fmt;
    VkImageUsageFlags usage;
    uint32_t w, h;
};

/* A game image, per swapchain and index. */
struct zsrc {
    uint32_t chain, index; /* chain 0: free */
    AHardwareBuffer *key;  /* the game's buffer this entry is for */
    struct zimg im;
    int ok;                /* imported */
    int dead;              /* the swapchain is gone: destroyed once no job uses it */
    uint64_t last_serial;
};

/* A buffer of a layer's pool. */
struct zout {
    int slot, idx;          /* slot -1: free */
    struct zimg im;
    uint64_t last_serial;
};

struct zjob {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acq_sem, prev_sem, rel_sem, done_sem;
    uint64_t serial;        /* submitted, not finished */
    int64_t arrival_ns, s_desired_ns, ready_ns;
    struct wl_event_source *watch;
};

struct want {
    uint32_t w, h;
    VkFormat in;
    int mode, compat;
};

static struct {
    struct wl_event_loop *loop;
    int state;              /* 0: no device yet, 1: device up, -1: off for the session */
    /* Device and engine are made on a worker thread; the event loop takes the result. */
    _Atomic int prep;       /* 0: none, 1: running, 2: done */
    struct want prep_want;
    int dev_made;           /* worker only */
    int prep_dev;           /* dev_create's result in this run: 0 ok or not run, -1 failed, -2 unsupported */
    zfg_engine *prep_engine;
    int prep_status;
    const char *prep_backend;
    double prep_ms;
    char prep_why[200];
    zfg_engine *engine;
    struct want built;
    const char *backend;
    double resize_ms;
    /* knobs, read when ZeroFG turns on */
    int k_mode, k_priority, k_dpt, k_vote, k_compat, k_trace;
    int64_t k_margin;
    int active;
    int64_t last_use_ns, slot_ns[SC_VORTEK_LAYERS];
    struct {
        int valid, slot, fence; /* fence: the game's acquire fence while no generation has waited for it */
        uint32_t chain, index;
        struct zsrc *src;
        uint64_t seq;
    } prev;
    uint64_t seq, serial;
    int64_t last_arrival, lead, prev_b;
    struct zfg_period period;
    float vote;
    int vote_slot;
    int64_t vote_ns;
    const char *said_filter;
    int said_on;
    struct zjob jobs[ZJOBS];
    struct zsrc src[ZSRC_MAX];
    struct zout out[ZOUT_MAX];
} Z = {.prev = {.fence = -1}, .vote_slot = -1};

static struct {
    int64_t start_ns;
    unsigned real, gen, skip[SK_COUNT], ready_n, after_target, gpu_n;
    int64_t ready_sum, ready_max;
    float gpu[ZGPU_SAMPLES];
    double gpu_sum, gpu_max;
    unsigned long long total_gen;
} S;

static _Atomic int g_mode = ZFG_MODE_REALLYZERO;
static _Atomic(const char *) g_unsupported;
static _Atomic int g_checked;
static char g_unsupported_buf[200];
static _Atomic float g_rate_real, g_rate_shown;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int fence_signalled(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int r;
    do { r = poll(&p, 1, 0); } while (r < 0 && errno == EINTR);
    return r != 0;
}

static void set_unsupported(const char *why) {
    if (atomic_load(&g_unsupported)) return;
    snprintf(g_unsupported_buf, sizeof(g_unsupported_buf), "%s", why);
    atomic_store(&g_unsupported, g_unsupported_buf);
}

const char *zfg_unsupported(void) { return atomic_load(&g_unsupported); }

int zfg_checked(void) { return atomic_load(&g_checked); }

void zfg_set_mode(int mode) { atomic_store(&g_mode, mode == ZFG_MODE_ZERO ? ZFG_MODE_ZERO : ZFG_MODE_REALLYZERO); }

void zfg_rates(float *real, float *shown) {
    if (real) *real = atomic_load(&g_rate_real);
    if (shown) *shown = atomic_load(&g_rate_shown);
}

/* The event loop uses the device only once it took it from the worker. */
static int dev_ok(void) { return Z.state == 1; }

static const char *mode_name(int mode) { return mode == ZFG_MODE_ZERO ? "zero" : "reallyzero"; }

/* ---- reads of game images, for their release fences (any thread) ---- */

static pthread_mutex_t g_reads_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t chain, index; int fd, used; } g_reads[ZREADS];

static void reads_set(uint32_t chain, uint32_t index, int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_reads_lock);
    int slot = -1;
    for (int i = 0; i < ZREADS; i++) {
        if (g_reads[i].used && g_reads[i].chain == chain && g_reads[i].index == index) { slot = i; break; }
        if (slot < 0 && !g_reads[i].used) slot = i;
    }
    /* Full: the entries left belong to images released long ago; the first one goes. */
    if (slot < 0) slot = 0;
    if (g_reads[slot].used) close(g_reads[slot].fd);
    g_reads[slot].chain = chain;
    g_reads[slot].index = index;
    g_reads[slot].fd = fd;
    g_reads[slot].used = 1;
    pthread_mutex_unlock(&g_reads_lock);
}

static void reads_drop_chain(uint32_t chain) {
    pthread_mutex_lock(&g_reads_lock);
    for (int i = 0; i < ZREADS; i++)
        if (g_reads[i].used && g_reads[i].chain == chain) {
            close(g_reads[i].fd);
            g_reads[i].used = 0;
        }
    pthread_mutex_unlock(&g_reads_lock);
}

static int merge_fences(int a, int b) {
    struct sync_merge_data d;
    memset(&d, 0, sizeof(d));
    snprintf(d.name, sizeof(d.name), "zerofg");
    d.fd2 = b;
    d.fence = -1;
    int r;
    do { r = ioctl(a, SYNC_IOC_MERGE, &d); } while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : d.fence;
}

int zfg_release_fence(uint32_t chain, uint32_t index, int fence_fd) {
    int read_fd = -1;
    pthread_mutex_lock(&g_reads_lock);
    for (int i = 0; i < ZREADS; i++)
        if (g_reads[i].used && g_reads[i].chain == chain && g_reads[i].index == index) {
            read_fd = g_reads[i].fd;
            g_reads[i].used = 0;
            break;
        }
    pthread_mutex_unlock(&g_reads_lock);
    if (read_fd < 0) return fence_fd;
    if (fence_signalled(read_fd)) {
        close(read_fd);
        return fence_fd;
    }
    if (fence_fd < 0) return read_fd;
    const int merged = merge_fences(fence_fd, read_fd);
    if (merged >= 0) {
        close(fence_fd);
        close(read_fd);
        return merged;
    }
    struct pollfd p = {.fd = read_fd, .events = POLLIN};
    int r;
    do { r = poll(&p, 1, 100); } while (r < 0 && errno == EINTR);
    close(read_fd);
    return fence_fd;
}

/* ---- capability check on the compositor's GPU ---- */

static int format_has(PFN_vkGetPhysicalDeviceFormatProperties props, VkPhysicalDevice pd, VkFormat f,
                      VkFormatFeatureFlags need) {
    VkFormatProperties fp;
    memset(&fp, 0, sizeof(fp));
    props(pd, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* NULL: what ZeroFG checks before it builds is there; otherwise why not. */
static const char *check_gpu(VkInstance inst, VkPhysicalDevice pd, uint32_t qfam, char *buf, size_t n) {
    PFN_vkGetInstanceProcAddr gipa = vk_loader_gipa();
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures2 feats2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceFormatProperties fmts = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkEnumerateDeviceExtensionProperties exts = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qprops =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!props || !feats2 || !fmts || !exts || !qprops) return "the Vulkan driver lacks Vulkan 1.1 entry points";
    VkPhysicalDeviceProperties p;
    props(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) {
        snprintf(buf, n, "the GPU offers Vulkan %u.%u; ZeroFG needs 1.3", VK_API_VERSION_MAJOR(p.apiVersion),
                 VK_API_VERSION_MINOR(p.apiVersion));
        return buf;
    }
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    feats2(pd, &f2);
    if (!f13.synchronization2) return "the GPU has no synchronization2";
    if (!f2.features.shaderStorageImageExtendedFormats) return "the GPU has no shaderStorageImageExtendedFormats";
    uint32_t ne = 0;
    exts(pd, NULL, &ne, NULL);
    VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof(*e));
    if (!e) return "out of memory";
    exts(pd, NULL, &ne, e);
    const char *missing = NULL;
    if (!has_ext(e, ne, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME))
        missing = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    else if (!has_ext(e, ne, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))
        missing = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    else if (!has_ext(e, ne, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))
        missing = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    free(e);
    if (missing) {
        snprintf(buf, n, "the GPU driver has no %s", missing);
        return buf;
    }
    uint32_t nq = 0;
    qprops(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    qprops(pd, &nq, qf);
    if (qfam >= nq || !(qf[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT)) return "the compositor's queue family has no compute";
    const VkFormatFeatureFlags work = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const VkFormatFeatureFlags sample = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const struct { VkFormat f; VkFormatFeatureFlags need; const char *name; } want[] = {
        {VK_FORMAT_R16_SFLOAT, work, "R16_SFLOAT storage with linear sampling"},
        {VK_FORMAT_R16G16_SFLOAT, work, "R16G16_SFLOAT storage with linear sampling"},
        {VK_FORMAT_R8G8B8A8_UNORM, work | VK_FORMAT_FEATURE_BLIT_DST_BIT, "R8G8B8A8 storage, linear sampling and blits into it"},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
         VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT,
         "A2B10G10R10 storage and blits from it"},
        {VK_FORMAT_B8G8R8A8_UNORM, sample, "B8G8R8A8 linear sampling"},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        if (!format_has(fmts, pd, want[i].f, want[i].need)) {
            snprintf(buf, n, "the GPU has no %s", want[i].name);
            return buf;
        }
    return NULL;
}

void zfg_device_ready(void) {
    struct vkp_device_info d;
    if (vkp_device_info(&d) != 0) return;
    char buf[160];
    const char *why = check_gpu(d.inst, d.pd, d.qfam, buf, sizeof(buf));
    if (why) set_unsupported(why);
    atomic_store(&g_checked, 1);
    ZLOG("%s%s", why ? "unavailable: " : "available on the compositor's GPU (zero-copy main window, "
                                          "its own Vulkan device)", why ? why : "");
}

/* ---- device (worker thread) ---- */

static int dev_create(char *why, size_t n) {
    struct vkp_device_info d;
    if (vkp_device_info(&d) != 0) { snprintf(why, n, "the compositor has no Vulkan device"); return -1; }
    char buf[160];
    const char *bad = check_gpu(d.inst, d.pd, d.qfam, buf, sizeof(buf));
    if (bad) { snprintf(why, n, "%s", bad); return -2; }
    D.inst = d.inst;
    D.pd = d.pd;
    D.qfam = d.qfam;
    D.ts_bits = d.ts_bits;
    D.ts_period = d.ts_period;
    D.gipa = vk_loader_gipa();
    PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)D.gipa(D.inst, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures2 feats2 = (PFN_vkGetPhysicalDeviceFeatures2)D.gipa(D.inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties exts =
        (PFN_vkEnumerateDeviceExtensionProperties)D.gipa(D.inst, "vkEnumerateDeviceExtensionProperties");
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)D.gipa(D.inst, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)D.gipa(D.inst, "vkGetDeviceProcAddr");
    if (!props2 || !feats2 || !exts || !create || !gdpa) { snprintf(why, n, "the Vulkan driver lacks entry points"); return -2; }

    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p13};
    VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11};
    props2(D.pd, &pp);
    VkPhysicalDeviceVulkan13Features s13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features s12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &s13};
    VkPhysicalDeviceFeatures2 sf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &s12};
    feats2(D.pd, &sf);

    uint32_t ne = 0;
    exts(D.pd, NULL, &ne, NULL);
    VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof(*e));
    if (!e) { snprintf(why, n, "out of memory"); return -1; }
    exts(D.pd, NULL, &ne, e);
    const char *names[8];
    uint32_t nn = 0;
    names[nn++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    names[nn++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    names[nn++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    const char *prio_ext = has_ext(e, ne, "VK_KHR_global_priority") ? "VK_KHR_global_priority"
                           : has_ext(e, ne, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME) ? VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME
                                                                                 : NULL;
    free(e);

    VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13,
                                            .shaderFloat16 = s12.shaderFloat16};
    VkPhysicalDeviceFeatures2 ef = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e12};
    ef.features.shaderStorageImageExtendedFormats = VK_TRUE;
    const float qprio = 1.0f;
    VkDeviceQueueGlobalPriorityCreateInfoEXT gp = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT,
        .globalPriority = Z.k_priority == 2 ? VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT : VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT};
    const char *level = Z.k_priority == 2 ? "REALTIME" : "HIGH";
    const int ask = Z.k_priority > 0 && prio_ext;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .pNext = ask ? &gp : NULL,
                                   .queueFamilyIndex = D.qfam, .queueCount = 1, .pQueuePriorities = &qprio};
    if (ask) names[nn++] = prio_ext;
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &ef, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = nn, .ppEnabledExtensionNames = names};
    char prio[160];
    VkResult r = create(D.pd, &dci, NULL, &D.dev);
    if (ask && r == VK_SUCCESS) {
        snprintf(prio, sizeof(prio), "%s accepted by vkCreateDevice (%s)", level, prio_ext);
    } else if (ask) {
        qci.pNext = NULL;
        dci.enabledExtensionCount = nn - 1;
        const VkResult first = r;
        r = create(D.pd, &dci, NULL, &D.dev);
        snprintf(prio, sizeof(prio), "%s refused (%s): default priority", level,
                 first == VK_ERROR_NOT_PERMITTED_EXT ? "VK_ERROR_NOT_PERMITTED" : "vkCreateDevice failed");
        if (first != VK_ERROR_NOT_PERMITTED_EXT) {
            const size_t len = strlen(prio);
            snprintf(prio + len, sizeof(prio) - len, ", VkResult %d", (int)first);
        }
    } else {
        snprintf(prio, sizeof(prio), "%s", Z.k_priority > 0 ? "unavailable: no VK_KHR_global_priority or VK_EXT_global_priority"
                                                           : "not requested (DROIDDECK_ZEROFG_PRIORITY=0)");
    }
    if (r != VK_SUCCESS) { snprintf(why, n, "vkCreateDevice for ZeroFG failed (VkResult %d)", (int)r); return -1; }
    int missing = 0;
#define X(f) D.f = (PFN_vk##f)gdpa(D.dev, "vk" #f); missing |= D.f == NULL;
    ZDEV_FUNCS(X)
#undef X
    if (missing) { snprintf(why, n, "ZeroFG's device lacks entry points"); return -2; }
    D.GetDeviceQueue(D.dev, D.qfam, 0, &D.queue);

    D.caps = (struct zfg_caps){
        .api_version = pp.properties.apiVersion < VK_API_VERSION_1_3 ? pp.properties.apiVersion : VK_API_VERSION_1_3,
        .sync2 = 1, .storage_extended_formats = 1, .fp16_supported = s12.shaderFloat16 != 0,
        .fp16_enabled = s12.shaderFloat16 != 0, .subgroup_size = p11.subgroupSize, .subgroup_min = p13.minSubgroupSize,
        .subgroup_max = p13.maxSubgroupSize, .max_workgroup_subgroups = p13.maxComputeWorkgroupSubgroups,
        .subgroup_stages = p11.subgroupSupportedStages, .subgroup_ops = p11.subgroupSupportedOperations};

    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = D.qfam};
    if (D.CreateCommandPool(D.dev, &cpi, NULL, &D.pool) != VK_SUCCESS) { snprintf(why, n, "no command pool"); return -1; }
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = D.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo plain = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkExportSemaphoreCreateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                       .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo exportable = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &exp};
    for (int i = 0; i < ZJOBS; i++) {
        struct zjob *j = &Z.jobs[i];
        if (D.AllocateCommandBuffers(D.dev, &cai, &j->cmd) != VK_SUCCESS ||
            D.CreateFence(D.dev, &fci, NULL, &j->fence) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &plain, NULL, &j->acq_sem) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &plain, NULL, &j->prev_sem) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &plain, NULL, &j->rel_sem) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &exportable, NULL, &j->done_sem) != VK_SUCCESS) {
            snprintf(why, n, "ZeroFG's command buffers, fences or semaphores could not be created");
            return -1;
        }
    }
    if (D.ts_bits) {
        VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = 2 * ZJOBS};
        if (D.CreateQueryPool(D.dev, &qpi, NULL, &D.qpool) != VK_SUCCESS) D.qpool = VK_NULL_HANDLE;
    }
    ZLOG("device: %s, Vulkan %u.%u, queue family %u, subgroup %u, fp16 %s, GPU timestamps %s; global priority %s",
         pp.properties.deviceName, VK_API_VERSION_MAJOR(pp.properties.apiVersion), VK_API_VERSION_MINOR(pp.properties.apiVersion),
         D.qfam, p11.subgroupSize, s12.shaderFloat16 ? "on" : "off", D.qpool ? "on" : "off", prio);
    return 0;
}

static void *prep_main(void *arg) {
    (void)arg;
    const int64_t t0 = now_ns();
    Z.prep_engine = NULL;
    Z.prep_status = ZFG_ST_OK;
    Z.prep_backend = NULL;
    Z.prep_why[0] = 0;
    if (!Z.dev_made) {
        Z.prep_dev = dev_create(Z.prep_why, sizeof(Z.prep_why));
        Z.dev_made = Z.prep_dev == 0;
    }
    if (Z.dev_made) {
        const struct zfg_build b = {.instance = D.inst, .pd = D.pd, .dev = D.dev, .gipa = D.gipa, .caps = D.caps,
                                    .mode = Z.prep_want.mode, .compat_only = Z.prep_want.compat, .contexts = ZJOBS,
                                    .width = Z.prep_want.w, .height = Z.prep_want.h, .in_format = Z.prep_want.in,
                                    .out_format = OUT_FORMAT};
        Z.prep_engine = zfg_engine_build(&b, &Z.prep_status, &Z.prep_backend);
    }
    Z.prep_ms = (double)(now_ns() - t0) / 1e6;
    atomic_store(&Z.prep, 2);
    return NULL;
}

static void prep_start(const struct want *w) {
    if (atomic_load(&Z.prep) != 0) return;
    Z.prep_want = *w;
    Z.prep_dev = 0;
    atomic_store(&Z.prep, 1);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, prep_main, NULL) != 0) prep_main(NULL);
    pthread_attr_destroy(&a);
}

static void forget_prev(void);
static void vote_clear(void);

static void disable(const char *why, int unsupported) {
    if (Z.state == -1) return;
    Z.state = -1;
    Z.active = 0;
    forget_prev();
    vote_clear();
    atomic_store(&g_rate_real, 0.0f);
    atomic_store(&g_rate_shown, 0.0f);
    if (unsupported) set_unsupported(why);
    ZLOG("off for this session: %s; zero-copy frames are shown as they are", why);
}

/* Takes what the worker made; 1 when an engine for `w` is ready. */
static int adopt(const struct want *w) {
    if (atomic_load(&Z.prep) != 2) return Z.engine != NULL;
    atomic_store(&Z.prep, 0);
    if (Z.prep_dev < 0) {
        disable(Z.prep_why, Z.prep_dev == -2);
        return 0;
    }
    if (Z.state == 0) Z.state = 1;
    if (!Z.prep_engine) {
        char why[200];
        snprintf(why, sizeof(why), "ZeroFG (%s) could not be built at %ux%u: %s", mode_name(Z.prep_want.mode), Z.prep_want.w,
                 Z.prep_want.h, zfg_status_name(Z.prep_status));
        disable(why, Z.prep_status == ZFG_ST_UNSUPPORTED);
        return 0;
    }
    Z.engine = Z.prep_engine;
    Z.prep_engine = NULL;
    Z.built = Z.prep_want;
    Z.backend = Z.prep_backend;
    Z.resize_ms = Z.prep_ms;
    Z.said_on = 0;
    return memcmp(&Z.built, w, sizeof(*w)) == 0;
}

/* ---- jobs ---- */

static void job_finish(struct zjob *j) {
    const int k = (int)(j - Z.jobs);
    if (j->watch) {
        wl_event_source_remove(j->watch);
        j->watch = NULL;
    }
    if (D.qpool) {
        uint64_t ts[2];
        if (D.GetQueryPoolResults(D.dev, D.qpool, (uint32_t)(2 * k), 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const unsigned bits = D.ts_bits ? D.ts_bits : 64;
            const uint64_t mask = bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
            const double ms = (double)((ts[1] - ts[0]) & mask) * (double)D.ts_period / 1e6;
            if (S.gpu_n < ZGPU_SAMPLES) S.gpu[S.gpu_n] = (float)ms;
            S.gpu_n++;
            S.gpu_sum += ms;
            if (ms > S.gpu_max) S.gpu_max = ms;
        }
    }
    j->serial = 0;
}

static void note_ready(struct zjob *j, int64_t t) {
    if (j->ready_ns || !j->serial) return;
    j->ready_ns = t;
    const int64_t delay = t - j->arrival_ns;
    Z.lead = zfg_lead_update(Z.lead, delay);
    S.ready_n++;
    S.ready_sum += delay;
    if (delay > S.ready_max) S.ready_max = delay;
    if (j->s_desired_ns && t > j->s_desired_ns) S.after_target++;
}

static int job_wait(struct zjob *j) {
    if (!j->serial) return 0;
    VkResult r = D.WaitForFences(D.dev, 1, &j->fence, VK_TRUE, WAIT_NS);
    if (r == VK_SUCCESS) {
        note_ready(j, now_ns());
        job_finish(j);
        return 0;
    }
    if (r == VK_ERROR_DEVICE_LOST) disable("ZeroFG's GPU device was lost", 0);
    else ZLOG("a generation has not finished in %llu ms (%d)", (unsigned long long)(WAIT_NS / 1000000), (int)r);
    return -1;
}

static int in_flight(uint64_t serial) {
    for (int i = 0; i < ZJOBS; i++)
        if (Z.jobs[i].serial && Z.jobs[i].serial <= serial) return 1;
    return 0;
}

static int jobs_running(void) {
    int n = 0;
    for (int i = 0; i < ZJOBS; i++) n += Z.jobs[i].serial != 0;
    return n;
}

static void zimg_destroy(struct zimg *im) {
    if (im->view) D.DestroyImageView(D.dev, im->view, NULL);
    if (im->img) D.DestroyImage(D.dev, im->img, NULL);
    if (im->mem) D.FreeMemory(D.dev, im->mem, NULL);
    if (im->ahb) AHardwareBuffer_release(im->ahb);
    memset(im, 0, sizeof(*im));
}

static void reap(void) {
    if (!dev_ok()) return;
    for (int i = 0; i < ZJOBS; i++) {
        struct zjob *j = &Z.jobs[i];
        if (!j->serial) continue;
        VkResult r = D.GetFenceStatus(D.dev, j->fence);
        if (r == VK_SUCCESS) {
            note_ready(j, now_ns());
            job_finish(j);
        } else if (r == VK_ERROR_DEVICE_LOST) {
            disable("ZeroFG's GPU device was lost", 0);
            return;
        }
    }
    for (int i = 0; i < ZSRC_MAX; i++) {
        struct zsrc *e = &Z.src[i];
        if (e->chain && e->dead && e != Z.prev.src && !in_flight(e->last_serial)) {
            zimg_destroy(&e->im);
            memset(e, 0, sizeof(*e));
        }
    }
}

static int on_done(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    struct zjob *j = data;
    if (j->watch) wl_event_source_remove(j->watch);
    j->watch = NULL;
    note_ready(j, now_ns());
    reap();
    return 0;
}

static void wait_all(void) {
    if (!dev_ok()) return;
    for (int i = 0; i < ZJOBS; i++) job_wait(&Z.jobs[i]);
}

/* ---- imports ---- */

static int zimport(AHardwareBuffer *ahb, int sampled, struct zimg *out, const char **why) {
    memset(out, 0, sizeof(*out));
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(ahb, &desc);
    VkAndroidHardwareBufferFormatPropertiesANDROID fp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID ap = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp};
    if (D.GetAndroidHardwareBufferPropertiesANDROID(D.dev, ahb, &ap) != VK_SUCCESS || fp.format == VK_FORMAT_UNDEFINED ||
        !ap.memoryTypeBits) {
        *why = "its buffers have no Vulkan format";
        return -1;
    }
    const VkFormatFeatureFlags need = sampled ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                                              : VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.formatFeatures & need) != need) {
        *why = sampled ? "its buffers cannot be sampled with linear filtering" : "the layer buffers cannot be blitted into";
        return -1;
    }
    const VkImageUsageFlags tries[2] = {
        sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext, .imageType = VK_IMAGE_TYPE_2D,
                             .format = fp.format, .extent = {desc.width, desc.height, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    for (int t = 0; t < 2 && !out->img; t++) {
        ici.usage = tries[t];
        if (D.CreateImage(D.dev, &ici, NULL, &out->img) != VK_SUCCESS) out->img = VK_NULL_HANDLE;
        else out->usage = tries[t];
    }
    if (!out->img) { *why = "vkCreateImage refused the imported buffer"; return -1; }
    uint32_t idx = 0;
    while (idx < 32 && !(ap.memoryTypeBits & (1u << idx))) idx++;
    VkImportAndroidHardwareBufferInfoANDROID imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                    .buffer = ahb};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp,
                                         .image = out->img};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded,
                                .allocationSize = ap.allocationSize, .memoryTypeIndex = idx};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = fp.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (D.AllocateMemory(D.dev, &mai, NULL, &out->mem) != VK_SUCCESS) { out->mem = VK_NULL_HANDLE; goto fail; }
    if (D.BindImageMemory(D.dev, out->img, out->mem, 0) != VK_SUCCESS) goto fail;
    vci.image = out->img;
    if (D.CreateImageView(D.dev, &vci, NULL, &out->view) != VK_SUCCESS) { out->view = VK_NULL_HANDLE; goto fail; }
    AHardwareBuffer_acquire(ahb);
    out->ahb = ahb;
    out->fmt = fp.format;
    out->w = desc.width;
    out->h = desc.height;
    return 0;
fail:
    zimg_destroy(out);
    *why = "the imported buffer could not be bound";
    return -1;
}

static struct zsrc *src_get(const struct vku_frame *f, const char **why) {
    struct zsrc *free_e = NULL;
    for (int pass = 0; pass < 2 && !free_e; pass++) {
        for (int i = 0; i < ZSRC_MAX; i++) {
            struct zsrc *e = &Z.src[i];
            if (!e->chain) { if (!free_e) free_e = e; continue; }
            if (e->dead || e->chain != f->chain || e->index != f->index) continue;
            if (e->key == f->ahb) {
                if (!e->ok) *why = "its buffers cannot be imported";
                return e->ok ? e : NULL;
            }
            e->dead = 1;
        }
        if (!free_e) reap();
    }
    if (!free_e) { *why = "too many swapchain images are imported"; return NULL; }
    free_e->chain = f->chain;
    free_e->index = f->index;
    free_e->key = f->ahb;
    free_e->dead = 0;
    free_e->last_serial = 0;
    /* A failed import stays in the table, so it is not tried again every frame. */
    free_e->ok = zimport(f->ahb, 1, &free_e->im, why) == 0;
    if (!free_e->ok) {
        ZLOG("swapchain %u image %u cannot be used: %s", VP_CHAIN_ID(f->chain), f->index, *why);
        return NULL;
    }
    return free_e;
}

static struct zout *out_get(int slot, int idx, const char **why) {
    AHardwareBuffer *ahb = sc_layer_vortek_pool_ahb(slot, idx);
    if (!ahb) { *why = "no layer buffer"; return NULL; }
    struct zout *free_e = NULL;
    for (int i = 0; i < ZOUT_MAX; i++) {
        struct zout *o = &Z.out[i];
        if (o->im.img && o->slot == slot && o->idx == idx) {
            if (o->im.ahb == ahb) return o;
            /* The pool reallocated this buffer: the old import goes once nothing uses it. */
            if (in_flight(o->last_serial)) {
                for (int k = 0; k < ZJOBS; k++)
                    if (Z.jobs[k].serial && Z.jobs[k].serial <= o->last_serial && job_wait(&Z.jobs[k]) != 0) {
                        *why = "a generation did not finish";
                        return NULL;
                    }
            }
            zimg_destroy(&o->im);
            o->slot = -1;
        }
        if (!o->im.img && !free_e) free_e = o;
    }
    if (!free_e) { *why = "too many layer buffers are imported"; return NULL; }
    if (zimport(ahb, 0, &free_e->im, why) != 0) return NULL;
    if (free_e->im.fmt != OUT_FORMAT) {
        zimg_destroy(&free_e->im);
        *why = "the layer buffers are not R8G8B8A8";
        return NULL;
    }
    free_e->slot = slot;
    free_e->idx = idx;
    free_e->last_serial = 0;
    return free_e;
}

static void outs_drop(void) {
    for (int i = 0; i < ZOUT_MAX; i++) {
        if (!Z.out[i].im.img) continue;
        zimg_destroy(&Z.out[i].im);
        Z.out[i].slot = -1;
    }
}

/* ---- generation ---- */

static void img_barrier(VkImageMemoryBarrier2 *b, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t sq,
                        uint32_t dq, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                        VkAccessFlags2 da) {
    *b = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss,
                                 .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                                 .newLayout = to, .srcQueueFamilyIndex = sq, .dstQueueFamilyIndex = dq, .image = img,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}

static void barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *b, uint32_t n) {
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n,
                           .pImageMemoryBarriers = b};
    D.CmdPipelineBarrier2(cmd, &di);
}

static struct zfg_image zimage(const struct zimg *im, VkImageLayout layout, uint64_t seq) {
    struct zfg_image z = {.image = im->img, .view = im->view, .layout = layout, .format = im->fmt, .width = im->w,
                          .height = im->h, .usage = im->usage, .sequence = seq};
    return z;
}

/* The game images stay in GENERAL: one of them is on the screen while it is read, and a layout
 * transition could rewrite it. Ownership moves from and back to the foreign queue family around the work. */
static int record(struct zjob *j, const struct zsrc *a, const struct zsrc *b, const struct zout *o, uint64_t seq_b) {
    const int k = (int)(j - Z.jobs);
    const uint32_t qf = D.qfam, fq = VK_QUEUE_FAMILY_FOREIGN_EXT;
    const VkPipelineStageFlags2 all = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    D.ResetCommandBuffer(j->cmd, 0);
    if (D.BeginCommandBuffer(j->cmd, &bi) != VK_SUCCESS) return ZFG_ST_VULKAN;
    if (D.qpool) D.CmdResetQueryPool(j->cmd, D.qpool, (uint32_t)(2 * k), 2);
    VkImageMemoryBarrier2 in[3];
    img_barrier(&in[0], a->im.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, fq, qf, VK_PIPELINE_STAGE_2_NONE, 0, all,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(&in[1], b->im.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, fq, qf, VK_PIPELINE_STAGE_2_NONE, 0, all,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(&in[2], o->im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, fq, qf, VK_PIPELINE_STAGE_2_NONE, 0, all, rw);
    barriers(j->cmd, in, 3);
    if (D.qpool) D.CmdWriteTimestamp2(j->cmd, all, D.qpool, (uint32_t)(2 * k));
    const struct zfg_image pa = zimage(&a->im, VK_IMAGE_LAYOUT_GENERAL, seq_b - 1);
    const struct zfg_image pb = zimage(&b->im, VK_IMAGE_LAYOUT_GENERAL, seq_b);
    const struct zfg_image po = zimage(&o->im, VK_IMAGE_LAYOUT_GENERAL, 0);
    const int st = zfg_engine_interpolate(Z.engine, j->cmd, (uint32_t)k, &pa, &pb, &po);
    if (D.qpool) D.CmdWriteTimestamp2(j->cmd, all, D.qpool, (uint32_t)(2 * k + 1));
    VkImageMemoryBarrier2 out[3];
    img_barrier(&out[0], a->im.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, qf, fq, all, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    img_barrier(&out[1], b->im.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, qf, fq, all, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    img_barrier(&out[2], o->im.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, qf, fq, all,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    barriers(j->cmd, out, 3);
    if (D.EndCommandBuffer(j->cmd) != VK_SUCCESS && st == ZFG_ST_OK) return ZFG_ST_VULKAN;
    return st;
}

static int import_wait(VkSemaphore sem, int fd) {
    VkImportSemaphoreFdInfoKHR ii = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = sem,
                                     .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                     .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fd};
    return D.ImportSemaphoreFdKHR(D.dev, &ii) == VK_SUCCESS ? 0 : -1;
}

/* Records and submits the generation of S from a and b into o. Consumes acq_fd and prev_fd; *rel_fd (the
 * display's release fence of o) becomes -1 once the GPU waits for it. On success returns 0 with S's completion
 * fence in *done_fd (-1: already complete). On failure the caller gives the pool buffer back with *rel_fd;
 * *skip says why. */
static int submit(struct zjob *j, struct zsrc *a, struct zsrc *b, struct zout *o, uint64_t seq_b, int acq_fd,
                  int prev_fd, int *rel_fd, int *done_fd, int *skip) {
    *done_fd = -1;
    const int st = record(j, a, b, o, seq_b);
    if (st != ZFG_ST_OK) {
        static int said;
        if (!said) { said = 1; ZLOG("a generation could not be recorded: %s", zfg_status_name(st)); }
        *skip = st == ZFG_ST_UNSUPPORTED || st == ZFG_ST_INVALID ? SK_UNSUPPORTED : SK_ERROR;
        goto fail;
    }
    VkSemaphore waits[3];
    VkPipelineStageFlags stages[3];
    uint32_t nw = 0;
    int *fds[3] = {&acq_fd, &prev_fd, rel_fd};
    VkSemaphore sems[3] = {j->acq_sem, j->prev_sem, j->rel_sem};
    for (int i = 0; i < 3; i++) {
        if (*fds[i] < 0) continue;
        if (fence_signalled(*fds[i])) { close(*fds[i]); *fds[i] = -1; continue; }
        if (import_wait(sems[i], *fds[i]) != 0) {
            disable("sync_fd waits cannot be imported into ZeroFG's device", 0);
            *skip = SK_ERROR;
            goto fail;
        }
        *fds[i] = -1;
        waits[nw] = sems[i];
        stages[nw++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = nw, .pWaitSemaphores = waits,
                       .pWaitDstStageMask = stages, .commandBufferCount = 1, .pCommandBuffers = &j->cmd,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &j->done_sem};
    D.ResetFences(D.dev, 1, &j->fence);
    const VkResult qr = D.QueueSubmit(D.queue, 1, &si, j->fence);
    if (qr != VK_SUCCESS) {
        disable(qr == VK_ERROR_DEVICE_LOST ? "ZeroFG's GPU device was lost" : "a generation could not be submitted", 0);
        *skip = SK_ERROR;
        goto fail;
    }
    j->serial = ++Z.serial;
    j->ready_ns = 0;
    a->last_serial = b->last_serial = o->last_serial = j->serial;
    VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = j->done_sem,
                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -1;
    if (D.GetSemaphoreFdKHR(D.dev, &gi, &fd) != VK_SUCCESS) fd = -1;
    if (fd < 0) {
        /* Without a fence to hand on, S has to be complete before anyone sees it. */
        if (job_wait(j) != 0) {
            *skip = SK_ERROR;
            return -1;
        }
        return 0;
    }
    j->watch = wl_event_loop_add_fd(Z.loop, fd, WL_EVENT_READABLE, on_done, j);
    reads_set(a->chain, a->index, fcntl(fd, F_DUPFD_CLOEXEC, 0));
    reads_set(b->chain, b->index, fcntl(fd, F_DUPFD_CLOEXEC, 0));
    *done_fd = fd;
    return 0;
fail:
    if (acq_fd >= 0) close(acq_fd);
    if (prev_fd >= 0) close(prev_fd);
    return -1;
}

/* ---- state ---- */

static int knob(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false")) return 0;
    if (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true")) return 1;
    return dflt;
}

static void read_knobs(void) {
    const char *m = getenv("DROIDDECK_ZEROFG_MODE");
    Z.k_mode = m && !strcasecmp(m, "zero") ? ZFG_MODE_ZERO : m && !strcasecmp(m, "reallyzero") ? ZFG_MODE_REALLYZERO : -1;
    const char *p = getenv("DROIDDECK_ZEROFG_PRIORITY");
    Z.k_priority = !p || !*p || !strcasecmp(p, "high") ? 1 : !strcasecmp(p, "realtime") ? 2 : knob("DROIDDECK_ZEROFG_PRIORITY", 1);
    Z.k_dpt = knob("DROIDDECK_ZEROFG_DPT", 1);
    Z.k_vote = knob("DROIDDECK_ZEROFG_VOTE", 1);
    Z.k_trace = knob("DROIDDECK_ZEROFG_TRACE", 0);
    const char *b = getenv("DROIDDECK_ZEROFG_BACKEND");
    Z.k_compat = b && !strcasecmp(b, "compat");
    const char *mg = getenv("DROIDDECK_ZEROFG_MARGIN_MS");
    const int margin = mg && *mg ? atoi(mg) : 2;
    Z.k_margin = (int64_t)(margin < 0 ? 0 : margin > 50 ? 50 : margin) * 1000000LL;
}

static int mode_now(void) { return Z.k_mode >= 0 ? Z.k_mode : atomic_load(&g_mode); }

static void forget_prev(void) {
    Z.prev.valid = 0;
    Z.prev.src = NULL;
    if (Z.prev.fence >= 0) close(Z.prev.fence);
    Z.prev.fence = -1;
    Z.prev_b = 0;
}

static void vote_clear(void) {
    if (Z.vote_slot >= 0) sc_layer_vortek_frame_rate(Z.vote_slot, 0.0f);
    Z.vote_slot = -1;
    Z.vote = 0.0f;
}

static void stats_reset(int64_t now) {
    const unsigned long long total = S.total_gen;
    memset(&S, 0, sizeof(S));
    S.total_gen = total;
    S.start_ns = now;
}

static void activate(int64_t now) {
    read_knobs();
    Z.active = 1;
    Z.last_arrival = 0;
    Z.lead = 0;
    zfg_period_reset(&Z.period);
    forget_prev();
    stats_reset(now);
}

static void deactivate(const char *why) {
    if (!Z.active) return;
    Z.active = 0;
    forget_prev();
    vote_clear();
    atomic_store(&g_rate_real, 0.0f);
    atomic_store(&g_rate_shown, 0.0f);
    if (why) ZLOG("off: %s (%llu frames generated)", why, S.total_gen);
}

static float refresh_hz(void) {
    float hz = vkp_framegen_refresh_hz();
    if (hz <= 1.0f) hz = sc_layer_frame_rate_want();
    return hz > 1.0f ? hz : 0.0f;
}

static VkFormat ahb_vk_format(uint32_t f) {
    switch (f) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case AHB_FORMAT_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static int cmp_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void stats_maybe_log(int64_t now, int dpt) {
    const int64_t span = now - S.start_ns;
    if (span < STATS_NS) return;
    const double secs = (double)span / 1e9;
    const unsigned n = S.gpu_n < ZGPU_SAMPLES ? S.gpu_n : ZGPU_SAMPLES;
    double med = 0.0;
    if (n) {
        qsort(S.gpu, n, sizeof(S.gpu[0]), cmp_float);
        med = n % 2 ? S.gpu[n / 2] : 0.5 * ((double)S.gpu[n / 2 - 1] + (double)S.gpu[n / 2]);
    }
    unsigned skipped = 0;
    for (int i = 0; i < SK_COUNT; i++) skipped += S.skip[i];
    const int64_t period = zfg_period_get(&Z.period);
    const float real = (float)(S.real / secs), shown = (float)((S.real + S.gen) / secs);
    atomic_store(&g_rate_real, real);
    atomic_store(&g_rate_shown, shown);
    ZLOG("stats %.1fs real_fps=%.1f gen=%u gen_fps=%.1f skip=%u skip_first=%u skip_cut=%u skip_fast=%u skip_late=%u "
         "skip_busy=%u skip_unsupported=%u skip_error=%u skip_prep=%u gpu_ms_med=%.2f gpu_ms_avg=%.2f gpu_ms_max=%.2f "
         "gpu_n=%u ready_ms_avg=%.1f ready_ms_max=%.1f s_after_target=%u dpt=%d period_ms=%.1f lead_ms=%.1f vote_hz=%.0f "
         "mode=%s size=%ux%u",
         secs, (double)real, S.gen, S.gen / secs, skipped, S.skip[SK_FIRST], S.skip[SK_CUT], S.skip[SK_FAST],
         S.skip[SK_LATE], S.skip[SK_BUSY], S.skip[SK_UNSUPPORTED], S.skip[SK_ERROR], S.skip[SK_PREP], med,
         S.gpu_n ? S.gpu_sum / S.gpu_n : 0.0, S.gpu_max, S.gpu_n,
         S.ready_n ? (double)S.ready_sum / S.ready_n / 1e6 : 0.0, (double)S.ready_max / 1e6, S.after_target, dpt,
         (double)period / 1e6, (double)Z.lead / 1e6, (double)Z.vote, mode_name(Z.built.mode), Z.built.w, Z.built.h);
    stats_reset(now);
}

static void vote_update(int slot, int64_t period, float refresh, int64_t now) {
    if (!Z.k_vote) return;
    const float v = zfg_vote(period, refresh);
    if (v <= 0.0f) return;
    const float d = v > Z.vote ? v - Z.vote : Z.vote - v;
    if (Z.vote_slot == slot && (d < 2.0f || now - Z.vote_ns < 1000000000LL)) return;
    if (Z.vote_slot >= 0 && Z.vote_slot != slot) sc_layer_vortek_frame_rate(Z.vote_slot, 0.0f);
    sc_layer_vortek_frame_rate(slot, v);
    Z.vote_slot = slot;
    Z.vote = v;
    Z.vote_ns = now;
}

static void say_on(int dpt) {
    if (Z.said_on) return;
    Z.said_on = 1;
    ZLOG("on: %s %ux%u (%s -> R8G8B8A8 through ZeroFG's blit), backend %s, built in %.0f ms; the previous real frame "
         "is the one on the screen (no copy); desired present time %s, frame-rate vote %s, margin %lld ms",
         mode_name(Z.built.mode), Z.built.w, Z.built.h, Z.built.in == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8" : "R8G8B8A8",
         Z.backend ? Z.backend : "?", Z.resize_ms, dpt ? "on" : Z.k_dpt ? "unavailable (no ASurfaceTransaction_setDesiredPresentTime)" : "off (DROIDDECK_ZEROFG_DPT=0)",
         Z.k_vote ? "2x the real rate" : "off (DROIDDECK_ZEROFG_VOTE=0)", (long long)(Z.k_margin / 1000000));
}

/* ---- frames ---- */

/* Retention of the previous real frame: hold, (b) of the design. B is shown from the game's own buffer, so the
 * image on the screen is the next pair's A and the compositor holds no image beyond what zero-copy holds. The
 * game gets an image back with the display's release fence merged with the end of the last generation that
 * read it (zfg_release_fence). */

static void skip_direct(int why) {
    S.skip[why]++;
    forget_prev();
}

int zfg_present(const struct vku_frame *f, int acquire_fd, void *token, struct ASurfaceTransaction *batch) {
    if (!f || f->slot < 0 || f->slot >= SC_VORTEK_LAYERS || !f->ahb || !Z.loop) return ZFG_DIRECT;
    if (!vkp_framegen_zerofg_armed()) {
        if (Z.active) deactivate("frame generation turned off");
        return ZFG_DIRECT;
    }
    if (!f->main_window) {
        if (Z.prev.valid && Z.prev.slot == f->slot) forget_prev();
        return ZFG_DIRECT;
    }
    const char *filter = NULL;
    if (vku_filter_selected(&filter)) {
        if (Z.said_filter != filter) {
            Z.said_filter = filter;
            ZLOG("off while the scaling filter %s is selected: zero-copy upscaling and ZeroFG do not run together", filter);
        }
        deactivate(NULL);
        return ZFG_DIRECT;
    }
    Z.said_filter = NULL;
    if (Z.state < 0) return ZFG_DIRECT;
    const int64_t now = now_ns();
    if (!Z.active) activate(now);
    Z.last_use_ns = now;
    reap();
    if (Z.state < 0) return ZFG_DIRECT;

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(f->ahb, &desc);
    const VkFormat in = ahb_vk_format(desc.format);
    if (in == VK_FORMAT_UNDEFINED) {
        static uint32_t said_fmt;
        if (said_fmt != desc.format) {
            said_fmt = desc.format;
            ZLOG("swapchain %u keeps its frames as they are: AHardwareBuffer format %#x is not 8-bit RGBA",
                 VP_CHAIN_ID(f->chain), desc.format);
        }
        skip_direct(SK_UNSUPPORTED);
        return ZFG_DIRECT;
    }
    const struct want w = {.w = desc.width, .h = desc.height, .in = in, .mode = mode_now(), .compat = Z.k_compat};
    if (!adopt(&w) || memcmp(&Z.built, &w, sizeof(w)) != 0) {
        if (Z.state < 0) return ZFG_DIRECT;
        if (atomic_load(&Z.prep) == 0) {
            if (Z.engine) {
                wait_all();
                zfg_engine_destroy(Z.engine);
                Z.engine = NULL;
                memset(&Z.built, 0, sizeof(Z.built));
            }
            prep_start(&w);
        }
        skip_direct(SK_PREP);
        return ZFG_DIRECT;
    }
    int geo[8];
    if (vkp_update_map(f->scene_w, f->scene_h) != 0 ||
        !vkp_map_window(f->buf_w, f->buf_h, f->x, f->y, f->w, f->h, f->scene_w, f->scene_h, geo)) {
        forget_prev();
        return ZFG_DIRECT;
    }
    const char *why_src = NULL;
    struct zsrc *b = src_get(f, &why_src);
    if (!b || b->im.fmt != in) {
        skip_direct(SK_UNSUPPORTED);
        return ZFG_DIRECT;
    }

    Z.slot_ns[f->slot] = now;
    S.real++;
    const int64_t iv = Z.last_arrival ? now - Z.last_arrival : 0;
    Z.last_arrival = now;
    int64_t period = zfg_period_get(&Z.period);
    const int gap = iv > 0 && zfg_is_gap(iv, period);
    if (iv > 0 && !gap) zfg_period_add(&Z.period, iv);
    period = zfg_period_get(&Z.period);
    const float refresh = refresh_hz();
    const int64_t refresh_ns = refresh > 1.0f ? (int64_t)(1e9 / refresh) : 8333333;
    const uint64_t seq = ++Z.seq;
    const int dpt = Z.k_dpt && sc_layer_can_desire_present();

    int why = -1;
    struct zsrc *a = Z.prev.src;
    if (!Z.prev.valid || !a || Z.prev.slot != f->slot || Z.prev.chain != f->chain || Z.prev.index == f->index ||
        a->im.w != b->im.w || a->im.h != b->im.h || a->im.fmt != b->im.fmt || Z.prev.seq + 1 != seq)
        why = SK_FIRST;
    else if (gap) why = SK_CUT;
    else if (period <= 0) why = SK_FIRST;
    else if (zfg_too_fast(period, refresh)) why = SK_FAST;
    else if (jobs_running() >= 2) why = SK_LATE;
    struct zjob *j = NULL;
    for (int i = 0; why < 0 && i < ZJOBS && !j; i++)
        if (!Z.jobs[i].serial) j = &Z.jobs[i];
    if (why < 0 && !j) why = SK_LATE;
    int idx = -1, rel_fd = -1;
    struct zout *o = NULL;
    if (why < 0) {
        struct vkp_image *unused = NULL;
        idx = sc_layer_vortek_pool_take(f->slot, (int)b->im.w, (int)b->im.h, &unused, &rel_fd);
        if (idx == -1) why = SK_BUSY;
        else if (idx < 0) why = SK_ERROR;
    }
    const char *why_out = NULL;
    if (why < 0 && !(o = out_get(f->slot, idx, &why_out))) {
        static const char *said;
        if (said != why_out) { said = why_out; ZLOG("no generated frames: %s", why_out); }
        sc_layer_vortek_pool_put(f->slot, idx, rel_fd);
        rel_fd = -1;
        idx = -1;
        why = SK_UNSUPPORTED;
    }
    const int64_t lead = (Z.lead > 0 ? Z.lead : period / 2) + Z.k_margin;
    const int64_t lead_cap = 2 * period;
    struct zfg_slots t = {0, 0};
    if (period > 0) t = zfg_schedule(now, Z.prev_b, period, lead < lead_cap ? lead : lead_cap, period / 4);
    int done_fd = -1;
    if (why < 0) {
        const int acq = acquire_fd >= 0 ? fcntl(acquire_fd, F_DUPFD_CLOEXEC, 0) : -1;
        const int prev_fd = Z.prev.fence;
        Z.prev.fence = -1;
        j->arrival_ns = now;
        j->s_desired_ns = zfg_desired(t.s, refresh_ns);
        int skip = SK_ERROR;
        if (submit(j, a, b, o, seq, acq, prev_fd, &rel_fd, &done_fd, &skip) != 0) {
            sc_layer_vortek_pool_put(f->slot, idx, rel_fd);
            idx = -1;
            why = skip;
        } else if (rel_fd >= 0) {
            close(rel_fd);
        }
        rel_fd = -1;
    }
    /* The game's acquire fence for the next pair, when no generation waited for this frame. */
    const int keep_acq = why >= 0 && acquire_fd >= 0 ? fcntl(acquire_fd, F_DUPFD_CLOEXEC, 0) : -1;

    int r;
    if (why < 0) {
        vortek_present_batch_flush();
        const int keep_fd = done_fd >= 0 ? fcntl(done_fd, F_DUPFD_CLOEXEC, 0) : -1;
        struct ASurfaceTransaction *ts = sc_layer_tx_begin();
        if (ts && dpt) sc_layer_tx_desired_present(ts, zfg_desired(t.s, refresh_ns));
        const int rs = ts ? sc_layer_present_vortek_pooled(f->slot, f->z, idx, done_fd, geo, ts) : -1;
        if (ts) sc_layer_tx_apply(ts);
        else if (done_fd >= 0) close(done_fd);
        if (rs != 0) {
            sc_layer_vortek_pool_put(f->slot, idx, keep_fd);
            S.skip[SK_ERROR]++;
        } else {
            if (keep_fd >= 0) close(keep_fd);
            S.gen++;
            S.total_gen++;
        }
        struct ASurfaceTransaction *tb = sc_layer_tx_begin();
        if (tb && dpt) sc_layer_tx_desired_present(tb, zfg_desired(t.b, refresh_ns));
        r = tb ? sc_layer_present_vortek(f->slot, f->z, f->ahb, f->buf_w, f->buf_h, acquire_fd, token, f->x, f->y, f->w,
                                         f->h, f->scene_w, f->scene_h, tb)
               : -1;
        if (tb) sc_layer_tx_apply(tb);
        else if (acquire_fd >= 0) close(acquire_fd);
        Z.prev_b = t.b;
    } else if ((why == SK_LATE || why == SK_BUSY) && dpt && Z.prev_b && period > 0) {
        /* No S for this pair: B keeps the rhythm, a whole period after the previous B. */
        vortek_present_batch_flush();
        struct ASurfaceTransaction *tb = sc_layer_tx_begin();
        if (tb) sc_layer_tx_desired_present(tb, zfg_desired(t.b, refresh_ns));
        r = tb ? sc_layer_present_vortek(f->slot, f->z, f->ahb, f->buf_w, f->buf_h, acquire_fd, token, f->x, f->y, f->w,
                                         f->h, f->scene_w, f->scene_h, tb)
               : -1;
        if (tb) sc_layer_tx_apply(tb);
        else if (acquire_fd >= 0) close(acquire_fd);
        Z.prev_b = t.b;
    } else {
        r = sc_layer_present_vortek(f->slot, f->z, f->ahb, f->buf_w, f->buf_h, acquire_fd, token, f->x, f->y, f->w, f->h,
                                    f->scene_w, f->scene_h, batch);
        Z.prev_b = 0;
    }
    if (why >= 0) S.skip[why]++;
    if (r != 0) {
        if (keep_acq >= 0) close(keep_acq);
        vortek_present_release(f->chain, f->index, -1);
        forget_prev();
        return ZFG_TAKEN;
    }
    if (Z.prev.fence >= 0) close(Z.prev.fence);
    Z.prev.valid = 1;
    Z.prev.slot = f->slot;
    Z.prev.chain = f->chain;
    Z.prev.index = f->index;
    Z.prev.src = b;
    Z.prev.seq = seq;
    Z.prev.fence = keep_acq;
    if (why < 0 || why == SK_LATE || why == SK_BUSY) vote_update(f->slot, period, refresh, now);
    say_on(dpt);
    if (Z.k_trace)
        ZLOG("trace t=%.4f seq %llu iv %.2f period %.2f lead %.2f %s s %+.2f b %+.2f", (double)now / 1e9,
             (unsigned long long)seq, (double)iv / 1e6, (double)period / 1e6, (double)Z.lead / 1e6,
             why < 0 ? "gen" : skip_names[why], t.s ? (double)(t.s - now) / 1e6 : 0.0,
             t.b ? (double)(t.b - now) / 1e6 : 0.0);
    stats_maybe_log(now, dpt);
    return ZFG_SHOWN;
}

/* ---- lifecycle ---- */

void zfg_init(struct wl_event_loop *loop) {
    Z.loop = loop;
    for (int i = 0; i < ZOUT_MAX; i++) Z.out[i].slot = -1;
}

void zfg_layer_hidden(int slot) {
    if (Z.prev.valid && Z.prev.slot == slot) forget_prev();
    if (Z.vote_slot == slot) vote_clear();
}

void zfg_chain_gone(uint32_t chain) {
    if (!chain) return;
    if (Z.prev.valid && Z.prev.chain == chain) forget_prev();
    for (int i = 0; i < ZSRC_MAX; i++)
        if (Z.src[i].chain == chain) Z.src[i].dead = 1;
    reads_drop_chain(chain);
    reap();
}

void zfg_idle(void) {
    if (!dev_ok()) return;
    forget_prev();
    wait_all();
    reap();
    outs_drop();
}

int zfg_layer_recent(int slot) {
    return slot >= 0 && slot < SC_VORTEK_LAYERS && Z.slot_ns[slot] && now_ns() - Z.slot_ns[slot] < RECENT_NS;
}

void zfg_tick(void) {
    if (!dev_ok()) return;
    reap();
    const int64_t t = now_ns();
    if (Z.active && t - Z.last_use_ns > IDLE_NS) deactivate("no frames from the main window");
    if (Z.active || atomic_load(&Z.prep) != 0) return;
    if (t - Z.last_use_ns < IDLE_NS) return;
    int freed = 0;
    if (Z.engine || jobs_running()) {
        wait_all();
        reap();
        if (Z.engine) {
            zfg_engine_destroy(Z.engine);
            Z.engine = NULL;
            memset(&Z.built, 0, sizeof(Z.built));
            freed = 1;
        }
    }
    if (jobs_running()) return;
    for (int i = 0; i < ZOUT_MAX; i++)
        if (Z.out[i].im.img) { zimg_destroy(&Z.out[i].im); Z.out[i].slot = -1; freed = 1; }
    for (int i = 0; i < ZSRC_MAX; i++)
        if (Z.src[i].chain && Z.src[i].im.img) { zimg_destroy(&Z.src[i].im); memset(&Z.src[i], 0, sizeof(Z.src[i])); }
    for (int i = 0; i < SC_VORTEK_LAYERS; i++)
        if (Z.slot_ns[i] && !vku_layer_recent(i) && sc_layer_vortek_pool_trim(i) == 0) Z.slot_ns[i] = 0;
    if (freed) ZLOG("engine and imports freed (unused for %lld s)", (long long)(IDLE_NS / 1000000000LL));
}
