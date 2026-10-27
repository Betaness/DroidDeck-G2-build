/* [mali-g2] Server memory per frame: a client loop shaped like DXVK 3.x's frame, for watching the
 * Vortek server's heap over many thousands of frames (tests/frame_leak_test.sh samples it).
 * Each frame (FL_INFLIGHT frames in flight, default 2):
 *   wait for the slot (timeline vkWaitSemaphores or a fence), vkResetCommandPool of the slot's pool,
 *   acquire (binary semaphore), vkBeginCommandBuffer (ONE_TIME_SUBMIT) of FL_CMDBUFS command
 *   buffers, record FL_DRAWS draws in dynamic rendering into the swapchain image (push constants,
 *   descriptor sets, viewport/scissor, sync2 barriers), end, submit (vkQueueSubmit2 or vkQueueSubmit)
 *   waiting on the acquire semaphore, signalling the present semaphore and the timeline, present.
 * FL_STEPS (default all) picks the parts, to bisect which call grows the server:
 *   timeline  timeline semaphore + vkWaitSemaphores for the slot instead of a fence
 *   submit2   vkQueueSubmit2 instead of vkQueueSubmit
 *   draw      dynamic rendering + draws (otherwise one vkCmdClearColorImage)
 *   desc      per frame vkResetDescriptorPool + vkAllocateDescriptorSets + vkUpdateDescriptorSets,
 *             bound for every draw
 *   query     timestamps written every frame, read back with vkGetQueryPoolResults
 *   counter   vkGetSemaphoreCounterValue polling (DXVK's submission queue)
 *   present   acquire + present (otherwise render to an offscreen image)
 *   poolreset vkResetCommandPool per frame (otherwise the implicit reset of vkBeginCommandBuffer)
 * e.g. FL_STEPS=timeline,submit2,present. FL_FRAMES (default 20000), FL_DRAWS (default 64),
 * FL_CMDBUFS (default 2), FL_REPORT (default 2000): every FL_REPORT frames prints
 * "FL frame N t S fps F".
 * Found with it (Mali-G2, also without Vortek): a command buffer that records both a query and
 * rendering keeps ~420 bytes of driver heap per vkCmdBeginRendering across resets without
 * RELEASE_RESOURCES (FL_STEPS=draw,query FL_PASSES=16: ~6.7 KB per frame).
 * Query variants: FL_QUERY_KIND=ts (default: vkCmdWriteTimestamp after the rendering), ts2
 * (vkCmdWriteTimestamp2), inrp (timestamp inside the rendering), occl (an occlusion query around
 * the draws), reset (only the vkCmdResetQueryPool of the first command buffer, no query written);
 * FL_QUERY_COUNT (default 1) such queries per frame; FL_QUERY_READ=0 skips
 * vkGetQueryPoolResults; FL_QUERY_WHERE=top|bottom writes only the timestamp at the start of the
 * first command buffer or only the queries of the last one; FL_QUERY_HOSTRESET=1 resets with
 * vkResetQueryPool instead of vkCmdResetQueryPool.
 * FL_PASSES (default 1): the draws split over that many vkCmdBeginRendering/vkCmdEndRendering
 * (occl/inrp queries need FL_QUERY_COUNT a multiple of it). FL_RENDERPASS=1/2 records them as
 * render pass instances of a VkRenderPass with vkCmdBeginRenderPass / vkCmdBeginRenderPass2 instead.
 * FL_POOL_RELEASE=N resets each command pool at every Nth use with VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT
 * (1 = always). With FL_TIMING=1 the end line is followed by "FL timing:" with the CPU time of the
 * command pool resets that released (FL_POOL_RELEASE) and of the others, and of recording the frame
 * right after a release and the other frames (recording = begin to end of the frame's command buffers),
 * and every FL frame line by "FL window N reset_us R rec_us C", the means over that report window. FL_RECREATE=N: every N frames, after
 * vkDeviceWaitIdle, destroys and recreates FL_RECREATE_WHAT (cmdpool, querypool or both).
 * FL_CB_RESET=1/2: vkResetCommandBuffer before each begin, with flags 0 / RELEASE_RESOURCES.
 * FL_SERVER_PID: the Vortek server's pid; every FL frame line then also carries the server's malloc
 * heap ("srvheap", kB of resident Scudo/malloc mappings from /proc/<pid>/smaps). FL_SERVER_PID=self
 * measures this process (the in-process Mali path, ../../hybris/tests/frame_leak_test.sh), counting
 * unnamed anonymous mappings and [heap] as heap, with its whole resident size ("rss", kB) too. Each FL
 * frame line is then followed by "FL cpu N ms T", the server's (this process's) CPU time so far.
 * Also builds as a bionic program against the device's own loader and driver (no Vortek, no X11,
 * offscreen only); it then prints the process's malloc heap with every FL frame line:
 *   clang --target=aarch64-linux-android30 -O2 -o frame_leak_test frame_leak_test.c -lvulkan
 * Build: cc -O2 -o frame_leak_test frame_leak_test.c -lvulkan -lX11 */
#ifndef __ANDROID__
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#else
#include <malloc.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "cmd_stream_spv.h"

#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define MAX_INFLIGHT 4
#define MAX_CMDBUFS 4
#define SETS_PER_FRAME 16
#define QUERIES_PER_SLOT 64

typedef struct { float rect[4]; float color[4]; } Push;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int envInt(const char* name, int def) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : def;
}

static bool step(const char* name) {
    const char* v = getenv("FL_STEPS");
    if (!v || !*v || strcmp(v, "all") == 0) return true;
    size_t n = strlen(name);
    for (const char* p = v; (p = strstr(p, name)); p += n)
        if ((p == v || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

/* Unnamed anonymous mappings count too with `anonymous`: bionic's allocator inside libhybris (the
 * in-process path) cannot name its mappings in a glibc process. */
static long serverHeapKb(int pid, bool anonymous) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long total = 0, kb;
    bool heap = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end;
        int nameAt = 0;
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &start, &end, &nameAt) == 2)
            heap = strstr(line, "scudo") || strstr(line, "malloc") ||
                   (anonymous && (nameAt == 0 || line[nameAt] == 0 || line[nameAt] == '\n' || !strncmp(line + nameAt, "[heap]", 6)));
        else if (heap && sscanf(line, "Rss: %ld kB", &kb) == 1) total += kb;
    }
    fclose(f);
    return total;
}

static long selfRssKb(void) {
    char line[256];
    long kb = -1;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

/* utime + stime of the process, ms */
static double procCpuMs(int pid) {
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    double ms = -1;
    if (fgets(line, sizeof(line), f)) {
        const char* p = strrchr(line, ')');
        unsigned long utime, stime;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) == 2)
            ms = (utime + stime) * 1000.0 / sysconf(_SC_CLK_TCK);
    }
    fclose(f);
    return ms;
}

static VkPhysicalDeviceMemoryProperties mp;
static uint32_t findType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

int main(void) {
    int frames = envInt("FL_FRAMES", 20000), inflight = envInt("FL_INFLIGHT", 2), draws = envInt("FL_DRAWS", 64);
    int ncb = envInt("FL_CMDBUFS", 2), report = envInt("FL_REPORT", 2000);
    int cbReset = envInt("FL_CB_RESET", 0), serverPid = envInt("FL_SERVER_PID", 0);
#ifndef __ANDROID__
    bool selfHeap = getenv("FL_SERVER_PID") && strcmp(getenv("FL_SERVER_PID"), "self") == 0;
    if (selfHeap) serverPid = getpid();
#endif
    int passes = envInt("FL_PASSES", 1), poolRelease = envInt("FL_POOL_RELEASE", 0), recreate = envInt("FL_RECREATE", 0);
    int renderPassKind = envInt("FL_RENDERPASS", 0), timing = envInt("FL_TIMING", 0);
    const char* recreateWhat = getenv("FL_RECREATE_WHAT") && *getenv("FL_RECREATE_WHAT") ? getenv("FL_RECREATE_WHAT") : "both";
    if (passes < 1) passes = 1;
    int queryRead = envInt("FL_QUERY_READ", 1), queryHostReset = envInt("FL_QUERY_HOSTRESET", 0);
    const char* queryWhere = getenv("FL_QUERY_WHERE") ? getenv("FL_QUERY_WHERE") : "both";
    bool queryTop = strcmp(queryWhere, "bottom") != 0, queryBottom = strcmp(queryWhere, "top") != 0;
    const char* queryKind = getenv("FL_QUERY_KIND") && *getenv("FL_QUERY_KIND") ? getenv("FL_QUERY_KIND") : "ts";
    bool occl = strcmp(queryKind, "occl") == 0, ts2 = strcmp(queryKind, "ts2") == 0, inrp = strcmp(queryKind, "inrp") == 0;
    bool resetOnly = strcmp(queryKind, "reset") == 0;
    if (resetOnly) queryRead = 0, queryTop = queryBottom = false;
    int queryCount = envInt("FL_QUERY_COUNT", 1);
    if (queryCount < 1) queryCount = 1;
    if (queryCount > QUERIES_PER_SLOT - 1) queryCount = QUERIES_PER_SLOT - 1;
    if (inflight < 1) inflight = 1;
    if (inflight > MAX_INFLIGHT) inflight = MAX_INFLIGHT;
    if (ncb < 1) ncb = 1;
    if (ncb > MAX_CMDBUFS) ncb = MAX_CMDBUFS;
    bool useTimeline = step("timeline"), useSubmit2 = step("submit2"), useDraw = step("draw"), useDesc = step("desc");
    bool useQuery = step("query"), useCounter = step("counter"), usePresent = step("present"), usePoolReset = step("poolreset");
#ifdef __ANDROID__
    usePresent = false;
#endif
    if (useQuery) printf("FL queries: %s x%d, where %s, read %d, host reset %d\n", queryKind, queryCount, queryWhere, queryRead, queryHostReset);
    printf("FL steps:%s%s%s%s%s%s%s%s; %d frames, %d in flight, %d draws, %d command buffers\n",
           useTimeline ? " timeline" : "", useSubmit2 ? " submit2" : "", useDraw ? " draw" : "", useDesc ? " desc" : "",
           useQuery ? " query" : "", useCounter ? " counter" : "", usePresent ? " present" : "", usePoolReset ? " poolreset" : "",
           frames, inflight, draws, ncb);

#ifndef __ANDROID__
    Display* dpy = NULL;
    Window win = 0;
    if (usePresent) {
        dpy = XOpenDisplay(NULL);
        if (!dpy) { printf("FAIL no X display\n"); return 1; }
        win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, envInt("FL_W", 640), envInt("FL_H", 360), 0, 0, 0);
        XMapWindow(dpy, win);
        XSync(dpy, False);
    }
#endif

    const char* iext[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "framelk", 1, "framelk", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, usePresent ? 2 : 0, iext};
    VkInstance inst;
    CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifndef __ANDROID__
    if (usePresent) {
        VkXlibSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, 0, 0, dpy, win};
        CK(vkCreateXlibSurfaceKHR(inst, &si, 0, &surface));
    }
#endif

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qi, 0, 0, usePresent ? 1 : 0, dext};
    VkDevice dev;
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkExtent2D extent = {640, 360};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageCount = 0;
    VkImage* images;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    if (usePresent) {
        VkSurfaceCapabilitiesKHR caps;
        CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
        if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
        VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = caps.minImageCount;
        sci.imageFormat = format;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        CK(vkCreateSwapchainKHR(dev, &sci, 0, &swapchain));
        vkGetSwapchainImagesKHR(dev, swapchain, &imageCount, NULL);
        images = calloc(imageCount, sizeof(VkImage));
        vkGetSwapchainImagesKHR(dev, swapchain, &imageCount, images);
    }
    else {
        imageCount = 3;
        images = calloc(imageCount, sizeof(VkImage));
        for (uint32_t i = 0; i < imageCount; i++) {
            VkImageCreateInfo imi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, format, {extent.width, extent.height, 1}, 1, 1,
                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
            CK(vkCreateImage(dev, &imi, 0, &images[i]));
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(dev, images[i], &mr);
            VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, findType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            VkDeviceMemory m;
            CK(vkAllocateMemory(dev, &ma, 0, &m));
            CK(vkBindImageMemory(dev, images[i], m, 0));
        }
    }
    VkImageView* views = calloc(imageCount, sizeof(VkImageView));
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, images[i], VK_IMAGE_VIEW_TYPE_2D, format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CK(vkCreateImageView(dev, &ivi, 0, &views[i]));
    }

    /* uniform buffer the descriptor sets point at (the shaders do not read it) */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer ubo;
    CK(vkCreateBuffer(dev, &bci, 0, &ubo));
    VkMemoryRequirements umr;
    vkGetBufferMemoryRequirements(dev, ubo, &umr);
    VkMemoryAllocateInfo uma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, umr.size, findType(umr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory umem;
    CK(vkAllocateMemory(dev, &uma, 0, &umem));
    CK(vkBindBufferMemory(dev, ubo, umem, 0));

    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &binding};
    VkDescriptorSetLayout setLayout;
    CK(vkCreateDescriptorSetLayout(dev, &dli, 0, &setLayout));
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SETS_PER_FRAME};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, SETS_PER_FRAME, 1, &poolSize};
    VkDescriptorPool descPool[MAX_INFLIGHT];
    for (int i = 0; i < inflight; i++) CK(vkCreateDescriptorPool(dev, &dpi, 0, &descPool[i]));

    VkShaderModuleCreateInfo vsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(cmd_stream_vert_spv), cmd_stream_vert_spv};
    VkShaderModuleCreateInfo fsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(cmd_stream_frag_spv), cmd_stream_frag_spv};
    VkShaderModule vs, fs;
    CK(vkCreateShaderModule(dev, &vsi, 0, &vs));
    CK(vkCreateShaderModule(dev, &fsi, 0, &fs));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &setLayout, 1, &pcr};
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(dev, &pli, 0, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo mss = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn};
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &format};
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, 0, 2, stages, &vis, &ias, 0, &vps, &rs, &mss, 0, &cbs, &ds,
        layout, VK_NULL_HANDLE, 0};
    VkPipeline pipeline;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipeline));
    /* FL_RENDERPASS: a single-subpass VkRenderPass that loads and stores the image, a framebuffer per
     * image and a pipeline for it */
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer* framebuffers = NULL;
    if (renderPassKind) {
        VkAttachmentDescription ad = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar};
        VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd};
        CK(vkCreateRenderPass(dev, &rpi, 0, &renderPass));
        framebuffers = calloc(imageCount, sizeof(VkFramebuffer));
        for (uint32_t i = 0; i < imageCount; i++) {
            VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, renderPass, 1, &views[i], extent.width, extent.height, 1};
            CK(vkCreateFramebuffer(dev, &fbi, 0, &framebuffers[i]));
        }
        gpi.pNext = NULL;
        gpi.renderPass = renderPass;
        CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipeline));
    }

    VkQueryPool queryPool, occlPool;
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, QUERIES_PER_SLOT * MAX_INFLIGHT};
    CK(vkCreateQueryPool(dev, &qpi, 0, &queryPool));
    qpi.queryType = VK_QUERY_TYPE_OCCLUSION;
    CK(vkCreateQueryPool(dev, &qpi, 0, &occlPool));
    /* queries of a slot: [base] the timestamp at the start, [base + 1 ...] the FL_QUERY_COUNT others */
    VkQueryPool lastPool = occl ? occlPool : queryPool;

    VkCommandPool pools[MAX_INFLIGHT];
    VkCommandBuffer cbs_[MAX_INFLIGHT][MAX_CMDBUFS];
    VkFence fences[MAX_INFLIGHT];
    VkSemaphore acquireSem[MAX_INFLIGHT], presentSem[MAX_INFLIGHT];
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                   usePoolReset ? VK_COMMAND_POOL_CREATE_TRANSIENT_BIT : VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    for (int i = 0; i < inflight; i++) {
        CK(vkCreateCommandPool(dev, &cpi, 0, &pools[i]));
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pools[i], VK_COMMAND_BUFFER_LEVEL_PRIMARY, ncb};
        CK(vkAllocateCommandBuffers(dev, &cai, cbs_[i]));
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT};
        CK(vkCreateFence(dev, &fci, 0, &fences[i]));
        VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CK(vkCreateSemaphore(dev, &semi, 0, &acquireSem[i]));
        CK(vkCreateSemaphore(dev, &semi, 0, &presentSem[i]));
    }
    VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, 0, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    VkSemaphore timeline;
    CK(vkCreateSemaphore(dev, &tsi, 0, &timeline));

    double t0 = now(), tReport = t0, recordTime = 0;
    /* FL_TIMING: [0] resets without release, [1] with; recording [0] other frames, [1] right after a release */
    double resetSum[2] = {0}, resetMax[2] = {0}, recSum[2] = {0}, recMax[2] = {0};
    long resetN[2] = {0}, recN[2] = {0};
    double winReset = 0, winRec = 0;  /* this report window's totals, for "reset_us" / "rec_us" */
    uint64_t submitted = 0;
    for (int f = 0; f < frames; f++) {
        int slot = f % inflight;
        if (recreate > 0 && f > 0 && f % recreate == 0) {
            CK(vkDeviceWaitIdle(dev));
            if (strcmp(recreateWhat, "querypool") != 0) {
                for (int i = 0; i < inflight; i++) {
                    vkDestroyCommandPool(dev, pools[i], 0);
                    CK(vkCreateCommandPool(dev, &cpi, 0, &pools[i]));
                    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pools[i], VK_COMMAND_BUFFER_LEVEL_PRIMARY, ncb};
                    CK(vkAllocateCommandBuffers(dev, &cai, cbs_[i]));
                }
            }
            if (strcmp(recreateWhat, "cmdpool") != 0) {
                vkDestroyQueryPool(dev, queryPool, 0);
                vkDestroyQueryPool(dev, occlPool, 0);
                qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                CK(vkCreateQueryPool(dev, &qpi, 0, &queryPool));
                qpi.queryType = VK_QUERY_TYPE_OCCLUSION;
                CK(vkCreateQueryPool(dev, &qpi, 0, &occlPool));
                lastPool = occl ? occlPool : queryPool;
                if (queryHostReset) {
                    vkResetQueryPool(dev, queryPool, 0, QUERIES_PER_SLOT * MAX_INFLIGHT);
                    vkResetQueryPool(dev, occlPool, 0, QUERIES_PER_SLOT * MAX_INFLIGHT);
                }
            }
        }
        /* the slot's previous frame */
        if (useTimeline) {
            uint64_t want = f >= inflight ? (uint64_t)(f - inflight + 1) : 0;
            if (want) {
                if (useCounter) {
                    uint64_t value = 0;
                    CK(vkGetSemaphoreCounterValue(dev, timeline, &value));
                }
                VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, 0, 0, 1, &timeline, &want};
                CK(vkWaitSemaphores(dev, &wi, UINT64_MAX));
            }
        }
        else {
            CK(vkWaitForFences(dev, 1, &fences[slot], VK_TRUE, UINT64_MAX));
            CK(vkResetFences(dev, 1, &fences[slot]));
        }
        int qbase = slot * QUERIES_PER_SLOT;
        if (useQuery && queryRead && f >= inflight && !(recreate > 0 && f % recreate < (unsigned)inflight)) {
            uint64_t ts[QUERIES_PER_SLOT];
            if (queryTop) vkGetQueryPoolResults(dev, queryPool, qbase, 1, sizeof(uint64_t), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (queryBottom)
                vkGetQueryPoolResults(dev, lastPool, qbase + 1, queryCount, queryCount * sizeof(uint64_t), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        }
        if (useQuery && queryHostReset) {
            vkResetQueryPool(dev, queryPool, qbase, QUERIES_PER_SLOT);
            vkResetQueryPool(dev, occlPool, qbase, QUERIES_PER_SLOT);
        }
        double tRecord = now();
        bool released = poolRelease > 0 && (f / inflight) % poolRelease == 0;  /* every Nth use of each slot's pool */
        if (usePoolReset) {
            CK(vkResetCommandPool(dev, pools[slot], released ? VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT : 0));
            double dt = now() - tRecord;
            winReset += dt;
            if (f >= 2 * inflight) {
                resetSum[released] += dt;
                resetN[released]++;
                if (dt > resetMax[released]) resetMax[released] = dt;
            }
        }
        double tCommands = now();
        if (useDesc) CK(vkResetDescriptorPool(dev, descPool[slot], 0));

        uint32_t index = f % imageCount;
        if (usePresent) {
            VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquireSem[slot], VK_NULL_HANDLE, &index);
            if (ar != VK_SUBOPTIMAL_KHR) CK(ar);
        }

        VkDescriptorSet sets[SETS_PER_FRAME];
        if (useDesc) {
            VkDescriptorSetLayout layouts[SETS_PER_FRAME];
            for (int i = 0; i < SETS_PER_FRAME; i++) layouts[i] = setLayout;
            VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, descPool[slot], SETS_PER_FRAME, layouts};
            CK(vkAllocateDescriptorSets(dev, &dai, sets));
            VkDescriptorBufferInfo dbi[SETS_PER_FRAME];
            VkWriteDescriptorSet w[SETS_PER_FRAME];
            for (int i = 0; i < SETS_PER_FRAME; i++) {
                dbi[i] = (VkDescriptorBufferInfo){ubo, 256 * i, 256};
                w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, sets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &dbi[i], 0};
            }
            vkUpdateDescriptorSets(dev, SETS_PER_FRAME, w, 0, NULL);
        }

        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        for (int c = 0; c < ncb; c++) {
            VkCommandBuffer cb = cbs_[slot][c];
            if (cbReset) CK(vkResetCommandBuffer(cb, cbReset == 2 ? VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT : 0));
            CK(vkBeginCommandBuffer(cb, &bi));
            bool last = c == ncb - 1;
            if (c == 0 && useQuery) {
                if (!queryHostReset) {
                    vkCmdResetQueryPool(cb, queryPool, qbase, QUERIES_PER_SLOT);
                    vkCmdResetQueryPool(cb, occlPool, qbase, QUERIES_PER_SLOT);
                }
                if (queryTop) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, qbase);
            }
            if (last) {
                VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    useDraw ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, 0, 0, 0, 0, 0, 0, 1, &b};
                vkCmdPipelineBarrier2(cb, &dep);
                if (useDraw) {
                    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, views[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, extent}, 1, 0, 1, &att};
                    VkViewport vp = {0, 0, extent.width, extent.height, 0, 1};
                    VkRect2D sc = {{0, 0}, extent};
                    bool rpQueries = useQuery && queryBottom && (occl || inrp);
                    for (int d = 0; d < draws; d++) {
                      if (d % ((draws + passes - 1) / passes) == 0) {
                        if (d > 0) {
                            if (renderPassKind == 2) {
                                VkSubpassEndInfo se = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
                                vkCmdEndRenderPass2(cb, &se);
                            }
                            else if (renderPassKind) vkCmdEndRenderPass(cb);
                            else vkCmdEndRendering(cb);
                            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                        }
                        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, renderPass,
                                                     framebuffers ? framebuffers[index] : VK_NULL_HANDLE, {{0, 0}, extent}};
                        if (renderPassKind == 2) {
                            VkSubpassBeginInfo sb = {VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, 0, VK_SUBPASS_CONTENTS_INLINE};
                            vkCmdBeginRenderPass2(cb, &rbi, &sb);
                        }
                        else if (renderPassKind) vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                        else vkCmdBeginRendering(cb, &ri);
                        vkCmdSetViewport(cb, 0, 1, &vp);
                        vkCmdSetScissor(cb, 0, 1, &sc);
                        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                      }
                        int q = d * queryCount / (draws ? draws : 1);
                        bool queryHere = rpQueries && (d == 0 || q != (d - 1) * queryCount / draws);
                        if (queryHere && occl) vkCmdBeginQuery(cb, occlPool, qbase + 1 + q, 0);
                        if (queryHere && inrp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, qbase + 1 + q);
                        if (useDesc) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[d % SETS_PER_FRAME], 0, NULL);
                        float x = (d % 8) / 4.0f - 1, y = (d / 8 % 8) / 4.0f - 1;
                        Push p = {{x, y, x + 0.25f, y + 0.25f}, {(f & 0xff) / 255.0f, d / (float)draws, 0.5f, 1}};
                        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
                        vkCmdDraw(cb, 6, 1, 0, 0);
                        bool queryEnds = d == draws - 1 || (d + 1) * queryCount / draws != q;
                        if (rpQueries && occl && queryEnds) vkCmdEndQuery(cb, occlPool, qbase + 1 + q);
                    }
                    if (renderPassKind == 2) {
                        VkSubpassEndInfo se = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
                        vkCmdEndRenderPass2(cb, &se);
                    }
                    else if (renderPassKind) vkCmdEndRenderPass(cb);
                    else vkCmdEndRendering(cb);
                }
                else {
                    VkClearColorValue color = {.float32 = {(f & 0xff) / 255.0f, 0.2f, 0.6f, 1}};
                    vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
                }
                b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                b.dstAccessMask = 0;
                b.oldLayout = b.newLayout;
                b.newLayout = usePresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
                vkCmdPipelineBarrier2(cb, &dep);
                if (useQuery && queryBottom && !occl && !inrp) {
                    for (int q = 0; q < queryCount; q++) {
                        if (ts2) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool, qbase + 1 + q);
                        else vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, qbase + 1 + q);
                    }
                }
            }
            else {
                /* DXVK's init / upload command buffer: a small fill */
                vkCmdFillBuffer(cb, ubo, 0, 4096, f);
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT};
                VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, 0, 0, 1, &mb};
                vkCmdPipelineBarrier2(cb, &dep);
            }
            CK(vkEndCommandBuffer(cb));
        }

        double tEnd = now();
        recordTime += tEnd - tRecord;
        winRec += tEnd - tCommands;
        if (f >= 2 * inflight) {
            bool after = released && usePoolReset;  /* recorded into the pool just reset with release */
            recSum[after] += tEnd - tCommands;
            recN[after]++;
            if (tEnd - tCommands > recMax[after]) recMax[after] = tEnd - tCommands;
        }
        submitted++;
        if (useSubmit2) {
            VkCommandBufferSubmitInfo cbi[MAX_CMDBUFS];
            for (int c = 0; c < ncb; c++) cbi[c] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, 0, cbs_[slot][c], 0};
            VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, 0, acquireSem[slot], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
            VkSemaphoreSubmitInfo sig[2] = {
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, 0, timeline, submitted, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
                {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, 0, presentSem[slot], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0}};
            VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, 0, 0, usePresent ? 1 : 0, &wait, ncb, cbi,
                                (useTimeline ? 1 : 0) + (usePresent ? 1 : 0), useTimeline ? sig : sig + 1};
            CK(vkQueueSubmit2(queue, 1, &si, useTimeline ? VK_NULL_HANDLE : fences[slot]));
        }
        else {
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSemaphore sigs[2] = {timeline, presentSem[slot]};
            uint64_t values[2] = {submitted, 0};
            VkTimelineSemaphoreSubmitInfo tsub = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, 0, 0, 0,
                                                  (useTimeline ? 1 : 0) + (usePresent ? 1 : 0), useTimeline ? values : values + 1};
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, useTimeline ? &tsub : NULL, usePresent ? 1 : 0, &acquireSem[slot], &stage, ncb, cbs_[slot],
                               (useTimeline ? 1 : 0) + (usePresent ? 1 : 0), useTimeline ? sigs : sigs + 1};
            CK(vkQueueSubmit(queue, 1, &si, useTimeline ? VK_NULL_HANDLE : fences[slot]));
        }

        if (usePresent) {
            VkPresentInfoKHR pinfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &presentSem[slot], 1, &swapchain, &index};
            VkResult pr = vkQueuePresentKHR(queue, &pinfo);
            if (pr != VK_SUBOPTIMAL_KHR) CK(pr);
        }
        if (report > 0 && (f + 1) % report == 0) {
            double t = now();
#ifdef __ANDROID__
            struct mallinfo mi = mallinfo();
            printf("FL frame %d t %.2f fps %.1f heap %zu\n", f + 1, t - t0, report / (t - tReport), (size_t)mi.uordblks);
#else
            if (selfHeap)
                printf("FL frame %d t %.2f fps %.1f rss %ld srvheap %ld\n", f + 1, t - t0, report / (t - tReport), selfRssKb(),
                       serverHeapKb(serverPid, true));
            else if (serverPid > 0) printf("FL frame %d t %.2f fps %.1f srvheap %ld\n", f + 1, t - t0, report / (t - tReport), serverHeapKb(serverPid, false));
            else printf("FL frame %d t %.2f fps %.1f\n", f + 1, t - t0, report / (t - tReport));
#endif
            if (timing) printf("FL window %d reset_us %.1f rec_us %.1f\n", f + 1, winReset * 1e6 / report, winRec * 1e6 / report);
            if (serverPid > 0) printf("FL cpu %d ms %.0f\n", f + 1, procCpuMs(serverPid));
            winReset = winRec = 0;
            fflush(stdout);
            tReport = t;
        }
    }
    CK(vkDeviceWaitIdle(dev));
    printf("FL done: %d frames in %.1f s, command pool reset + recording %.1f us per frame on the CPU\n", frames, now() - t0,
           recordTime * 1e6 / frames);
    if (timing)
        printf("FL timing: reset %.1f us (n %ld, max %.1f), reset+release %.1f us (n %ld, max %.1f); "
               "recording %.1f us (n %ld, max %.1f), after a release %.1f us (n %ld, max %.1f)\n",
               resetN[0] ? resetSum[0] * 1e6 / resetN[0] : 0, resetN[0], resetMax[0] * 1e6,
               resetN[1] ? resetSum[1] * 1e6 / resetN[1] : 0, resetN[1], resetMax[1] * 1e6,
               recN[0] ? recSum[0] * 1e6 / recN[0] : 0, recN[0], recMax[0] * 1e6,
               recN[1] ? recSum[1] * 1e6 / recN[1] : 0, recN[1], recMax[1] * 1e6);
    printf("FRAMELEAK-DONE\n");
    return 0;
}
