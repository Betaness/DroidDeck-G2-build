/*
 * The Mali workarounds of the Vortek client (mali-g2/vortek/src/droiddeck_maint.c) that are about the
 * driver rather than Vortek's wire protocol; with the driver in this process, maintenance5/6,
 * graphics pipeline libraries and the extension filter (ext_commands.h) need nothing.
 *   - ClipDistance / CullDistance stores are removed from shaders: the Mali shader compiler crashes
 *     on them (spirv_clip_cull.h). VORTEK_KEEP_CLIP_DISTANCE=1 keeps them.
 *   - Constant composites built from specialization constants become spec-constant composites: the
 *     Mali compiler folds them at the constituents' defaults (spirv_spec_composite.h), which breaks
 *     DXVK's texture bound checks. Shader modules, inline stages of graphics, compute and ray tracing
 *     pipelines (the VkShaderModuleCreateInfo anywhere in the stage's pNext chain,
 *     stage_inline_module.h), and vkGetShaderModuleCreateInfoIdentifierEXT, so the identifier is the
 *     one of the module's code. DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX=0 (else
 *     VORTEK_SPEC_COMPOSITE_FIX=0) leaves them.
 *   - vkd3d-proton is told storage texel buffers need single-texel alignment only
 *     (VORTEK_STRICT_TEXEL_ALIGNMENT=1 reports the driver's value).
 *   - vkGetImageSubresourceLayout and its 2/2KHR/2EXT and vkGetDeviceImageSubresourceLayout(KHR)
 *     variants: the rowPitch 0 Mali reports for one-row LINEAR subresources is replaced by the
 *     row's size (linear_row_pitch.h).
 *   - VK_EXT_vertex_attribute_divisor stands in for the driver's KHR extension (icd.c); its
 *     properties structure is filled from the KHR one here.
 *   - Core features DXVK requires that the Mali lacks are reported, as the Vortek server reports
 *     them (vortekrenderer's checkDeviceFeatures), and turned off again at vkCreateDevice (icd.c):
 *     fillModeNonSolid (what line or point fill then draws is up to the driver, as through Vortek),
 *     and shaderClipDistance / shaderCullDistance while their writes are removed from shaders.
 *     Without them DXVK finds no adapter ("Device does not support required feature
 *     'fillModeNonSolid'").
 *   - Graphics pipeline library parts and links: the state of the subsets a call does not provide
 *     may dangle (the spec ignores it), so it is dropped from copies of the create infos before
 *     the wrappers here read pStages (../vortek/src/gpl_library_state.h, shared with the Vortek
 *     client), and the driver gets the copies (tests/gpl_test.sh).
 *   - Pipelines the application creates without a VkPipelineCache of its own (DXVK) use the
 *     device's persistent implicit cache (pipeline_cache_hybris.c, the Vortek server's code).
 *   - Command pool and command buffer resets add RELEASE_RESOURCES as the Vortek server's do
 *     (winlator-0018): the Mali keeps ~420 bytes of host heap per vkCmdBeginRendering recorded into
 *     command pool memory that has held a query, until the pool (or, for clients that reset single
 *     buffers, the buffer) is reset with RELEASE_RESOURCES, and DXVK resets its pools with flags 0,
 *     so the game process grows by tens of MB a minute
 *     (vortek/tests/frame_leak_test.c). By default a reset releases when the estimate of what the
 *     driver keeps (vortek/src/cmd_memory.h, from the vkCmdBeginRendering and query commands
 *     recorded here) reaches DROIDDECK_HYBRIS_CMD_RELEASE_BYTES (else VORTEK_CMD_RELEASE_BYTES,
 *     default 16m) for its pool, or *_CMD_RELEASE_TOTAL_BYTES (default 2 x that) over all pools;
 *     vkBeginCommandBuffer of a command buffer due for release resets it with RELEASE_RESOURCES
 *     first when its pool allows single resets. *_CMD_RELEASE_LOG=1 logs every release. With
 *     DROIDDECK_HYBRIS_CMD_RELEASE_EVERY (else VORTEK_CMD_RELEASE_EVERY) set, every Nth reset of the
 *     same pool or command buffer releases instead (1 = every reset, 0 = never) and nothing is
 *     counted: only the two reset calls are wrapped then (a value that is not a number is ignored).
 *     *_CMD_RELEASE_MODE=every forces that (32 without a number), =adaptive the estimate
 *     (cmdmem_release_policy). The estimate stays the default here; the Vortek server's default is
 *     every 32nd reset.
 *   - D3D11 stream output is counted (vortek/src/xfb_stats.h): graphics pipelines with a stage that
 *     declares transform feedback or with a geometry shader, vkCmdBeginTransformFeedbackEXT and
 *     vkCmdDrawIndirectByteCountEXT calls, logged as "hybris-icd: xfb stats: ..." once something is
 *     counted, for the play journal. Counters only; vkDestroyShaderModule is wrapped for the handles
 *     of modules that declare it.
 *   - DROIDDECK_HYBRIS_PASS_BARRIER=log|narrow|narrow-rt|nogeo (pass_barrier.h): vkCmdPipelineBarrier2
 *     and ...2KHR are wrapped to count DXVK's end-of-render-pass barriers and, in the narrowing modes,
 *     drop the vertex pipeline stages from their destination. Shader code the driver gets (modules,
 *     inline stages, identifier queries) is scanned for the guards until they trip. Unset or off:
 *     nothing is wrapped or scanned. A line "hybris-icd: pass barrier: ..." every
 *     DROIDDECK_HYBRIS_PASS_BARRIER_LOG_S seconds (60) of presents, and at device destruction.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "icd.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spirv_clip_cull.h"
#include "spirv_spec_composite.h"
#include "stage_inline_module.h"
#include "gpl_library_state.h"
#include "linear_row_pitch.h"
#include "pipeline_cache.h"
#define CMDMEM_LOG(fmt, ...) fprintf(stderr, "hybris-icd: " fmt "\n", ##__VA_ARGS__)
#include "cmd_memory.h"
#include "crash_guard.h"
#include "xfb_stats.h"
#include "pass_barrier.h"

#define XFB_TAG "hybris-icd"

static uint64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- DROIDDECK_HYBRIS_PASS_BARRIER: state; the wrappers are further down ---- */

static PbMode pbMode;
static uint32_t pbLogSeconds = 60;
static pthread_once_t pbOnce = PTHREAD_ONCE_INIT;
static uint32_t pbXfb, pbVertexStores, pbGeoImageReads; /* guards (atomic) */
static PbCounts pbTotal;                                /* atomic fields */
static uint64_t pbPresents, pbStartNs, pbLastLogNs, pbLoggedCalls = UINT64_MAX;

static void pbReadMode(void) {
    const char* text = getenv("DROIDDECK_HYBRIS_PASS_BARRIER");
    bool unknown;
    pbMode = pb_parse_mode(text, &unknown);
    if (unknown) fprintf(stderr, "hybris-icd: DROIDDECK_HYBRIS_PASS_BARRIER=%s ignored (off, log, narrow, narrow-rt or nogeo)\n", text);
    if (pbMode == PB_OFF) return;
    int seconds = icd_env_int("DROIDDECK_HYBRIS_PASS_BARRIER_LOG_S", 60);
    pbLogSeconds = seconds > 0 ? (uint32_t)seconds : 60;
    pbStartNs = pbLastLogNs = nowNs();
    fprintf(stderr, "hybris-icd: DROIDDECK_HYBRIS_PASS_BARRIER=%s, counts every %u s\n", pb_mode_names[pbMode], pbLogSeconds);
}

static PbMode pbModeNow(void) {
    pthread_once(&pbOnce, pbReadMode);
    return pbMode;
}

/* Shader code handed to the driver (`what`, and the module's handle when there is one): the guards not
 * tripped yet. */
static void pbShaderSeen(const VkShaderModuleCreateInfo* info, const char* what, uint64_t handle) {
    if (pbModeNow() == PB_OFF || !info || !info->pCode) return;
    size_t words = info->codeSize / 4;
    bool xfb = !__atomic_load_n(&pbXfb, __ATOMIC_RELAXED) && xfb_spirv_declares(info->pCode, words);
    unsigned want = (__atomic_load_n(&pbGeoImageReads, __ATOMIC_RELAXED) ? 0 : PB_SCAN_IMAGE_READS) |
                    (__atomic_load_n(&pbVertexStores, __ATOMIC_RELAXED) ? 0 : PB_SCAN_WRITES);
    unsigned found = want ? pb_spirv_geo_scan(info->pCode, words, want) : 0;
    if (!xfb && !found) return;
    char at[24] = "";
    if (handle) snprintf(at, sizeof(at), " 0x%llx", (unsigned long long)handle);
    if (xfb && !__atomic_exchange_n(&pbXfb, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: pass barrier guard: transform feedback (%s%s)\n", what, at);
    if ((found & PB_SCAN_IMAGE_READS) && !__atomic_exchange_n(&pbGeoImageReads, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: pass barrier guard: vertex pipeline image reads (%s%s)\n", what, at);
    if ((found & PB_SCAN_WRITES) && !__atomic_exchange_n(&pbVertexStores, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: pass barrier guard: vertex pipeline memory writes (%s%s)\n", what, at);
}

static void* findInChain(const void* chain, VkStructureType type) {
    for (const VkBaseInStructure* s = chain; s; s = s->pNext)
        if (s->sType == type) return (void*)s;
    return NULL;
}

static void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    /* The driver does not know the EXT divisor structure: it is unlinked for the call. */
    VkBaseOutStructure* prev = (VkBaseOutStructure*)properties;
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT* divExt = NULL;
    while (prev->pNext && prev->pNext->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT) prev = prev->pNext;
    if (prev->pNext) {
        divExt = (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT*)prev->pNext;
        prev->pNext = divExt->pNext;
    }
    instance->GetPhysicalDeviceProperties2(physical, properties);
    if (divExt) {
        prev->pNext = (VkBaseOutStructure*)divExt;
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR khr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &khr};
        instance->GetPhysicalDeviceProperties2(physical, &query);
        divExt->maxVertexAttribDivisor = khr.maxVertexAttribDivisor;
    }
    /* vkd3d-proton refuses a device without single-texel alignment for storage texel buffers; the
     * Mali asks for 64-byte offsets. D3D12 games create typed UAV buffer views at element offsets
     * that are aligned in practice, so vkd3d-proton (only) is told the Mali has it. */
    if (instance->vkd3d && !icd_env_int("VORTEK_STRICT_TEXEL_ALIGNMENT", 0)) {
        VkPhysicalDeviceVulkan13Properties* p13 = findInChain(properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
        if (p13) p13->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        VkPhysicalDeviceTexelBufferAlignmentProperties* tba =
            findInChain(properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES);
        if (tba) tba->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    }
    if (findInChain(properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT) &&
        icd_placed_available(instance, physical))
        placed_fill_properties(properties->pNext);
}

static bool clipStripOn(void) {
    static int on = -1;
    if (on < 0) on = !icd_env_int("VORTEK_KEEP_CLIP_DISTANCE", 0);
    return on;
}

static void emulateFeatures(VkPhysicalDeviceFeatures* features) {
    features->fillModeNonSolid = VK_TRUE;
    if (clipStripOn()) {
        features->shaderClipDistance = VK_TRUE;
        features->shaderCullDistance = VK_TRUE;
    }
}

static void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures* features) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    instance->GetPhysicalDeviceFeatures(physical, features);
    emulateFeatures(features);
}

static void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    instance->GetPhysicalDeviceFeatures2(physical, features);
    emulateFeatures(&features->features);
    if (findInChain(features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) &&
        icd_placed_available(instance, physical))
        placed_fill_features(features->pNext);
}

void mali_drop_emulated_features(Instance* instance, VkPhysicalDevice physical, VkPhysicalDeviceFeatures* enabled) {
    VkPhysicalDeviceFeatures driver = {0};
    instance->GetPhysicalDeviceFeatures(physical, &driver);
    VkPhysicalDeviceFeatures emulated = driver;
    emulateFeatures(&emulated);
    VkBool32* want = (VkBool32*)enabled;
    const VkBool32* has = (const VkBool32*)&driver;
    const VkBool32* fake = (const VkBool32*)&emulated;
    for (size_t i = 0; i < sizeof(*enabled) / sizeof(VkBool32); i++)
        if (want[i] && !has[i] && fake[i]) want[i] = VK_FALSE;
}

static bool specFixOn(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX");
        if (!e || !*e) e = getenv("VORTEK_SPEC_COMPOSITE_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

static bool rewritesOn(void) {
    return clipStripOn() || specFixOn();
}

/* A rewritten copy of `info` in *out (code to free in *freeCode), or false to use `info`: clip/cull
 * distance stores removed, spec-constant composites made specializable. */
static bool rewriteShader(const VkShaderModuleCreateInfo* info, VkShaderModuleCreateInfo* out, uint32_t** freeCode) {
    if (!info->pCode || info->codeSize < 20) return false;
    size_t words = info->codeSize / 4;
    uint32_t* code = clipStripOn() ? dd_strip_clip_cull(info->pCode, words, &words) : NULL;
    if (code) {
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "hybris-icd: clip/cull distance writes removed from a shader (the Mali compiler crashes on them)\n");
    }
    if (specFixOn() && dd_spec_composites(code ? code : info->pCode, words, NULL)) {
        if (!code && (code = malloc(words * 4))) memcpy(code, info->pCode, words * 4);
        if (code) {
            size_t n = dd_spec_composites(code, words, code);
            static int logged;
            if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
                fprintf(stderr, "hybris-icd: %zu spec-constant composite(s) made specializable in a shader (Mali folds them at their defaults)\n", n);
        }
    }
    if (!code) return false;
    *out = *info;
    out->pCode = code;
    out->codeSize = words * 4;
    *freeCode = code;
    return true;
}

static VkResult VKAPI_CALL CreateShaderModule(VkDevice handle, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator,
                                              VkShaderModule* module) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkShaderModuleCreateInfo rewritten;
    uint32_t* code = NULL;
    VkResult result = device->CreateShaderModule(handle, rewriteShader(info, &rewritten, &code) ? &rewritten : info, allocator, module);
    free(code);
    if (result == VK_SUCCESS) {
        xfb_stats_module_created((uint64_t)*module, info);
        pbShaderSeen(info, "shader module", (uint64_t)*module);
    }
    return result;
}

static void VKAPI_CALL DestroyShaderModule(VkDevice handle, VkShaderModule module, const VkAllocationCallbacks* allocator) {
    Device* device = icd_device(handle);
    if (!device) return;
    xfb_stats_module_destroyed((uint64_t)module);
    device->DestroyShaderModule(handle, module, allocator);
}

/* A stage's inline code (VkShaderModuleCreateInfo anywhere in its pNext chain) rewritten: the stage
 * copy, the rewritten module, its code and the copied structures ahead of it in the chain. */
typedef struct {
    VkShaderModuleCreateInfo module;
    uint32_t* code;
    DdStageChain chain;
} StageCopy;

typedef struct {
    VkPipelineShaderStageCreateInfo* stages; /* NULL: nothing rewritten, use the application's */
    StageCopy* copies;
    uint32_t count;
} StagesRewrite;

static bool anyInline(const VkPipelineShaderStageCreateInfo* stages, uint32_t count) {
    for (uint32_t s = 0; s < count; s++)
        if (dd_inline_module(stages[s].pNext)) return true;
    return false;
}

/* Copies of `count` stages in *r with their inline code rewritten; false (r->stages NULL) when none
 * changed. */
static bool rewriteStages(const VkPipelineShaderStageCreateInfo* stages, uint32_t count, StagesRewrite* r) {
    memset(r, 0, sizeof(*r));
    if (!count || !rewritesOn() || !anyInline(stages, count)) return false;
    r->stages = malloc(count * sizeof(*r->stages));
    r->copies = calloc(count, sizeof(*r->copies));
    r->count = count;
    bool any = false;
    for (uint32_t s = 0; r->stages && r->copies && s < count; s++) {
        r->stages[s] = stages[s];
        const VkShaderModuleCreateInfo* inl = dd_inline_module(stages[s].pNext);
        StageCopy* c = &r->copies[s];
        if (!inl || !rewriteShader(inl, &c->module, &c->code)) continue;
        const void* head = dd_replace_inline_module(stages[s].pNext, inl, &c->module, &c->chain);
        if (!head) {
            free(c->code);
            c->code = NULL;
            static int logged;
            if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
                fprintf(stderr, "hybris-icd: inline shader code behind an unknown stage structure left as it is\n");
            continue;
        }
        r->stages[s].pNext = head;
        any = true;
    }
    if (!any) {
        for (uint32_t s = 0; r->copies && s < count; s++) free(r->copies[s].code);
        free(r->copies);
        free(r->stages);
        memset(r, 0, sizeof(*r));
    }
    return any;
}

static void freeStages(StagesRewrite* r) {
    for (uint32_t s = 0; r->copies && s < r->count; s++) free(r->copies[s].code);
    free(r->copies);
    free(r->stages);
}

/* Inline stage code (DXVK 3.x, pipeline libraries): stages with a VkShaderModuleCreateInfo in their
 * pNext chain are copied with the rewritten code. Without a cache of the application's, pipelines
 * go through the persistent one (pipeline_cache_hybris.c).
 * The state a library part or a link does not provide may dangle (pStages of a vertex input or
 * fragment output part, of a fast link): when the call has such a part, the create infos are
 * copied with that state dropped first (dd_gpl_drop_ignored_state, as the Vortek client does),
 * and the stage rewrites, the cache choice, the xfb statistics and the driver get the copies;
 * infos[i].pStages is never read. Calls with complete pipelines only are passed on as before. */
static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                   const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                                   VkPipeline* pipelines) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    bool partial = false;
    uint32_t libraryCount;
    for (uint32_t i = 0; i < count && !partial; i++)
        partial = (dd_gpl_parts(&infos[i], &libraryCount) & DD_GPL_ALL_PARTS) != DD_GPL_ALL_PARTS;
    VkGraphicsPipelineCreateInfo* copy = NULL;
    if (partial) {
        if (!(copy = malloc(count * sizeof(*copy)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(copy, infos, count * sizeof(*copy));
        for (uint32_t i = 0; i < count; i++) dd_gpl_drop_ignored_state(&copy[i]);
    }
    const VkGraphicsPipelineCreateInfo* view = copy ? copy : infos; /* pStages safe to read */
    VkPipelineCache use = PipelineCache_forGraphics(handle, cache, view, count);
    for (uint32_t i = 0; i < count && pbModeNow() != PB_OFF; i++)
        for (uint32_t s = 0; view[i].pStages && s < view[i].stageCount; s++) pbShaderSeen(dd_inline_module(view[i].pStages[s].pNext), "inline stage", 0);
    uint64_t start = nowNs();
    bool inline_ = false;
    for (uint32_t i = 0; i < count && rewritesOn() && !inline_; i++) inline_ = anyInline(view[i].pStages, view[i].stageCount);
    if (inline_ && !copy && (copy = malloc(count * sizeof(*copy)))) memcpy(copy, infos, count * sizeof(*copy));
    StagesRewrite* rewrites = inline_ && copy ? calloc(count, sizeof(*rewrites)) : NULL;
    bool any = partial;
    for (uint32_t i = 0; rewrites && i < count; i++) {
        if (!rewriteStages(copy[i].pStages, copy[i].stageCount, &rewrites[i])) continue;
        copy[i].pStages = rewrites[i].stages;
        any = true;
    }
    VkResult result = guards_create_graphics_pipelines(device, handle, use, count, any ? copy : infos, allocator, pipelines);
    if (result == VK_SUCCESS) PipelineCache_noteCreated(handle, count, nowNs() - start, use && use != cache);
    if (result >= 0) xfb_stats_pipelines_created(XFB_TAG, count, view, pipelines);
    for (uint32_t i = 0; rewrites && i < count; i++) freeStages(&rewrites[i]);
    free(rewrites);
    free(copy);
    return result;
}

static VkResult VKAPI_CALL CreateComputePipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                  const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                                  VkPipeline* pipelines) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkPipelineCache use = PipelineCache_forCompute(handle, cache, infos, count);
    uint64_t start = nowNs();
    bool inline_ = false;
    for (uint32_t i = 0; i < count && rewritesOn() && !inline_; i++) inline_ = anyInline(&infos[i].stage, 1);
    VkComputePipelineCreateInfo* copy = inline_ ? malloc(count * sizeof(*copy)) : NULL;
    StagesRewrite* rewrites = inline_ ? calloc(count, sizeof(*rewrites)) : NULL;
    bool any = false;
    if (copy && rewrites) {
        memcpy(copy, infos, count * sizeof(*copy));
        for (uint32_t i = 0; i < count; i++) {
            if (!rewriteStages(&infos[i].stage, 1, &rewrites[i])) continue;
            copy[i].stage = rewrites[i].stages[0];
            any = true;
        }
    }
    VkResult result = guards_create_compute_pipelines(device, handle, use, count, any ? copy : infos, allocator, pipelines);
    if (result == VK_SUCCESS) PipelineCache_noteCreated(handle, count, nowNs() - start, use && use != cache);
    for (uint32_t i = 0; rewrites && i < count; i++) freeStages(&rewrites[i]);
    free(rewrites);
    free(copy);
    return result;
}

/* Ray tracing stages get the same rewrites (vkd3d-proton's DXR). The copies live until the call
 * returns, so a call they changed runs without the deferred operation: the driver may read the
 * create infos until a deferred operation completes. VK_OPERATION_NOT_DEFERRED_KHR says it is done. */
static VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(VkDevice handle, VkDeferredOperationKHR deferred, VkPipelineCache cache,
                                                        uint32_t count, const VkRayTracingPipelineCreateInfoKHR* infos,
                                                        const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    Device* device = icd_device(handle);
    if (!device || !device->CreateRayTracingPipelinesKHR) return VK_ERROR_DEVICE_LOST;
    bool inline_ = false;
    for (uint32_t i = 0; i < count && rewritesOn() && !inline_; i++) inline_ = anyInline(infos[i].pStages, infos[i].stageCount);
    VkRayTracingPipelineCreateInfoKHR* copy = inline_ ? malloc(count * sizeof(*copy)) : NULL;
    StagesRewrite* rewrites = inline_ ? calloc(count, sizeof(*rewrites)) : NULL;
    bool any = false;
    if (copy && rewrites) {
        memcpy(copy, infos, count * sizeof(*copy));
        for (uint32_t i = 0; i < count; i++) {
            if (!rewriteStages(infos[i].pStages, infos[i].stageCount, &rewrites[i])) continue;
            copy[i].pStages = rewrites[i].stages;
            any = true;
        }
    }
    VkResult result = device->CreateRayTracingPipelinesKHR(handle, any ? VK_NULL_HANDLE : deferred, cache, count, any ? copy : infos,
                                                           allocator, pipelines);
    if (any && deferred && result == VK_SUCCESS) result = VK_OPERATION_NOT_DEFERRED_KHR;
    for (uint32_t i = 0; rewrites && i < count; i++) freeStages(&rewrites[i]);
    free(rewrites);
    free(copy);
    return result;
}

/* The identifier of the code the module is made of (rewritten), as vkGetShaderModuleIdentifierEXT
 * of that module gives: DXVK looks pipelines up by it. */
static void VKAPI_CALL GetShaderModuleCreateInfoIdentifierEXT(VkDevice handle, const VkShaderModuleCreateInfo* info,
                                                              VkShaderModuleIdentifierEXT* identifier) {
    Device* device = icd_device(handle);
    if (!device || !device->GetShaderModuleCreateInfoIdentifierEXT) return;
    VkShaderModuleCreateInfo rewritten;
    uint32_t* code = NULL;
    device->GetShaderModuleCreateInfoIdentifierEXT(handle, rewriteShader(info, &rewritten, &code) ? &rewritten : info, identifier);
    free(code);
    pbShaderSeen(info, "identifier query", 0);
}

static void fixRowPitch(VkSubresourceLayout* layout) {
    if (!dd_fix_linear_row_pitch(layout)) return;
    static int logged;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: rowPitch 0 of a one-row linear image reported as %llu (Mali)\n", (unsigned long long)layout->rowPitch);
}

static void VKAPI_CALL GetImageSubresourceLayout(VkDevice handle, VkImage image, const VkImageSubresource* subresource,
                                                 VkSubresourceLayout* layout) {
    Device* device = icd_device(handle);
    if (!device) return;
    device->GetImageSubresourceLayout(handle, image, subresource, layout);
    fixRowPitch(layout);
}

/* vkGetImageSubresourceLayout2, ...2KHR and ...2EXT are one command under three names. */
static void VKAPI_CALL GetImageSubresourceLayout2(VkDevice handle, VkImage image, const VkImageSubresource2KHR* subresource,
                                                  VkSubresourceLayout2KHR* layout) {
    Device* device = icd_device(handle);
    if (!device) return;
    PFN_vkGetImageSubresourceLayout2KHR next = device->GetImageSubresourceLayout2 ? device->GetImageSubresourceLayout2
                                             : device->GetImageSubresourceLayout2KHR ? device->GetImageSubresourceLayout2KHR
                                                                                     : device->GetImageSubresourceLayout2EXT;
    if (next) next(handle, image, subresource, layout);
    else device->GetImageSubresourceLayout(handle, image, &subresource->imageSubresource, &layout->subresourceLayout);
    fixRowPitch(&layout->subresourceLayout);
}

static void VKAPI_CALL GetDeviceImageSubresourceLayout(VkDevice handle, const VkDeviceImageSubresourceInfoKHR* info,
                                                       VkSubresourceLayout2KHR* layout) {
    Device* device = icd_device(handle);
    if (!device) return;
    PFN_vkGetDeviceImageSubresourceLayoutKHR next =
        device->GetDeviceImageSubresourceLayout ? device->GetDeviceImageSubresourceLayout : device->GetDeviceImageSubresourceLayoutKHR;
    if (!next) return;
    next(handle, info, layout);
    fixRowPitch(&layout->subresourceLayout);
}

/* The wrapper of a layout command the device's driver has (any for VK_NULL_HANDLE), else NULL. */
/* The wrappers of commands of extensions the driver may lack: handed out when it has them (always
 * without a device, for vkGetInstanceProcAddr). */
static PFN_vkVoidFunction driverProc(VkDevice handle, const char* name) {
    static const struct { const char* name; size_t offset; PFN_vkVoidFunction wrapper; } procs[] = {
        {"vkCreateRayTracingPipelinesKHR", offsetof(Device, CreateRayTracingPipelinesKHR), (PFN_vkVoidFunction)CreateRayTracingPipelinesKHR},
        {"vkGetShaderModuleCreateInfoIdentifierEXT", offsetof(Device, GetShaderModuleCreateInfoIdentifierEXT),
         (PFN_vkVoidFunction)GetShaderModuleCreateInfoIdentifierEXT},
    };
    for (size_t i = 0; i < ARRAY_LEN(procs); i++) {
        if (strcmp(name, procs[i].name)) continue;
        if (!handle) return procs[i].wrapper;
        Device* device = icd_device(handle);
        return device && *(PFN_vkVoidFunction*)((char*)device + procs[i].offset) ? procs[i].wrapper : NULL;
    }
    return NULL;
}

static PFN_vkVoidFunction layoutProc(VkDevice handle, const char* name) {
    static const struct { const char* name; size_t offset; PFN_vkVoidFunction wrapper; } layouts[] = {
        {"vkGetImageSubresourceLayout2", offsetof(Device, GetImageSubresourceLayout2), (PFN_vkVoidFunction)GetImageSubresourceLayout2},
        {"vkGetImageSubresourceLayout2KHR", offsetof(Device, GetImageSubresourceLayout2KHR), (PFN_vkVoidFunction)GetImageSubresourceLayout2},
        {"vkGetImageSubresourceLayout2EXT", offsetof(Device, GetImageSubresourceLayout2EXT), (PFN_vkVoidFunction)GetImageSubresourceLayout2},
        {"vkGetDeviceImageSubresourceLayout", offsetof(Device, GetDeviceImageSubresourceLayout),
         (PFN_vkVoidFunction)GetDeviceImageSubresourceLayout},
        {"vkGetDeviceImageSubresourceLayoutKHR", offsetof(Device, GetDeviceImageSubresourceLayoutKHR),
         (PFN_vkVoidFunction)GetDeviceImageSubresourceLayout},
    };
    for (size_t i = 0; i < ARRAY_LEN(layouts); i++) {
        if (strcmp(name, layouts[i].name)) continue;
        if (!handle) return layouts[i].wrapper;
        Device* device = icd_device(handle);
        return device && *(PFN_vkVoidFunction*)((char*)device + layouts[i].offset) ? layouts[i].wrapper : NULL;
    }
    return NULL;
}

/* *_CMD_RELEASE_*: the DROIDDECK_HYBRIS_ variable, else the VORTEK_ one (droiddeck-env sets those for
 * the server); `name` (64 bytes) gets the one read. */
static const char* releaseEnv(const char* suffix, char* name) {
    snprintf(name, 64, "DROIDDECK_HYBRIS_CMD_RELEASE_%s", suffix);
    const char* text = getenv(name);
    if (!text || !*text) {
        snprintf(name, 64, "VORTEK_CMD_RELEASE_%s", suffix);
        text = getenv(name);
    }
    return text && *text ? text : NULL;
}

/* The fixed release interval (-1: by the estimate) and, without one, the estimate. Read once, before
 * the first device's procs are handed out. */
static int releaseEvery_ = -2;
static CmdMem* commandMemory;
static pthread_once_t releaseOnce = PTHREAD_ONCE_INIT;

static void readReleaseConfig(void) {
    char modeName[64], everyName[64], bytesName[64], totalName[64], logName[64];
    const char* mode = releaseEnv("MODE", modeName);
    const char* text = releaseEnv("EVERY", everyName);
    const char* bytes = releaseEnv("BYTES", bytesName);
    const char* totalBytes = releaseEnv("TOTAL_BYTES", totalName);
    const char* ignored;
    /* the default stays the estimate here (the Vortek server's is every 32nd reset) */
    int every = cmdmem_release_policy(mode, text, bytes || totalBytes, -1, &ignored);
    if (ignored) fprintf(stderr, "hybris-icd: %s=%s ignored\n", *ignored == 'M' ? modeName : everyName, *ignored == 'M' ? mode : text);
    if (ignored && *ignored == 'M') mode = NULL;
    if (every >= 0) {
        releaseEvery_ = every;
        fprintf(stderr, "hybris-icd: %s=%s, command memory released every %d resets\n", mode ? modeName : everyName,
                mode ? mode : text, releaseEvery_);
        return;
    }
    uint64_t perPool = cmdmem_parse_bytes(bytes, CMDMEM_DEFAULT_PER_POOL);
    uint64_t total = cmdmem_parse_bytes(totalBytes, CMDMEM_DEFAULT_TOTAL(perPool));
    const char* log = releaseEnv("LOG", logName);
    commandMemory = cmdmem_create(perPool, total, log && *log != '0');
    releaseEvery_ = commandMemory ? -1 : 32;
}

static int releaseEvery(void) {
    pthread_once(&releaseOnce, readReleaseConfig);
    return releaseEvery_;
}

#define RESET_SLOT_BITS 8
static pthread_mutex_t resetLock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t handle; uint32_t resets; } resetCounts[1u << RESET_SLOT_BITS];

/* The fixed interval: a handle new to its slot (first reset, or a collision) releases. */
static bool releaseOnReset(uint64_t handle) {
    int every = releaseEvery();
    if (every <= 1) return every == 1;
    uint32_t slot = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> (64 - RESET_SLOT_BITS));
    bool release = true;
    pthread_mutex_lock(&resetLock);
    if (resetCounts[slot].handle != handle) {
        resetCounts[slot].handle = handle;
        resetCounts[slot].resets = 0;
    }
    else if (++resetCounts[slot].resets < (uint32_t)every) release = false;
    else resetCounts[slot].resets = 0;
    pthread_mutex_unlock(&resetLock);
    return release;
}

static VkResult VKAPI_CALL ResetCommandPool(VkDevice handle, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (commandMemory ? cmdmem_pool_reset(commandMemory, (uint64_t)pool) : releaseOnReset((uint64_t)pool))
        flags |= VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT;
    return device->ResetCommandPool(handle, pool, flags);
}

/* Command buffer functions do not name their device. Theirs from libhybris are the Mali driver's own
 * (in libGLES_mali.so), the same for every device: the first non-null one is kept. */
static PFN_vkResetCommandBuffer nextResetCommandBuffer;

static void keepNext(void* slot, PFN_vkVoidFunction function) {
    void* expected = NULL;
    if (function) __atomic_compare_exchange_n((void**)slot, &expected, (void*)function, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static void pbLog(bool force);

void mali_device_created(Device* device) {
    releaseEvery(); /* before the device's first pool */
    keepNext(&nextResetCommandBuffer, (PFN_vkVoidFunction)device->ResetCommandBuffer);
    xfb_stats_device_created();
    if (pbModeNow() != PB_OFF && device->vertexStores && !__atomic_exchange_n(&pbVertexStores, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: pass barrier guard: vertexPipelineStoresAndAtomics enabled\n");
}

void mali_device_destroyed(Device* device) {
    cmdmem_device_destroyed(commandMemory, (uint64_t)(uintptr_t)device->handle);
    xfb_stats_flush(XFB_TAG);
    if (pbModeNow() != PB_OFF) pbLog(true);
}

void mali_presented(void) {
    xfb_stats_present(XFB_TAG);
    if (pbModeNow() != PB_OFF && !(__atomic_add_fetch(&pbPresents, 1, __ATOMIC_RELAXED) & 7)) pbLog(false);
}

/* ---- stream output counts (xfb_stats.h): the driver's two commands, kept like the reset's ---- */

static PFN_vkCmdBeginTransformFeedbackEXT nextBeginTransformFeedback;
static PFN_vkCmdDrawIndirectByteCountEXT nextDrawIndirectByteCount;

static void VKAPI_CALL CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer, uint32_t counterBufferCount,
                                                    const VkBuffer* counterBuffers, const VkDeviceSize* counterBufferOffsets) {
    xfb_stats_begin(XFB_TAG);
    __atomic_load_n(&nextBeginTransformFeedback, __ATOMIC_ACQUIRE)(commandBuffer, firstCounterBuffer, counterBufferCount, counterBuffers,
                                                                  counterBufferOffsets);
}

static void VKAPI_CALL CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount, uint32_t firstInstance,
                                                   VkBuffer counterBuffer, VkDeviceSize counterBufferOffset, uint32_t counterOffset,
                                                   uint32_t vertexStride) {
    xfb_stats_draw_auto(XFB_TAG);
    __atomic_load_n(&nextDrawIndirectByteCount, __ATOMIC_ACQUIRE)(commandBuffer, instanceCount, firstInstance, counterBuffer,
                                                                 counterBufferOffset, counterOffset, vertexStride);
}

/* The wrapper when the device's driver has the command (vkGetDeviceProcAddr with a device; without one
 * the driver's own is handed out, uncounted). */
static PFN_vkVoidFunction xfbProc(VkDevice handle, const char* name, void* slot, PFN_vkVoidFunction wrapper) {
    Device* device = handle ? icd_device(handle) : NULL;
    PFN_vkVoidFunction driver = device ? device->instance->GetDeviceProcAddr(handle, name) : NULL;
    if (!driver) return NULL;
    keepNext(slot, driver);
    return wrapper;
}

/* ---- DROIDDECK_HYBRIS_PASS_BARRIER (pass_barrier.h): the driver's barrier commands, kept like the reset's ---- */

static PFN_vkCmdPipelineBarrier2 nextPipelineBarrier2, nextPipelineBarrier2KHR;

/* The counts so far, unless (`force` aside) the interval has not passed, or no barrier came since the
 * last line. */
static void pbLog(bool force) {
    uint64_t last = __atomic_load_n(&pbLastLogNs, __ATOMIC_RELAXED);
    uint64_t now = nowNs();
    if (!force && ((int64_t)(now - last) < (int64_t)pbLogSeconds * 1000000000ll ||
                   !__atomic_compare_exchange_n(&pbLastLogNs, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)))
        return;
    if (force) __atomic_store_n(&pbLastLogNs, now, __ATOMIC_RELAXED);
    uint64_t calls = __atomic_load_n(&pbTotal.calls, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&pbLoggedCalls, calls, __ATOMIC_RELAXED) == calls) return;
    fprintf(stderr,
            "hybris-icd: pass barrier: pid %d, mode %s, seconds %llu, presents %llu, calls %llu, global %llu, global-writes %llu, "
            "attachment %llu, frag-to-geo %llu, narrowed %llu, global-dst 0x%llx, attachment-dst 0x%llx, "
            "guards xfb %d vertex-stores %d geo-image-reads %d\n",
            (int)getpid(), pb_mode_names[pbMode], (unsigned long long)((now - pbStartNs) / 1000000000ull),
            (unsigned long long)__atomic_load_n(&pbPresents, __ATOMIC_RELAXED), (unsigned long long)calls,
            (unsigned long long)__atomic_load_n(&pbTotal.global, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&pbTotal.globalWrites, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&pbTotal.attachment, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&pbTotal.fragToGeo, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&pbTotal.narrowed, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&pbTotal.globalDst, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&pbTotal.attachmentDst, __ATOMIC_RELAXED),
            __atomic_load_n(&pbXfb, __ATOMIC_RELAXED) || __atomic_load_n(&xfbStats.pipelines, __ATOMIC_RELAXED) ? 1 : 0,
            (int)__atomic_load_n(&pbVertexStores, __ATOMIC_RELAXED), (int)__atomic_load_n(&pbGeoImageReads, __ATOMIC_RELAXED));
}

static void pbAdd(uint64_t* total, uint64_t n) {
    if (n) __atomic_add_fetch(total, n, __ATOMIC_RELAXED);
}

static void passBarrier(PFN_vkCmdPipelineBarrier2 next, VkCommandBuffer commandBuffer, const VkDependencyInfo* info) {
    PbGuards guards = {
        .xfb = __atomic_load_n(&pbXfb, __ATOMIC_RELAXED) || __atomic_load_n(&xfbStats.pipelines, __ATOMIC_RELAXED),
        .vertexStores = __atomic_load_n(&pbVertexStores, __ATOMIC_RELAXED),
        .geoImageReads = __atomic_load_n(&pbGeoImageReads, __ATOMIC_RELAXED),
    };
    PbCounts n = {0};
    PbCopy copy;
    next(commandBuffer, pb_rewrite(pbMode, &guards, info, &copy, &n));
    free(copy.heap);
    pbAdd(&pbTotal.calls, n.calls);
    pbAdd(&pbTotal.global, n.global);
    pbAdd(&pbTotal.globalWrites, n.globalWrites);
    pbAdd(&pbTotal.attachment, n.attachment);
    pbAdd(&pbTotal.fragToGeo, n.fragToGeo);
    pbAdd(&pbTotal.narrowed, n.narrowed);
    if (n.globalDst) __atomic_store_n(&pbTotal.globalDst, n.globalDst, __ATOMIC_RELAXED);
    if (n.attachmentDst) __atomic_store_n(&pbTotal.attachmentDst, n.attachmentDst, __ATOMIC_RELAXED);
}

static void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* info) {
    passBarrier(__atomic_load_n(&nextPipelineBarrier2, __ATOMIC_ACQUIRE), commandBuffer, info);
}

static void VKAPI_CALL CmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const VkDependencyInfo* info) {
    passBarrier(__atomic_load_n(&nextPipelineBarrier2KHR, __ATOMIC_ACQUIRE), commandBuffer, info);
}

static VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    PFN_vkResetCommandBuffer next = __atomic_load_n(&nextResetCommandBuffer, __ATOMIC_ACQUIRE);
    if (!next) return VK_ERROR_DEVICE_LOST;
    uint64_t key = (uint64_t)(uintptr_t)commandBuffer;
    if (commandMemory ? cmdmem_buffer_reset(commandMemory, key) : releaseOnReset(key)) flags |= VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT;
    return next(commandBuffer, flags);
}

/* ---- the estimate (no fixed interval): pools, command buffers and what they record ---- */

static VkResult VKAPI_CALL CreateCommandPool(VkDevice handle, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* allocator,
                                             VkCommandPool* pool) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkResult result = device->CreateCommandPool(handle, info, allocator, pool);
    if (result == VK_SUCCESS)
        cmdmem_pool_created(commandMemory, (uint64_t)(uintptr_t)handle, (uint64_t)*pool,
                            info->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    return result;
}

static void VKAPI_CALL DestroyCommandPool(VkDevice handle, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    Device* device = icd_device(handle);
    if (!device) return;
    cmdmem_pool_destroyed(commandMemory, (uint64_t)pool);
    device->DestroyCommandPool(handle, pool, allocator);
}

static VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice handle, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkResult result = device->AllocateCommandBuffers(handle, info, buffers);
    if (result == VK_SUCCESS && sizeof(VkCommandBuffer) == sizeof(uint64_t))
        cmdmem_buffers_allocated(commandMemory, (uint64_t)info->commandPool, info->commandBufferCount, (const uint64_t*)buffers);
    return result;
}

static void VKAPI_CALL FreeCommandBuffers(VkDevice handle, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers) {
    Device* device = icd_device(handle);
    if (!device) return;
    if (buffers && sizeof(VkCommandBuffer) == sizeof(uint64_t)) cmdmem_buffers_freed(commandMemory, count, (const uint64_t*)buffers);
    device->FreeCommandBuffers(handle, pool, count, buffers);
}

/* The driver's command buffer functions wrapped for the estimate, index = position in trackedNames. */
enum {
    T_BEGIN, T_END, T_BEGIN_RENDERING, T_BEGIN_RENDERING_KHR, T_BEGIN_RENDER_PASS, T_BEGIN_RENDER_PASS2, T_BEGIN_RENDER_PASS2_KHR,
    T_BEGIN_QUERY, T_BEGIN_QUERY_INDEXED, T_WRITE_TIMESTAMP, T_WRITE_TIMESTAMP2, T_WRITE_TIMESTAMP2_KHR, T_COUNT
};
static const char* const trackedNames[T_COUNT] = {
    "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkCmdBeginRendering", "vkCmdBeginRenderingKHR", "vkCmdBeginRenderPass",
    "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR", "vkCmdBeginQuery", "vkCmdBeginQueryIndexedEXT", "vkCmdWriteTimestamp",
    "vkCmdWriteTimestamp2", "vkCmdWriteTimestamp2KHR"};
static PFN_vkVoidFunction trackedNext[T_COUNT];
#define NEXT(index, type) ((type)__atomic_load_n(&trackedNext[index], __ATOMIC_ACQUIRE))
#define KEY(commandBuffer) ((uint64_t)(uintptr_t)(commandBuffer))

static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* info) {
    PFN_vkResetCommandBuffer reset = __atomic_load_n(&nextResetCommandBuffer, __ATOMIC_ACQUIRE);
    if (cmdmem_begin(commandMemory, KEY(commandBuffer)) && reset) reset(commandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    VkResult result = NEXT(T_BEGIN, PFN_vkBeginCommandBuffer)(commandBuffer, info);
    if (result == VK_SUCCESS) guards_command_buffer_begun(commandBuffer);
    return result;
}

static VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    VkResult result = guards_end_command_buffer(NEXT(T_END, PFN_vkEndCommandBuffer), commandBuffer);
    cmdmem_end(commandMemory, KEY(commandBuffer));
    return result;
}

static void VKAPI_CALL CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* info) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 1, false, false);
    NEXT(T_BEGIN_RENDERING, PFN_vkCmdBeginRendering)(commandBuffer, info);
}

static void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* info) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 1, false, false);
    NEXT(T_BEGIN_RENDERING_KHR, PFN_vkCmdBeginRendering)(commandBuffer, info);
}

static void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, false, true);
    NEXT(T_BEGIN_RENDER_PASS, PFN_vkCmdBeginRenderPass)(commandBuffer, info, contents);
}

static void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* subpass) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, false, true);
    NEXT(T_BEGIN_RENDER_PASS2, PFN_vkCmdBeginRenderPass2)(commandBuffer, info, subpass);
}

static void VKAPI_CALL CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* subpass) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, false, true);
    NEXT(T_BEGIN_RENDER_PASS2_KHR, PFN_vkCmdBeginRenderPass2)(commandBuffer, info, subpass);
}

static void VKAPI_CALL CmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, true, false);
    NEXT(T_BEGIN_QUERY, PFN_vkCmdBeginQuery)(commandBuffer, pool, query, flags);
}

static void VKAPI_CALL CmdBeginQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags,
                                               uint32_t index) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, true, false);
    NEXT(T_BEGIN_QUERY_INDEXED, PFN_vkCmdBeginQueryIndexedEXT)(commandBuffer, pool, query, flags, index);
}

static void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, true, false);
    NEXT(T_WRITE_TIMESTAMP, PFN_vkCmdWriteTimestamp)(commandBuffer, stage, pool, query);
}

static void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, true, false);
    NEXT(T_WRITE_TIMESTAMP2, PFN_vkCmdWriteTimestamp2)(commandBuffer, stage, pool, query);
}

static void VKAPI_CALL CmdWriteTimestamp2KHR(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    cmdmem_recorded(commandMemory, KEY(commandBuffer), 0, true, false);
    NEXT(T_WRITE_TIMESTAMP2_KHR, PFN_vkCmdWriteTimestamp2)(commandBuffer, stage, pool, query);
}

static const PFN_vkVoidFunction trackedWrappers[T_COUNT] = {
    (PFN_vkVoidFunction)BeginCommandBuffer, (PFN_vkVoidFunction)EndCommandBuffer, (PFN_vkVoidFunction)CmdBeginRendering,
    (PFN_vkVoidFunction)CmdBeginRenderingKHR, (PFN_vkVoidFunction)CmdBeginRenderPass, (PFN_vkVoidFunction)CmdBeginRenderPass2,
    (PFN_vkVoidFunction)CmdBeginRenderPass2KHR, (PFN_vkVoidFunction)CmdBeginQuery, (PFN_vkVoidFunction)CmdBeginQueryIndexedEXT,
    (PFN_vkVoidFunction)CmdWriteTimestamp, (PFN_vkVoidFunction)CmdWriteTimestamp2, (PFN_vkVoidFunction)CmdWriteTimestamp2KHR};

/* A wrapper for the estimate when the device's driver has the function (vkGetDeviceProcAddr with a
 * device; without one the driver's own is handed out and that command is not counted). */
static PFN_vkVoidFunction trackedProc(VkDevice handle, const char* name) {
    if (!strcmp(name, "vkCreateCommandPool")) return (PFN_vkVoidFunction)CreateCommandPool;
    if (!strcmp(name, "vkDestroyCommandPool")) return (PFN_vkVoidFunction)DestroyCommandPool;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (PFN_vkVoidFunction)AllocateCommandBuffers;
    if (!strcmp(name, "vkFreeCommandBuffers")) return (PFN_vkVoidFunction)FreeCommandBuffers;
    for (int i = 0; i < T_COUNT; i++) {
        if (strcmp(name, trackedNames[i])) continue;
        Device* device = handle ? icd_device(handle) : NULL;
        PFN_vkVoidFunction driver = device ? device->instance->GetDeviceProcAddr(handle, name) : NULL;
        if (!driver) return NULL;
        keepNext(&trackedNext[i], driver);
        return trackedWrappers[i];
    }
    return NULL;
}

PFN_vkVoidFunction mali_instance_proc(const char* name) {
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures")) return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceProperties2;
    return NULL;
}

PFN_vkVoidFunction mali_device_proc(VkDevice handle, const char* name) {
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)CreateShaderModule;
    if (!strcmp(name, "vkDestroyShaderModule")) return (PFN_vkVoidFunction)DestroyShaderModule;
    if (!strcmp(name, "vkCmdBeginTransformFeedbackEXT"))
        return xfbProc(handle, name, &nextBeginTransformFeedback, (PFN_vkVoidFunction)CmdBeginTransformFeedbackEXT);
    if (!strcmp(name, "vkCmdDrawIndirectByteCountEXT"))
        return xfbProc(handle, name, &nextDrawIndirectByteCount, (PFN_vkVoidFunction)CmdDrawIndirectByteCountEXT);
    if (!strcmp(name, "vkCmdPipelineBarrier2"))
        return pbModeNow() != PB_OFF ? xfbProc(handle, name, &nextPipelineBarrier2, (PFN_vkVoidFunction)CmdPipelineBarrier2) : NULL;
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR"))
        return pbModeNow() != PB_OFF ? xfbProc(handle, name, &nextPipelineBarrier2KHR, (PFN_vkVoidFunction)CmdPipelineBarrier2KHR) : NULL;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)CreateGraphicsPipelines;
    if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)CreateComputePipelines;
    if (!strcmp(name, "vkCreateRayTracingPipelinesKHR") || !strcmp(name, "vkGetShaderModuleCreateInfoIdentifierEXT"))
        return driverProc(handle, name);
    if (!strcmp(name, "vkGetImageSubresourceLayout")) return (PFN_vkVoidFunction)GetImageSubresourceLayout;
    if (!strncmp(name, "vkGet", 5) && strstr(name, "SubresourceLayout")) return layoutProc(handle, name);
    if (!strcmp(name, "vkResetCommandPool")) return (PFN_vkVoidFunction)ResetCommandPool;
    if (!strcmp(name, "vkResetCommandBuffer")) return (PFN_vkVoidFunction)ResetCommandBuffer;
    /* guards.c's recordings and crash guard need these also when the release is at a fixed interval */
    if ((!strcmp(name, "vkBeginCommandBuffer") && guards_query_visible_on()) || (!strcmp(name, "vkEndCommandBuffer") && crash_guard_on()))
        return trackedProc(handle, name);
    if (releaseEvery() < 0 && (!strncmp(name, "vkCmd", 5) || strstr(name, "Command"))) return trackedProc(handle, name);
    return NULL;
}
