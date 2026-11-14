#include "vortek_upscale.h"
#include "vk_present_dev.h"
#include "effects_chain.h"
#include "sc_layer.h"
#include "vortek_present.h"
#include "zerofg_zc.h"

#include <android/hardware_buffer.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#define VKU_RING 4
#define VKU_SRC_MAX 32
#define VKU_FMT VK_FORMAT_R8G8B8A8_UNORM
#define VKU_IDLE_NS 5000000000LL
#define VKU_WAIT_NS 2000000000ULL
#define AHB_FORMAT_BGRA8 5 /* HAL_PIXEL_FORMAT_BGRA_8888: DXVK's swapchains */

enum { F_OFF, F_SGSR, F_SGSR_HQ, F_SHARPEN, F_FSR };
static const char *const filter_names[] = {"off", "SGSR", "SGSR HQ", "Sharpen (RCAS)", "FSR (EASU+RCAS)"};
enum { P_SGSR, P_SGSR_HQ, P_RCAS, P_EASU, P_COUNT };

struct pc_sgsr { float ndc[4]; float vp[4]; float edge; };
struct pc_easu { float ndc[4]; uint32_t con0[4], con1[4], con2[4], con3[4]; float outW, outH; };
struct pc_rcas { float ndc[4]; uint32_t con[4]; float outW, outH; };

/* A game image imported for sampling, per swapchain and index. Its import holds a reference to the
 * AHardwareBuffer, so vortek_present may release its own when the swapchain goes. */
struct src_img {
    uint32_t chain, index;        /* chain 0: free entry */
    AHardwareBuffer *ahb;
    struct vkp_image *img;        /* NULL: cannot be imported (not tried again) */
    VkDescriptorSet ds;
    uint64_t last_serial;
    int dead;                     /* the swapchain is gone: destroyed once its last pass is done */
};

struct job {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acq_sem, rel_sem, done_sem;
    uint64_t serial;              /* submitted, fence not seen yet */
    int64_t submit_ns;
    int pending;                  /* waiting on the CPU for its fences (no second queue) */
    int waiting_rel;              /* pending on rel_fd rather than acquire_fd */
    struct wl_event_source *wait_src;
    int acquire_fd, rel_fd;
    struct src_img *src;
    struct vkp_image *dst;        /* NULL once sc_layer destroyed the pool buffer */
    int slot, pool_idx, filter, sharp_pct;
    uint32_t chain, index;
    int bw, bh, crop[4], pw, ph, geo[8];
};

struct mid_target {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkFramebuffer fb;
    VkDescriptorSet ds;
    int w, h;
    uint64_t last_serial;
    int64_t used_ns;
};

static struct {
    int state;                    /* 0: not tried, 1: ready, -1: off */
    int built;                    /* the Vulkan objects exist */
    struct wl_event_loop *loop;
    struct vkp_device_info d;
    VkQueue queue;
    int deferred;
    int fsr_allowed;
    VkCommandPool cmd_pool;
    VkRenderPass rp;
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkPipeline pipe[P_COUNT];
    VkQueryPool qpool;
    PFN_vkImportSemaphoreFdKHR import_sem;
    PFN_vkGetSemaphoreFdKHR export_sem;
    PFN_vkGetCalibratedTimestampsEXT calibrate;
    int64_t cal_cpu_ns, cal_at_ns;
    uint64_t cal_gpu;
    struct job ring[VKU_RING];
    uint64_t serial;
    struct src_img src[VKU_SRC_MAX];
    struct mid_target mid;
    struct { int64_t last_ns; struct job *pending; } layers[SC_VORTEK_LAYERS];
    int said_filter, said_bw, said_bh, said_pw, said_ph;
} G;

static struct {
    unsigned frames, deferred, drops, timed, starts;
    int64_t pass_ns, pass_max_ns, start_ns, start_max_ns;
    int filter, bw, bh, pw, ph;
} g_stat;

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

static uint32_t packf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void on_image_destroy(struct vkp_image *img);

void vku_init(struct wl_event_loop *loop) {
    G.loop = loop;
    vkp_set_image_destroy_hook(on_image_destroy);
    const char *fsr = getenv("DROIDDECK_ZC_FSR");
    G.fsr_allowed = fsr && atoi(fsr) > 0;
}

static void disable(const char *why) {
    if (G.state == -1) return;
    G.state = -1;
    droiddeck_log("vortek", "zero-copy upscaling off for this session: %s; zero-copy windows are shown as they are", why);
}

static void lost(const char *where) {
    vkp_note_device_lost(where);
    disable("the GPU device was lost");
}

/* ---- setup ---- */

static VkPipeline make_pipeline(VkShaderModule vert, int shader, const char *name) {
    size_t size = 0;
    const uint32_t *code = vkp_effects_shader(shader, &size);
    VkShaderModule frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    if (!code || g_vk.CreateShaderModule(G.d.dev, &mci, NULL, &frag) != VK_SUCCESS) {
        droiddeck_log("error", "zero-copy upscale: %s: vkCreateShaderModule failed", name);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_FALSE, .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cb, .pDynamicState = &ds, .layout = G.pl,
                                       .renderPass = G.rp, .subpass = 0};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = g_vk.CreateGraphicsPipelines(G.d.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
    g_vk.DestroyShaderModule(G.d.dev, frag, NULL);
    if (r != VK_SUCCESS) {
        droiddeck_log("error", "zero-copy upscale: %s: vkCreateGraphicsPipelines %d", name, (int)r);
        return VK_NULL_HANDLE;
    }
    return p;
}

static void teardown(void) {
    VkDevice dev = G.d.dev;
    if (!dev) return;
    for (int i = 0; i < P_COUNT; i++) if (G.pipe[i]) g_vk.DestroyPipeline(dev, G.pipe[i], NULL);
    for (int i = 0; i < VKU_RING; i++) {
        struct job *j = &G.ring[i];
        if (j->fence) g_vk.DestroyFence(dev, j->fence, NULL);
        if (j->acq_sem) g_vk.DestroySemaphore(dev, j->acq_sem, NULL);
        if (j->rel_sem) g_vk.DestroySemaphore(dev, j->rel_sem, NULL);
        if (j->done_sem) g_vk.DestroySemaphore(dev, j->done_sem, NULL);
    }
    if (G.cmd_pool) g_vk.DestroyCommandPool(dev, G.cmd_pool, NULL);
    if (G.qpool) g_vk.DestroyQueryPool(dev, G.qpool, NULL);
    if (G.dpool) g_vk.DestroyDescriptorPool(dev, G.dpool, NULL);
    if (G.pl) g_vk.DestroyPipelineLayout(dev, G.pl, NULL);
    if (G.dsl) g_vk.DestroyDescriptorSetLayout(dev, G.dsl, NULL);
    if (G.sampler) g_vk.DestroySampler(dev, G.sampler, NULL);
    if (G.rp) g_vk.DestroyRenderPass(dev, G.rp, NULL);
    memset(G.pipe, 0, sizeof(G.pipe));
    memset(G.ring, 0, sizeof(G.ring));
    G.cmd_pool = VK_NULL_HANDLE; G.qpool = VK_NULL_HANDLE; G.dpool = VK_NULL_HANDLE; G.pl = VK_NULL_HANDLE;
    G.dsl = VK_NULL_HANDLE; G.sampler = VK_NULL_HANDLE; G.rp = VK_NULL_HANDLE;
    G.built = 0;
}

static int sync_fd_exportable(void) {
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties props = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)
        vk_loader_gipa()(G.d.inst, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (!props) return 0;
    VkPhysicalDeviceExternalSemaphoreInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties out = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    props(G.d.pd, &info, &out);
    const VkExternalSemaphoreFeatureFlags need = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                                                 VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    return (out.externalSemaphoreFeatures & need) == need;
}

static void setup_calibration(void) {
    if (!(G.d.caps & VKP_CAP_CALIBRATED) || !G.qpool) return;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)
        vk_loader_gipa()(G.d.inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
    if (!domains) return;
    VkTimeDomainEXT list[8];
    uint32_t n = 8;
    if (domains(G.d.pd, &n, list) < 0) return;
    int device = 0, mono = 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        device |= list[i] == VK_TIME_DOMAIN_DEVICE_EXT;
        mono |= list[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
    }
    if (device && mono)
        G.calibrate = (PFN_vkGetCalibratedTimestampsEXT)g_vk.GetDeviceProcAddr(G.d.dev, "vkGetCalibratedTimestampsEXT");
}

static int setup(void) {
    if (G.state) return G.state == 1 ? 0 : -1;
    if (!G.loop || vkp_ready() != 0 || vkp_device_info(&G.d) != 0) { disable("no compositor device"); return -1; }
    if (!(G.d.caps & VKP_CAP_AHB)) { disable("the driver cannot import AHardwareBuffers"); return -1; }
    if (!(G.d.caps & VKP_CAP_SEMAPHORE_FD)) { disable("the driver has no VK_KHR_external_semaphore_fd"); return -1; }
    G.import_sem = (PFN_vkImportSemaphoreFdKHR)g_vk.GetDeviceProcAddr(G.d.dev, "vkImportSemaphoreFdKHR");
    G.export_sem = (PFN_vkGetSemaphoreFdKHR)g_vk.GetDeviceProcAddr(G.d.dev, "vkGetSemaphoreFdKHR");
    if (!G.import_sem || !G.export_sem || !sync_fd_exportable()) {
        disable("the driver cannot import and export sync_fd semaphores");
        return -1;
    }
    VkDevice dev = G.d.dev;
    G.built = 1;
    G.queue = G.d.side_queue ? G.d.side_queue : G.d.queue;
    const char *defer = getenv("DROIDDECK_ZC_DEFER");
    G.deferred = !G.d.side_queue || (defer && atoi(defer) > 0);

    VkAttachmentDescription att = {.format = VKU_FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                  .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &b};
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VKU_SRC_MAX + 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                      .maxSets = VKU_SRC_MAX + 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = G.d.qfam};
    if (g_vk.CreateRenderPass(dev, &rci, NULL, &G.rp) != VK_SUCCESS ||
        g_vk.CreateSampler(dev, &sci, NULL, &G.sampler) != VK_SUCCESS ||
        g_vk.CreateDescriptorSetLayout(dev, &dli, NULL, &G.dsl) != VK_SUCCESS ||
        g_vk.CreateDescriptorPool(dev, &dpi, NULL, &G.dpool) != VK_SUCCESS ||
        g_vk.CreateCommandPool(dev, &cpi, NULL, &G.cmd_pool) != VK_SUCCESS)
        goto fail;
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0,
                               .size = sizeof(struct pc_easu)};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    if (g_vk.CreatePipelineLayout(dev, &pli, NULL, &G.pl) != VK_SUCCESS) goto fail;

    size_t vsize = 0;
    const uint32_t *vcode = vkp_effects_shader(VKP_SHADER_UPSCALE_VERT, &vsize);
    VkShaderModuleCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsize, .pCode = vcode};
    VkShaderModule vert = VK_NULL_HANDLE;
    if (g_vk.CreateShaderModule(dev, &vci, NULL, &vert) != VK_SUCCESS) goto fail;
    G.pipe[P_SGSR] = make_pipeline(vert, VKP_SHADER_SGSR, "SGSR");
    G.pipe[P_SGSR_HQ] = make_pipeline(vert, VKP_SHADER_SGSR_HQ, "SGSR HQ");
    G.pipe[P_RCAS] = make_pipeline(vert, VKP_SHADER_RCAS, "RCAS");
    G.pipe[P_EASU] = make_pipeline(vert, VKP_SHADER_EASU, "EASU");
    g_vk.DestroyShaderModule(dev, vert, NULL);
    for (int i = 0; i < P_COUNT; i++) if (!G.pipe[i]) goto fail;

    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = G.cmd_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo plain = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkExportSemaphoreCreateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                       .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo exportable = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &exp};
    for (int i = 0; i < VKU_RING; i++) {
        struct job *j = &G.ring[i];
        j->acquire_fd = j->rel_fd = -1;
        if (g_vk.AllocateCommandBuffers(dev, &cai, &j->cmd) != VK_SUCCESS ||
            g_vk.CreateFence(dev, &fci, NULL, &j->fence) != VK_SUCCESS ||
            g_vk.CreateSemaphore(dev, &plain, NULL, &j->acq_sem) != VK_SUCCESS ||
            g_vk.CreateSemaphore(dev, &plain, NULL, &j->rel_sem) != VK_SUCCESS ||
            g_vk.CreateSemaphore(dev, &exportable, NULL, &j->done_sem) != VK_SUCCESS)
            goto fail;
    }
    if (G.d.caps & VKP_CAP_TIMESTAMPS) {
        VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = 2 * VKU_RING};
        if (g_vk.CreateQueryPool(dev, &qpi, NULL, &G.qpool) != VK_SUCCESS) G.qpool = VK_NULL_HANDLE;
    }
    setup_calibration();
    G.state = 1;
    droiddeck_log("vortek", "zero-copy upscaling ready: passes %s; GPU timestamps %s%s%s", G.deferred
                  ? "are submitted once the game's frame is ready (no second queue, or DROIDDECK_ZC_DEFER=1)"
                  : "wait for the game's frame on the GPU, on a queue of their own",
                  G.qpool ? "on" : "off", G.calibrate ? ", calibrated against CLOCK_MONOTONIC" : "",
                  G.fsr_allowed ? "; FSR allowed (DROIDDECK_ZC_FSR=1)" : "");
    return 0;
fail:
    teardown();
    disable("its render pass, pipelines or ring could not be created");
    return -1;
}

/* ---- passes in flight ---- */

static void job_finish(struct job *j) {
    const int k = (int)(j - G.ring);
    if (G.qpool) {
        uint64_t ts[2];
        if (g_vk.GetQueryPoolResults(G.d.dev, G.qpool, (uint32_t)(2 * k), 2, sizeof(ts), ts, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const unsigned bits = G.d.ts_bits ? G.d.ts_bits : 64;
            const uint64_t mask = bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
            const int64_t pass = (int64_t)((double)((ts[1] - ts[0]) & mask) * G.d.ts_period);
            g_stat.pass_ns += pass;
            if (pass > g_stat.pass_max_ns) g_stat.pass_max_ns = pass;
            g_stat.timed++;
            if (G.cal_at_ns) {
                const unsigned shift = 64 - bits;
                const int64_t ticks = (int64_t)(((ts[0] - G.cal_gpu) & mask) << shift) >> shift;
                const int64_t start = G.cal_cpu_ns + (int64_t)((double)ticks * G.d.ts_period) - j->submit_ns;
                if (start >= 0) {
                    g_stat.start_ns += start;
                    if (start > g_stat.start_max_ns) g_stat.start_max_ns = start;
                    g_stat.starts++;
                }
            }
        }
    }
    j->serial = 0;
    j->src = NULL;
    j->dst = NULL;
}

/* 0: done; -1: device lost or still running after VKU_WAIT_NS. */
static int job_wait(struct job *j) {
    if (!j->serial) return 0;
    VkResult r = g_vk.WaitForFences(G.d.dev, 1, &j->fence, VK_TRUE, VKU_WAIT_NS);
    if (r == VK_SUCCESS) { job_finish(j); return 0; }
    if (r == VK_ERROR_DEVICE_LOST) lost("zero-copy upscale fence");
    else droiddeck_log("error", "zero-copy upscale: a pass has not finished in %llu ms (%d)",
                       (unsigned long long)(VKU_WAIT_NS / 1000000), (int)r);
    return -1;
}

static int in_flight(uint64_t serial) {
    for (int i = 0; i < VKU_RING; i++)
        if (G.ring[i].serial && G.ring[i].serial <= serial) return 1;
    return 0;
}

static int src_busy(const struct src_img *e) {
    if (in_flight(e->last_serial)) return 1;
    for (int i = 0; i < VKU_RING; i++)
        if (G.ring[i].pending && G.ring[i].src == e) return 1;
    return 0;
}

static void src_destroy(struct src_img *e) {
    if (e->ds) g_vk.FreeDescriptorSets(G.d.dev, G.dpool, 1, &e->ds);
    if (e->img) vkp_image_destroy(e->img);
    memset(e, 0, sizeof(*e));
}

static void reap(void) {
    if (!G.built) return;
    for (int i = 0; i < VKU_RING; i++) {
        struct job *j = &G.ring[i];
        if (!j->serial) continue;
        VkResult r = g_vk.GetFenceStatus(G.d.dev, j->fence);
        if (r == VK_SUCCESS) job_finish(j);
        else if (r == VK_ERROR_DEVICE_LOST) { lost("zero-copy upscale fence"); return; }
    }
    for (int i = 0; i < VKU_SRC_MAX; i++)
        if (G.src[i].chain && G.src[i].dead && !src_busy(&G.src[i])) src_destroy(&G.src[i]);
}

static void mid_destroy(void) {
    struct mid_target *m = &G.mid;
    if (m->ds) g_vk.FreeDescriptorSets(G.d.dev, G.dpool, 1, &m->ds);
    if (m->fb) g_vk.DestroyFramebuffer(G.d.dev, m->fb, NULL);
    if (m->view) g_vk.DestroyImageView(G.d.dev, m->view, NULL);
    if (m->img) g_vk.DestroyImage(G.d.dev, m->img, NULL);
    if (m->mem) g_vk.FreeMemory(G.d.dev, m->mem, NULL);
    memset(m, 0, sizeof(*m));
}

static VkDescriptorSet alloc_ds(VkImageView view) {
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = G.dpool, .descriptorSetCount = 1, .pSetLayouts = &G.dsl};
    if (!view || g_vk.AllocateDescriptorSets(G.d.dev, &ai, &ds) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii = {.sampler = G.sampler, .imageView = view,
                                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii};
    g_vk.UpdateDescriptorSets(G.d.dev, 1, &wr, 0, NULL);
    return ds;
}

/* FSR's EASU output, at the size of the pool buffers. */
static int mid_ensure(int w, int h) {
    struct mid_target *m = &G.mid;
    m->used_ns = now_ns();
    if (m->img && m->w == w && m->h == h) return 0;
    for (int i = 0; i < VKU_RING; i++)
        if (G.ring[i].serial && G.ring[i].serial <= m->last_serial && job_wait(&G.ring[i]) != 0) return -1;
    mid_destroy();
    m->used_ns = now_ns();
    VkDevice dev = G.d.dev;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VKU_FMT,
                            .extent = {(uint32_t)w, (uint32_t)h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (g_vk.CreateImage(dev, &ii, NULL, &m->img) != VK_SUCCESS) { m->img = VK_NULL_HANDLE; goto fail; }
    VkMemoryRequirements req;
    g_vk.GetImageMemoryRequirements(dev, m->img, &req);
    int idx = -1;
    for (uint32_t i = 0; i < G.d.memprops->memoryTypeCount && idx < 0; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (G.d.memprops->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            idx = (int)i;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)idx};
    if (idx < 0 || g_vk.AllocateMemory(dev, &mai, NULL, &m->mem) != VK_SUCCESS) { m->mem = VK_NULL_HANDLE; goto fail; }
    g_vk.BindImageMemory(dev, m->img, m->mem, 0);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VKU_FMT,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (g_vk.CreateImageView(dev, &vci, NULL, &m->view) != VK_SUCCESS) { m->view = VK_NULL_HANDLE; goto fail; }
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = G.rp,
                                  .attachmentCount = 1, .pAttachments = &m->view, .width = (uint32_t)w,
                                  .height = (uint32_t)h, .layers = 1};
    if (g_vk.CreateFramebuffer(dev, &fi, NULL, &m->fb) != VK_SUCCESS) { m->fb = VK_NULL_HANDLE; goto fail; }
    if (!(m->ds = alloc_ds(m->view))) goto fail;
    m->w = w; m->h = h;
    return 0;
fail:
    droiddeck_log("error", "zero-copy upscale: FSR's %dx%d intermediate image could not be made", w, h);
    mid_destroy();
    return -1;
}

/* A pool buffer or an imported game image is about to go: the passes that use it finish first. */
static void on_image_destroy(struct vkp_image *img) {
    for (int i = 0; i < VKU_RING; i++) {
        struct job *j = &G.ring[i];
        if (j->pending && j->dst == img) j->dst = NULL;
        if (j->serial && (j->dst == img || (j->src && j->src->img == img))) job_wait(j);
    }
}

/* ---- frames ---- */

static void release_game(uint32_t chain, uint32_t index, int fence_fd) {
    vortek_present_release(chain, index, fence_fd);
}

/* The game may reuse a dropped frame's image once it is rendered: its acquire fence is the release fence. */
static void drop_frame(const struct vku_frame *f, int acquire_fd) {
    release_game(f->chain, f->index, acquire_fd);
    g_stat.drops++;
}

static void cancel_pending(int slot) {
    struct job *j = G.layers[slot].pending;
    if (!j) return;
    G.layers[slot].pending = NULL;
    if (j->wait_src) wl_event_source_remove(j->wait_src);
    j->wait_src = NULL;
    j->pending = 0;
    release_game(j->chain, j->index, j->acquire_fd);
    j->acquire_fd = -1;
    if (j->dst) sc_layer_vortek_pool_put(j->slot, j->pool_idx, j->rel_fd);
    else if (j->rel_fd >= 0) close(j->rel_fd);
    j->rel_fd = -1;
    j->src = NULL;
    j->dst = NULL;
    g_stat.drops++;
}

static int filter_mode(int *sharp_pct, const char **why) {
    int cas = 0;
    switch (vkp_effects_scaling(sharp_pct, &cas)) {
    case 3: return F_SGSR;
    case 8: return F_SGSR_HQ;
    case 6: return F_SHARPEN;
    case 4:
    case 5:
        if (G.fsr_allowed) return F_FSR;
        *why = "FSR runs on the copy path only (DROIDDECK_ZC_FSR=1 tries it on zero-copy layers, about 2-3.5 ms a frame)";
        return F_OFF;
    case 7:
        *why = "NIS runs on the copy path only";
        return F_OFF;
    default:
        if (cas) *why = "CAS runs on the copy path only";
        return F_OFF;
    }
}

static struct src_img *src_get(const struct vku_frame *f, const char **why) {
    struct src_img *free_e = NULL;
    for (int pass = 0; pass < 2 && !free_e; pass++) {
        for (int i = 0; i < VKU_SRC_MAX; i++) {
            struct src_img *e = &G.src[i];
            if (!e->chain) { if (!free_e) free_e = e; continue; }
            if (e->dead || e->chain != f->chain || e->index != f->index) continue;
            if (e->ahb == f->ahb) {
                if (!e->img) *why = "its buffers cannot be sampled";
                return e->img ? e : NULL;
            }
            e->dead = 1;
        }
        if (!free_e) reap();
    }
    if (!free_e) { *why = "too many swapchains are imported"; return NULL; }
    free_e->chain = f->chain;
    free_e->index = f->index;
    free_e->ahb = f->ahb;
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(f->ahb, &desc);
    if (desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM && desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM &&
        desc.format != AHB_FORMAT_BGRA8) {
        droiddeck_log("vortek", "zero-copy upscale: swapchain %u keeps its frames as they are: AHardwareBuffer format %#x "
                      "is not 8-bit RGBA", VP_CHAIN_ID(f->chain), desc.format);
        *why = "its frames are not 8-bit RGBA (HDR)";
        return NULL;
    }
    free_e->img = vkp_image_import_ahb(f->ahb, VKP_AHB_SAMPLED);
    if (free_e->img && !(free_e->ds = alloc_ds(vkp_image_view(free_e->img)))) {
        vkp_image_destroy(free_e->img);
        free_e->img = NULL;
    }
    if (!free_e->img) {
        droiddeck_log("vortek", "zero-copy upscale: swapchain %u image %u (%ux%u, format %#x) could not be imported for "
                      "sampling; that swapchain is shown as it is", VP_CHAIN_ID(f->chain), f->index, desc.width, desc.height, desc.format);
        *why = "its buffers cannot be sampled";
        return NULL;
    }
    return free_e;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t src_q,
                    uint32_t dst_q, VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = src_q,
                              .dstQueueFamilyIndex = dst_q, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    g_vk.CmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void draw(VkCommandBuffer cmd, VkPipeline p, VkFramebuffer fb, int w, int h, VkDescriptorSet src,
                 const void *pc, uint32_t pcsz) {
    VkRenderPassBeginInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = G.rp, .framebuffer = fb,
                                 .renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}}};
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    g_vk.CmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    g_vk.CmdSetViewport(cmd, 0, 1, &vp);
    g_vk.CmdSetScissor(cmd, 0, 1, &sc);
    g_vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    g_vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pl, 0, 1, &src, 0, NULL);
    g_vk.CmdPushConstants(cmd, G.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcsz, pc);
    g_vk.CmdDraw(cmd, 4, 1, 0, 0);
    g_vk.CmdEndRenderPass(cmd);
}

/* The quad covers the whole game image and the viewport only the shown part of it: the shaders see the
 * full image (UV 0..1, its size in the push constants) and clipping does the crop. */
static void crop_ndc(float ndc[4], const int crop[4], int bw, int bh) {
    const float cw = (float)(crop[2] - crop[0]), ch = (float)(crop[3] - crop[1]);
    ndc[0] = -1.0f - 2.0f * (float)crop[0] / cw;
    ndc[1] = -1.0f - 2.0f * (float)crop[1] / ch;
    ndc[2] = ndc[0] + 2.0f * (float)bw / cw;
    ndc[3] = ndc[1] + 2.0f * (float)bh / ch;
}

static void easu_con(struct pc_easu *e, float inW, float inH, float outW, float outH) {
    e->con0[0] = packf(inW / outW);               e->con0[1] = packf(inH / outH);
    e->con0[2] = packf(0.5f * inW / outW - 0.5f); e->con0[3] = packf(0.5f * inH / outH - 0.5f);
    e->con1[0] = packf(1.0f / inW);  e->con1[1] = packf(1.0f / inH);
    e->con1[2] = packf(1.0f / inW);  e->con1[3] = packf(-1.0f / inH);
    e->con2[0] = packf(-1.0f / inW); e->con2[1] = packf(2.0f / inH);
    e->con2[2] = packf(1.0f / inW);  e->con2[3] = packf(2.0f / inH);
    e->con3[0] = packf(0.0f);        e->con3[1] = packf(4.0f / inH);
    e->con3[2] = 0;                  e->con3[3] = 0;
}

static void record(struct job *j, VkFramebuffer dst_fb) {
    VkCommandBuffer cmd = j->cmd;
    const uint32_t qf = G.d.qfam;
    const int k = (int)(j - G.ring);
    VkImage src = vkp_image_handle(j->src->img), dst = vkp_image_handle(j->dst);
    const float sharp01 = (float)j->sharp_pct / 100.0f;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    g_vk.ResetCommandBuffer(cmd, 0);
    g_vk.BeginCommandBuffer(cmd, &bi);
    if (G.qpool) g_vk.CmdResetQueryPool(cmd, G.qpool, (uint32_t)(2 * k), 2);
    /* The game's image: GENERAL, not UNDEFINED, which would allow the driver to drop its contents. */
    barrier(cmd, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, qf,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, qf,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (G.qpool) g_vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, G.qpool, (uint32_t)(2 * k));
    if (j->filter == F_SGSR || j->filter == F_SGSR_HQ) {
        struct pc_sgsr pc = {0};
        crop_ndc(pc.ndc, j->crop, j->bw, j->bh);
        pc.vp[0] = 1.0f / (float)j->bw; pc.vp[1] = 1.0f / (float)j->bh;
        pc.vp[2] = (float)j->bw;        pc.vp[3] = (float)j->bh;
        pc.edge = 0.5f + sharp01 * 4.0f;
        draw(cmd, G.pipe[j->filter == F_SGSR ? P_SGSR : P_SGSR_HQ], dst_fb, j->pw, j->ph, j->src->ds, &pc, sizeof(pc));
    } else if (j->filter == F_SHARPEN) {
        struct pc_rcas pc = {0};
        crop_ndc(pc.ndc, j->crop, j->bw, j->bh);
        pc.con[0] = packf(sharp01);
        pc.outW = (float)j->bw; pc.outH = (float)j->bh;
        draw(cmd, G.pipe[P_RCAS], dst_fb, j->pw, j->ph, j->src->ds, &pc, sizeof(pc));
    } else {
        /* EASU works in output pixels of the whole image; only the shown part is rendered. */
        const float ow = (float)j->bw * (float)j->pw / (float)(j->crop[2] - j->crop[0]);
        const float oh = (float)j->bh * (float)j->ph / (float)(j->crop[3] - j->crop[1]);
        struct pc_easu e = {0};
        crop_ndc(e.ndc, j->crop, j->bw, j->bh);
        easu_con(&e, (float)j->bw, (float)j->bh, ow, oh);
        e.outW = ow; e.outH = oh;
        barrier(cmd, G.mid.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        draw(cmd, G.pipe[P_EASU], G.mid.fb, j->pw, j->ph, j->src->ds, &e, sizeof(e));
        barrier(cmd, G.mid.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        struct pc_rcas r = {.ndc = {-1.0f, -1.0f, 1.0f, 1.0f}};
        r.con[0] = packf(sharp01);
        r.outW = (float)j->pw; r.outH = (float)j->ph;
        draw(cmd, G.pipe[P_RCAS], dst_fb, j->pw, j->ph, G.mid.ds, &r, sizeof(r));
    }
    barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, qf, VK_QUEUE_FAMILY_FOREIGN_EXT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    barrier(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, qf, VK_QUEUE_FAMILY_FOREIGN_EXT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    if (G.qpool) g_vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, G.qpool, (uint32_t)(2 * k + 1));
    g_vk.EndCommandBuffer(cmd);
}

static int import_wait(VkSemaphore sem, int fd) {
    VkImportSemaphoreFdInfoKHR ii = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = sem,
                                     .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                     .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fd};
    return G.import_sem(G.d.dev, &ii) == VK_SUCCESS ? 0 : -1;
}

static void calibrate(void) {
    const int64_t t = now_ns();
    if (!G.calibrate || (G.cal_at_ns && t - G.cal_at_ns < 1000000000LL)) return;
    VkCalibratedTimestampInfoEXT info[2] = {
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};
    uint64_t ts[2], deviation = 0;
    if (G.calibrate(G.d.dev, 2, info, ts, &deviation) != VK_SUCCESS) return;
    G.cal_gpu = ts[0];
    G.cal_cpu_ns = (int64_t)ts[1];
    G.cal_at_ns = t;
}

/* Records, submits and shows j. Whatever happens, the game image is released exactly once and the
 * pool buffer is either shown or given back. 0: shown (in `batch`, or in a transaction of its own). */
static int run_job(struct job *j, int z, struct ASurfaceTransaction *batch) {
    int acquire_fd = j->acquire_fd, rel_fd = j->rel_fd;
    j->acquire_fd = j->rel_fd = -1;
    VkFramebuffer fb = j->dst ? vkp_image_framebuffer(j->dst, G.rp) : VK_NULL_HANDLE;
    if (!fb || !j->src || G.state != 1) {
        release_game(j->chain, j->index, acquire_fd);
        if (j->dst) sc_layer_vortek_pool_put(j->slot, j->pool_idx, rel_fd);
        else if (rel_fd >= 0) close(rel_fd);
        j->src = NULL; j->dst = NULL;
        g_stat.drops++;
        return -1;
    }
    record(j, fb);
    VkSemaphore waits[2];
    VkPipelineStageFlags stages[2];
    uint32_t nwait = 0;
    if (acquire_fd >= 0) {
        if (import_wait(j->acq_sem, acquire_fd) != 0) goto refused;
        acquire_fd = -1;
        waits[nwait] = j->acq_sem;
        stages[nwait++] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if (rel_fd >= 0) {
        if (import_wait(j->rel_sem, rel_fd) != 0) goto refused;
        rel_fd = -1;
        waits[nwait] = j->rel_sem;
        stages[nwait++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    calibrate();
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = nwait, .pWaitSemaphores = waits,
                       .pWaitDstStageMask = stages, .commandBufferCount = 1, .pCommandBuffers = &j->cmd,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &j->done_sem};
    g_vk.ResetFences(G.d.dev, 1, &j->fence);
    VkResult qr = g_vk.QueueSubmit(G.queue, 1, &si, j->fence);
    if (qr != VK_SUCCESS) {
        /* The semaphores may hold imported payloads now: nothing is submitted with them again. */
        if (qr == VK_ERROR_DEVICE_LOST) lost("zero-copy upscale submit");
        else disable("a pass could not be submitted");
        release_game(j->chain, j->index, -1);
        sc_layer_vortek_pool_put(j->slot, j->pool_idx, -1);
        j->src = NULL; j->dst = NULL;
        g_stat.drops++;
        return -1;
    }
    j->serial = ++G.serial;
    j->submit_ns = now_ns();
    j->src->last_serial = j->serial;
    if (j->filter == F_FSR) G.mid.last_serial = j->serial;

    int done_fd = -1;
    VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = j->done_sem,
                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    if (G.export_sem(G.d.dev, &gi, &done_fd) != VK_SUCCESS) done_fd = -1;
    int game_fd = done_fd >= 0 ? dup(done_fd) : -1, keep_fd = done_fd >= 0 ? dup(done_fd) : -1;
    if (done_fd < 0 || game_fd < 0 || keep_fd < 0) {
        /* Without a fence to hand on, the pass has to be over before anyone sees its result. */
        const int unfinished = job_wait(j) != 0;
        if (unfinished) {
            if (G.state == 1) disable("a pass did not finish");
        } else if (done_fd < 0) {
            disable("the driver did not export a sync_fd for a finished pass");
        }
        if (game_fd >= 0) close(game_fd);
        if (keep_fd >= 0) close(keep_fd);
        if (done_fd >= 0) close(done_fd);
        game_fd = keep_fd = done_fd = -1;
        if (unfinished) {
            /* A half-written buffer is not shown; the game gets its image back regardless. */
            release_game(j->chain, j->index, -1);
            sc_layer_vortek_pool_put(j->slot, j->pool_idx, -1);
            g_stat.drops++;
            return -1;
        }
    }
    release_game(j->chain, j->index, game_fd);
    if (sc_layer_present_vortek_pooled(j->slot, z, j->pool_idx, done_fd, j->geo, batch) != 0) {
        sc_layer_vortek_pool_put(j->slot, j->pool_idx, keep_fd);
        g_stat.drops++;
        return -1;
    }
    if (keep_fd >= 0) close(keep_fd);
    g_stat.frames++;
    g_stat.filter = j->filter;
    g_stat.bw = j->crop[2] - j->crop[0]; g_stat.bh = j->crop[3] - j->crop[1];
    g_stat.pw = j->geo[6] - j->geo[4]; g_stat.ph = j->geo[7] - j->geo[5];
    if (G.said_filter != j->filter || G.said_bw != g_stat.bw || G.said_bh != g_stat.bh || G.said_pw != g_stat.pw ||
        G.said_ph != g_stat.ph) {
        G.said_filter = j->filter; G.said_bw = g_stat.bw; G.said_bh = g_stat.bh; G.said_pw = g_stat.pw; G.said_ph = g_stat.ph;
        droiddeck_log("vortek", "zero-copy upscale: swapchain %u with %s, %dx%d of its %dx%d frame -> %dx%d on the screen%s",
                      VP_CHAIN_ID(j->chain), filter_names[j->filter], g_stat.bw, g_stat.bh, j->bw, j->bh, g_stat.pw, g_stat.ph,
                      j->filter == F_SHARPEN ? " (sharpened at its own size, the display scales it)" : "");
    }
    return 0;
refused:
    droiddeck_log("error", "zero-copy upscale: importing a sync_fd as a GPU wait failed");
    disable("sync_fd waits cannot be imported");
    release_game(j->chain, j->index, acquire_fd);
    sc_layer_vortek_pool_put(j->slot, j->pool_idx, rel_fd);
    j->src = NULL; j->dst = NULL;
    g_stat.drops++;
    return -1;
}

static int on_ready(int fd, uint32_t mask, void *data);

/* Without a queue of its own a pass must not wait on the GPU for anything (the compositor's queue
 * would stall behind the game): it is submitted once the game's frame and the pool buffer are ready.
 * Closes the fences that have signalled; 1: j now waits for the other one. */
static int defer(struct job *j) {
    int *fds[2] = {&j->acquire_fd, &j->rel_fd};
    for (int i = 0; i < 2; i++) {
        if (*fds[i] < 0) continue;
        if (fence_signalled(*fds[i])) {
            close(*fds[i]);
            *fds[i] = -1;
            continue;
        }
        j->wait_src = wl_event_loop_add_fd(G.loop, *fds[i], WL_EVENT_READABLE, on_ready, j);
        if (!j->wait_src) return 0; /* the GPU waits for it after all */
        j->waiting_rel = i;
        j->pending = 1;
        return 1;
    }
    return 0;
}

/* `fd` is the event loop's duplicate of the fence. */
static int on_ready(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    struct job *j = data;
    if (j->wait_src) wl_event_source_remove(j->wait_src);
    j->wait_src = NULL;
    /* Signalled, or failed: either way nothing is left to wait for. */
    int *done = j->waiting_rel ? &j->rel_fd : &j->acquire_fd;
    if (*done >= 0) close(*done);
    *done = -1;
    if (defer(j)) return 0;
    j->pending = 0;
    if (j->slot >= 0 && j->slot < SC_VORTEK_LAYERS && G.layers[j->slot].pending == j) G.layers[j->slot].pending = NULL;
    reap();
    run_job(j, -1, NULL);
    return 0;
}

static struct job *free_job(void) {
    for (int i = 0; i < VKU_RING; i++)
        if (!G.ring[i].serial && !G.ring[i].pending) return &G.ring[i];
    return NULL;
}

int vku_present(const struct vku_frame *f, int acquire_fd, struct ASurfaceTransaction *batch, const char **why) {
    const char *unused;
    if (!why) why = &unused;
    *why = NULL;
    if (!f || f->slot < 0 || f->slot >= SC_VORTEK_LAYERS) return VKU_DIRECT;
    int sharp = 0, r[8];
    const int filter = filter_mode(&sharp, why);
    if (filter == F_OFF) goto direct;
    if (!f->main_window) { *why = "it is not the main window (the largest, covering at least half of the screen)"; goto direct; }
    if (setup() != 0) { *why = "zero-copy upscaling is off (see above)"; goto direct; }
    reap();
    if (G.state != 1) { *why = "zero-copy upscaling is off (see above)"; goto direct; }
    if (vkp_update_map(f->scene_w, f->scene_h) != 0 ||
        !vkp_map_window(f->buf_w, f->buf_h, f->x, f->y, f->w, f->h, f->scene_w, f->scene_h, r))
        goto direct;
    const int cw = r[2] - r[0], ch = r[3] - r[1], dw = r[6] - r[4], dh = r[7] - r[5];
    if (cw <= 0 || ch <= 0 || dw <= 0 || dh <= 0) goto direct;
    if (filter == F_SHARPEN ? (dw < cw || dh < ch) : (dw * 10 < cw * 11 || dh * 10 < ch * 11)) {
        *why = filter == F_SHARPEN ? "the display scales it down" : "it is scaled up less than 1.1x";
        goto direct;
    }
    struct src_img *src = src_get(f, why);
    if (!src) goto direct;

    /* From here on the frame is upscaled or dropped, never shown as it is: a layer that flips between
     * the game's buffer and an upscaled one flickers in sharpness. */
    G.layers[f->slot].last_ns = now_ns();
    cancel_pending(f->slot);
    sc_layer_vortek_z(f->slot, f->z, batch);
    const int pw = filter == F_SHARPEN ? cw : dw, ph = filter == F_SHARPEN ? ch : dh;
    struct job *j = free_job();
    if (!j) { drop_frame(f, acquire_fd); return VKU_TAKEN; }
    struct vkp_image *dst = NULL;
    int rel_fd = -1;
    const int idx = sc_layer_vortek_pool_take(f->slot, pw, ph, &dst, &rel_fd);
    if (idx == -1) { drop_frame(f, acquire_fd); return VKU_TAKEN; }
    if (idx < 0 || !vkp_image_renderable(dst) || vkp_image_format(dst) != VKU_FMT || !vkp_image_framebuffer(dst, G.rp)) {
        if (idx >= 0) sc_layer_vortek_pool_put(f->slot, idx, rel_fd);
        disable(idx < 0 ? "its layer buffers could not be allocated" : "its layer buffers cannot be rendered into");
        drop_frame(f, acquire_fd);
        return VKU_TAKEN;
    }
    if (filter == F_FSR && mid_ensure(pw, ph) != 0) {
        sc_layer_vortek_pool_put(f->slot, idx, rel_fd);
        drop_frame(f, acquire_fd);
        return VKU_TAKEN;
    }
    j->src = src;
    j->dst = dst;
    j->slot = f->slot;
    j->pool_idx = idx;
    j->filter = filter;
    j->sharp_pct = sharp;
    j->chain = f->chain;
    j->index = f->index;
    j->bw = vkp_image_width(src->img);
    j->bh = vkp_image_height(src->img);
    memcpy(j->crop, r, sizeof(j->crop));
    j->pw = pw;
    j->ph = ph;
    const int geo[8] = {0, 0, pw, ph, r[4], r[5], r[6], r[7]};
    memcpy(j->geo, geo, sizeof(geo));
    j->acquire_fd = acquire_fd;
    j->rel_fd = rel_fd;
    if (G.deferred && defer(j)) {
        G.layers[f->slot].pending = j;
        g_stat.deferred++;
        return VKU_TAKEN;
    }
    return run_job(j, f->z, batch) == 0 ? VKU_SHOWN : VKU_TAKEN;
direct:
    cancel_pending(f->slot);
    return VKU_DIRECT;
}

int vku_pending(int slot) {
    return slot >= 0 && slot < SC_VORTEK_LAYERS && G.layers[slot].pending != NULL;
}

void vku_layer_hidden(int slot) {
    if (slot >= 0 && slot < SC_VORTEK_LAYERS) cancel_pending(slot);
}

void vku_chain_gone(uint32_t chain) {
    if (!chain) return;
    for (int i = 0; i < SC_VORTEK_LAYERS; i++)
        if (G.layers[i].pending && G.layers[i].pending->chain == chain) cancel_pending(i);
    for (int i = 0; i < VKU_SRC_MAX; i++)
        if (G.src[i].chain == chain) G.src[i].dead = 1;
    reap();
}

void vku_idle(void) {
    if (!G.built) return;
    for (int i = 0; i < SC_VORTEK_LAYERS; i++) cancel_pending(i);
    for (int i = 0; i < VKU_RING; i++) job_wait(&G.ring[i]);
    reap();
}

void vku_tick(void) {
    if (!G.built) return;
    reap();
    const int64_t t = now_ns();
    for (int i = 0; i < SC_VORTEK_LAYERS; i++) {
        if (!G.layers[i].last_ns || G.layers[i].pending || t - G.layers[i].last_ns < VKU_IDLE_NS) continue;
        if (zfg_layer_recent(i)) continue;
        if (sc_layer_vortek_pool_trim(i) == 0) G.layers[i].last_ns = 0;
    }
    if (G.mid.img && t - G.mid.used_ns > VKU_IDLE_NS && !in_flight(G.mid.last_serial)) mid_destroy();
}

int vku_filter_selected(const char **name) {
    int sharp = 0;
    const char *why = NULL;
    const int filter = filter_mode(&sharp, &why);
    if (name) *name = filter_names[filter];
    return filter != F_OFF;
}

int vku_layer_recent(int slot) {
    return slot >= 0 && slot < SC_VORTEK_LAYERS && G.layers[slot].last_ns && now_ns() - G.layers[slot].last_ns < VKU_IDLE_NS;
}

void vku_perf_take(char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (!g_stat.frames && g_stat.drops) {
        snprintf(out, n, " | zero-copy upscale: %u frames dropped", g_stat.drops);
    } else if (g_stat.frames) {
        int off = snprintf(out, n, " | zero-copy upscale %s %dx%d->%dx%d: %u frames (%u waited for on the CPU), %u dropped",
                           filter_names[g_stat.filter], g_stat.bw, g_stat.bh, g_stat.pw, g_stat.ph, g_stat.frames,
                           g_stat.deferred, g_stat.drops);
        if (g_stat.timed && off > 0 && (size_t)off < n)
            off += snprintf(out + off, n - (size_t)off, ", pass %.2f/%.2f ms",
                            (double)g_stat.pass_ns / 1e6 / g_stat.timed, (double)g_stat.pass_max_ns / 1e6);
        if (g_stat.starts && off > 0 && (size_t)off < n)
            snprintf(out + off, n - (size_t)off, ", started %.2f/%.2f ms after submit",
                     (double)g_stat.start_ns / 1e6 / g_stat.starts, (double)g_stat.start_max_ns / 1e6);
    }
    memset(&g_stat, 0, sizeof(g_stat));
}
