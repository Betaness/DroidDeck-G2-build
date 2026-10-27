/*
 * The Mali workarounds of the Vortek client (mali-g2/vortek/src/droiddeck_maint.c) that are about the
 * driver rather than Vortek's wire protocol; with the driver in this process, maintenance5/6,
 * graphics pipeline libraries and the extension filter (ext_commands.h) need nothing.
 *   - ClipDistance / CullDistance stores are removed from shaders: the Mali shader compiler crashes
 *     on them (spirv_clip_cull.h). VORTEK_KEEP_CLIP_DISTANCE=1 keeps them.
 *   - vkd3d-proton is told storage texel buffers need single-texel alignment only
 *     (VORTEK_STRICT_TEXEL_ALIGNMENT=1 reports the driver's value).
 *   - VK_EXT_vertex_attribute_divisor stands in for the driver's KHR extension (icd.c); its
 *     properties structure is filled from the KHR one here.
 *   - Core features DXVK requires that the Mali lacks are reported, as the Vortek server reports
 *     them (vortekrenderer's checkDeviceFeatures), and turned off again at vkCreateDevice (icd.c):
 *     fillModeNonSolid (what line or point fill then draws is up to the driver, as through Vortek),
 *     and shaderClipDistance / shaderCullDistance while their writes are removed from shaders.
 *     Without them DXVK finds no adapter ("Device does not support required feature
 *     'fillModeNonSolid'").
 *   - Command pool and command buffer resets add RELEASE_RESOURCES as the Vortek server's do
 *     (winlator-0018): the Mali keeps ~420 bytes of host heap per vkCmdBeginRendering in a command
 *     buffer that also writes a query until it is reset with RELEASE_RESOURCES, and DXVK resets its
 *     pools with flags 0, so the game process grows by tens of MB a minute
 *     (vortek/tests/frame_leak_test.c). By default a reset releases when the estimate of what the
 *     driver keeps (vortek/src/cmd_memory.h, from the vkCmdBeginRendering and query commands
 *     recorded here) reaches DROIDDECK_HYBRIS_CMD_RELEASE_BYTES (else VORTEK_CMD_RELEASE_BYTES,
 *     default 16m) for its pool, or *_CMD_RELEASE_TOTAL_BYTES (default 2 x that) over all pools;
 *     vkBeginCommandBuffer of a command buffer due for release resets it with RELEASE_RESOURCES
 *     first when its pool allows single resets. *_CMD_RELEASE_LOG=1 logs every release. With
 *     DROIDDECK_HYBRIS_CMD_RELEASE_EVERY (else VORTEK_CMD_RELEASE_EVERY) set, every Nth reset of the
 *     same pool or command buffer releases instead (1 = every reset, 0 = never) and nothing is
 *     counted: only the two reset calls are wrapped then (a value that is not a number is ignored).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "icd.h"

#include <stdlib.h>
#include <string.h>

#include "spirv_clip_cull.h"
#define CMDMEM_LOG(fmt, ...) fprintf(stderr, "hybris-icd: " fmt "\n", ##__VA_ARGS__)
#include "cmd_memory.h"

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

/* A rewritten copy of `info` in *out (code to free in *freeCode), or false to use `info`. */
static bool stripClipCull(const VkShaderModuleCreateInfo* info, VkShaderModuleCreateInfo* out, uint32_t** freeCode) {
    if (!clipStripOn() || !info->pCode || info->codeSize < 20) return false;
    size_t words = 0;
    uint32_t* code = dd_strip_clip_cull(info->pCode, info->codeSize / 4, &words);
    if (!code) return false;
    static int logged;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: clip/cull distance writes removed from a shader (the Mali compiler crashes on them)\n");
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
    VkShaderModuleCreateInfo stripped;
    uint32_t* code = NULL;
    VkResult result = device->CreateShaderModule(handle, stripClipCull(info, &stripped, &code) ? &stripped : info, allocator, module);
    free(code);
    return result;
}

/* Inline stage code (DXVK 3.x, pipeline libraries): a stage whose pNext chain starts with its
 * VkShaderModuleCreateInfo is copied with the rewritten code. */
static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                   const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                                   VkPipeline* pipelines) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (!clipStripOn()) return device->CreateGraphicsPipelines(handle, cache, count, infos, allocator, pipelines);
    uint32_t totalStages = 0;
    bool inline_ = false;
    for (uint32_t i = 0; i < count; i++) {
        totalStages += infos[i].stageCount;
        for (uint32_t s = 0; s < infos[i].stageCount; s++) {
            const VkBaseInStructure* next = infos[i].pStages[s].pNext;
            if (next && next->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) inline_ = true;
        }
    }
    if (!inline_) return device->CreateGraphicsPipelines(handle, cache, count, infos, allocator, pipelines);
    VkGraphicsPipelineCreateInfo* copy = malloc(count * sizeof(*copy));
    VkPipelineShaderStageCreateInfo* stages = calloc(totalStages, sizeof(*stages));
    VkShaderModuleCreateInfo* modules = calloc(totalStages, sizeof(*modules));
    uint32_t** codes = calloc(totalStages, sizeof(*codes));
    uint32_t used = 0;
    bool any = false;
    if (!copy || !stages || !modules || !codes) goto out;
    memcpy(copy, infos, count * sizeof(*copy));
    for (uint32_t i = 0; i < count; i++) {
        bool rewritten = false;
        uint32_t base = used;
        for (uint32_t s = 0; s < infos[i].stageCount; s++, used++) {
            stages[used] = infos[i].pStages[s];
            const VkShaderModuleCreateInfo* inl = infos[i].pStages[s].pNext;
            if (inl && inl->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && stripClipCull(inl, &modules[used], &codes[used])) {
                stages[used].pNext = &modules[used];
                rewritten = true;
            }
        }
        if (rewritten) {
            copy[i].pStages = &stages[base];
            any = true;
        }
    }
out:;
    VkResult result = device->CreateGraphicsPipelines(handle, cache, count, any ? copy : infos, allocator, pipelines);
    for (uint32_t i = 0; codes && i < used; i++) free(codes[i]);
    free(codes);
    free(modules);
    free(stages);
    free(copy);
    return result;
}

/* *_CMD_RELEASE_*: the DROIDDECK_HYBRIS_ variable, else the VORTEK_ one (droiddeck-env sets those for
 * the server). */
static const char* releaseEnv(const char* suffix, const char** used) {
    static char name[64];
    snprintf(name, sizeof(name), "DROIDDECK_HYBRIS_CMD_RELEASE_%s", suffix);
    const char* text = getenv(name);
    if (!text || !*text) {
        snprintf(name, sizeof(name), "VORTEK_CMD_RELEASE_%s", suffix);
        text = getenv(name);
    }
    if (used) *used = name;
    return text && *text ? text : NULL;
}

/* The fixed release interval (-1: by the estimate) and, without one, the estimate. Read once, before
 * the first device's procs are handed out. */
static int releaseEvery_ = -2;
static CmdMem* commandMemory;
static pthread_once_t releaseOnce = PTHREAD_ONCE_INIT;

static void readReleaseConfig(void) {
    const char* name;
    const char* text = releaseEnv("EVERY", &name);
    int every = cmdmem_parse_every(text);
    if (every >= 0) {
        releaseEvery_ = every;
        fprintf(stderr, "hybris-icd: %s=%d\n", name, releaseEvery_);
        return;
    }
    if (text) fprintf(stderr, "hybris-icd: %s=%s is not a number, releasing by the estimate\n", name, text);
    uint64_t perPool = cmdmem_parse_bytes(releaseEnv("BYTES", NULL), CMDMEM_DEFAULT_PER_POOL);
    uint64_t total = cmdmem_parse_bytes(releaseEnv("TOTAL_BYTES", NULL), CMDMEM_DEFAULT_TOTAL(perPool));
    const char* log = releaseEnv("LOG", NULL);
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

void mali_device_created(Device* device) {
    releaseEvery(); /* before the device's first pool */
    keepNext(&nextResetCommandBuffer, (PFN_vkVoidFunction)device->ResetCommandBuffer);
}

void mali_device_destroyed(Device* device) {
    cmdmem_device_destroyed(commandMemory, (uint64_t)(uintptr_t)device->handle);
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
    return NEXT(T_BEGIN, PFN_vkBeginCommandBuffer)(commandBuffer, info);
}

static VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    VkResult result = NEXT(T_END, PFN_vkEndCommandBuffer)(commandBuffer);
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
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)CreateGraphicsPipelines;
    if (!strcmp(name, "vkResetCommandPool")) return (PFN_vkVoidFunction)ResetCommandPool;
    if (!strcmp(name, "vkResetCommandBuffer")) return (PFN_vkVoidFunction)ResetCommandBuffer;
    if (releaseEvery() < 0 && (!strncmp(name, "vkCmd", 5) || strstr(name, "Command"))) return trackedProc(handle, name);
    return NULL;
}
