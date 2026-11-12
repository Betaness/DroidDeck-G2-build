/*
 * [mali-g2] DROIDDECK_HYBRIS_PASS_BARRIER: the barriers DXVK records at the end of a render pass,
 * counted and optionally narrowed (mali_fixes.c wraps vkCmdPipelineBarrier2 and ...2KHR for it; DXVK
 * calls only vkCmdPipelineBarrier2). The rules, the copy of a VkDependencyInfo and the SPIR-V scan of
 * the guard; no state, so tests/pass_barrier_test.c runs them on the host.
 *
 * After a synchronous render pass DXVK records one vkCmdPipelineBarrier2 with
 *   - a global VkMemoryBarrier2 with both access masks 0 (it is there against write-after-read only)
 *     whose destination is every shader stage, transfer, colour output and both fragment tests, plus
 *     transform feedback when the device has it (0x01001ff8 on the Mali), and
 *   - per attachment a VkImageMemoryBarrier2 with the layout unchanged (GENERAL with unified image
 *     layouts), attachment writes as its only source access and the image's stages as destination
 *     (every shader stage for a render target that is also sampled: 0x1cf8).
 * Both make the vertex work after them wait for the fragment work before them. Modes (only the
 * destination stage mask changes; access masks, layouts, queue families, flags and pNext stay):
 *   log        nothing changes, the barriers are only counted (PbCounts).
 *   narrow     global barriers (no pNext, access masks 0, destination including CS, VS, FS, transfer,
 *              colour output and both fragment tests) lose VS, TCS, TES, GS and transform feedback,
 *              unless shader code declaring transform feedback or a pipeline with it was seen, or a
 *              device enabled vertexPipelineStoresAndAtomics. Without that feature the vertex
 *              pipeline does not write memory, so it has no write-after-read hazard to wait for.
 *   narrow-rt  narrow, and attachment release barriers (no pNext, layout and queue family
 *              unchanged, source access attachment writes only, source stage colour output or a
 *              fragment test, destination with a vertex pipeline stage) lose VS, TCS, TES and GS,
 *              unless vertex, tessellation or geometry shader code reading an image that is not a
 *              texel buffer was seen (pb_spirv_geo_image_reads), or vertexPipelineStoresAndAtomics
 *              was enabled.
 *   nogeo      every barrier with a fragment stage in its source loses VS, TCS, TES, GS and transform
 *              feedback, without guards: an upper bound of what narrowing gives; it can break rendering.
 * A destination stays as it is when dropping would leave it empty or no longer covering its access
 * mask (shader access without a shader stage left, access types not listed here); transform feedback
 * access keeps the transform feedback stage.
 */
#ifndef DROIDDECK_PASS_BARRIER_H
#define DROIDDECK_PASS_BARRIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#define PB_GEO                                                                                                  \
    (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |              \
     VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT)
#define PB_XFB VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT
#define PB_FRAGMENT                                                                                             \
    (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |                   \
     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
#define PB_PASS_DST                                                                                             \
    (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | PB_FRAGMENT |             \
     VK_PIPELINE_STAGE_2_TRANSFER_BIT)
#define PB_ATTACHMENT_SRC                                                                                       \
    (VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |           \
     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
#define PB_ATTACHMENT_WRITES (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
#define PB_SHADER_STAGES                                                                                        \
    (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |                         \
     VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |                        \
     VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR)
/* access types no stage in PB_GEO or PB_XFB is needed for */
#define PB_ACCESS_ELSEWHERE                                                                                     \
    (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | \
     VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | PB_ATTACHMENT_WRITES |      \
     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |                            \
     VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT |                  \
     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)
#define PB_ACCESS_SHADER                                                                                        \
    (VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |                \
     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
#define PB_ACCESS_XFB                                                                                           \
    (VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |       \
     VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT)

typedef enum { PB_OFF, PB_LOG, PB_NARROW, PB_NARROW_RT, PB_NOGEO } PbMode;
static const char* const pb_mode_names[] = {"off", "log", "narrow", "narrow-rt", "nogeo"};

/* DROIDDECK_HYBRIS_PASS_BARRIER's value; *unknown for a value that is not a mode (off then). */
static inline PbMode pb_parse_mode(const char* text, bool* unknown) {
    *unknown = false;
    if (!text || !*text || !strcmp(text, "0")) return PB_OFF;
    for (int m = PB_OFF; m <= PB_NOGEO; m++)
        if (!strcmp(text, pb_mode_names[m])) return (PbMode)m;
    *unknown = true;
    return PB_OFF;
}

typedef struct {
    bool xfb;           /* transform feedback code or pipelines seen */
    bool vertexStores;  /* a device enabled vertexPipelineStoresAndAtomics */
    bool geoImageReads; /* vertex, tessellation or geometry shader code reading images seen */
} PbGuards;

enum { PB_GLOBAL = 1, PB_GLOBAL_WRITES = 2, PB_ATTACHMENT = 4, PB_FRAG_TO_GEO = 8 };

static inline unsigned pb_frag_to_geo(VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst) {
    return (src & PB_FRAGMENT) && (dst & (PB_GEO | PB_XFB)) ? PB_FRAG_TO_GEO : 0;
}

/* PB_GLOBAL: DXVK's global barrier's shape, PB_GLOBAL_WRITES too when it has a source access (passes
 * with UAV or stream output writes). */
static inline unsigned pb_memory_class(const VkMemoryBarrier2* b) {
    unsigned c = pb_frag_to_geo(b->srcStageMask, b->dstStageMask);
    if (!b->pNext && !b->dstAccessMask && (b->dstStageMask & PB_PASS_DST) == PB_PASS_DST)
        c |= PB_GLOBAL | (b->srcAccessMask ? PB_GLOBAL_WRITES : 0);
    return c;
}

static inline unsigned pb_buffer_class(const VkBufferMemoryBarrier2* b) {
    return pb_frag_to_geo(b->srcStageMask, b->dstStageMask);
}

/* PB_ATTACHMENT: an attachment release barrier's shape with a vertex pipeline stage to drop. */
static inline unsigned pb_image_class(const VkImageMemoryBarrier2* b) {
    unsigned c = pb_frag_to_geo(b->srcStageMask, b->dstStageMask);
    if (!b->pNext && b->oldLayout == b->newLayout && b->srcQueueFamilyIndex == b->dstQueueFamilyIndex &&
        !(b->srcAccessMask & ~(VkAccessFlags2)PB_ATTACHMENT_WRITES) && (b->srcStageMask & PB_ATTACHMENT_SRC) &&
        (b->dstStageMask & PB_GEO))
        c |= PB_ATTACHMENT;
    return c;
}

/* `dst` without `drop`, or `dst` when that leaves nothing or stages that no longer cover `access`. */
static inline VkPipelineStageFlags2 pb_drop(VkPipelineStageFlags2 dst, VkAccessFlags2 access, VkPipelineStageFlags2 drop) {
    if (access & ~(VkAccessFlags2)(PB_ACCESS_ELSEWHERE | PB_ACCESS_SHADER | PB_ACCESS_XFB)) return dst;
    if (access & PB_ACCESS_XFB) drop &= ~(VkPipelineStageFlags2)PB_XFB;
    VkPipelineStageFlags2 narrowed = dst & ~drop;
    if (!narrowed || ((access & PB_ACCESS_SHADER) && !(narrowed & PB_SHADER_STAGES))) return dst;
    return narrowed;
}

/* The destination stage mask to record (`cls` from the class function). */
static inline VkPipelineStageFlags2 pb_memory_dst(PbMode mode, const PbGuards* g, const VkMemoryBarrier2* b, unsigned cls) {
    if (mode == PB_NOGEO && (cls & PB_FRAG_TO_GEO)) return pb_drop(b->dstStageMask, b->dstAccessMask, PB_GEO | PB_XFB);
    if ((mode == PB_NARROW || mode == PB_NARROW_RT) && (cls & (PB_GLOBAL | PB_GLOBAL_WRITES)) == PB_GLOBAL && !g->xfb && !g->vertexStores)
        return pb_drop(b->dstStageMask, b->dstAccessMask, PB_GEO | PB_XFB);
    return b->dstStageMask;
}

static inline VkPipelineStageFlags2 pb_buffer_dst(PbMode mode, const PbGuards* g, const VkBufferMemoryBarrier2* b, unsigned cls) {
    (void)g;
    if (mode == PB_NOGEO && (cls & PB_FRAG_TO_GEO)) return pb_drop(b->dstStageMask, b->dstAccessMask, PB_GEO | PB_XFB);
    return b->dstStageMask;
}

static inline VkPipelineStageFlags2 pb_image_dst(PbMode mode, const PbGuards* g, const VkImageMemoryBarrier2* b, unsigned cls) {
    if (mode == PB_NOGEO && (cls & PB_FRAG_TO_GEO)) return pb_drop(b->dstStageMask, b->dstAccessMask, PB_GEO | PB_XFB);
    if (mode == PB_NARROW_RT && (cls & PB_ATTACHMENT) && !g->geoImageReads && !g->vertexStores)
        return pb_drop(b->dstStageMask, b->dstAccessMask, PB_GEO);
    return b->dstStageMask;
}

/* What one or more calls saw. The masks are the last ones seen of the class (0: none). */
typedef struct {
    uint64_t calls, global, globalWrites, attachment, fragToGeo, narrowed;
    VkPipelineStageFlags2 globalDst, attachmentDst;
} PbCounts;

#define PB_STACK_MEMORY 4
#define PB_STACK_BUFFERS 8
#define PB_STACK_IMAGES 16

/* The arrays of a rewritten VkDependencyInfo: on the stack up to the sizes above, else in `heap`. */
typedef struct {
    VkDependencyInfo info;
    VkMemoryBarrier2 memory[PB_STACK_MEMORY];
    VkBufferMemoryBarrier2 buffers[PB_STACK_BUFFERS];
    VkImageMemoryBarrier2 images[PB_STACK_IMAGES];
    void* heap; /* free() after the call */
} PbCopy;

#define PB_NONE UINT32_MAX

/* The dependency info to record for `info`: `info` itself when no barrier changes (or the copy cannot
 * be allocated), else c->info, a copy whose changed arrays are copies too. Adds the call to `n`. */
static inline const VkDependencyInfo* pb_rewrite(PbMode mode, const PbGuards* g, const VkDependencyInfo* info, PbCopy* c, PbCounts* n) {
    c->heap = NULL;
    n->calls++;
    if (!info) return info;
    uint32_t firstMemory = PB_NONE, firstBuffer = PB_NONE, firstImage = PB_NONE;
    for (uint32_t i = 0; info->pMemoryBarriers && i < info->memoryBarrierCount; i++) {
        const VkMemoryBarrier2* b = &info->pMemoryBarriers[i];
        unsigned cls = pb_memory_class(b);
        if (cls & PB_GLOBAL) {
            n->global++;
            n->globalDst = b->dstStageMask;
        }
        if (cls & PB_GLOBAL_WRITES) n->globalWrites++;
        if (cls & PB_FRAG_TO_GEO) n->fragToGeo++;
        if (firstMemory == PB_NONE && pb_memory_dst(mode, g, b, cls) != b->dstStageMask) firstMemory = i;
    }
    for (uint32_t i = 0; info->pBufferMemoryBarriers && i < info->bufferMemoryBarrierCount; i++) {
        const VkBufferMemoryBarrier2* b = &info->pBufferMemoryBarriers[i];
        unsigned cls = pb_buffer_class(b);
        if (cls & PB_FRAG_TO_GEO) n->fragToGeo++;
        if (firstBuffer == PB_NONE && pb_buffer_dst(mode, g, b, cls) != b->dstStageMask) firstBuffer = i;
    }
    for (uint32_t i = 0; info->pImageMemoryBarriers && i < info->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2* b = &info->pImageMemoryBarriers[i];
        unsigned cls = pb_image_class(b);
        if (cls & PB_ATTACHMENT) {
            n->attachment++;
            n->attachmentDst = b->dstStageMask;
        }
        if (cls & PB_FRAG_TO_GEO) n->fragToGeo++;
        if (firstImage == PB_NONE && pb_image_dst(mode, g, b, cls) != b->dstStageMask) firstImage = i;
    }
    if (firstMemory == PB_NONE && firstBuffer == PB_NONE && firstImage == PB_NONE) return info;

    size_t memoryHeap = firstMemory != PB_NONE && info->memoryBarrierCount > PB_STACK_MEMORY ? info->memoryBarrierCount : 0;
    size_t bufferHeap = firstBuffer != PB_NONE && info->bufferMemoryBarrierCount > PB_STACK_BUFFERS ? info->bufferMemoryBarrierCount : 0;
    size_t imageHeap = firstImage != PB_NONE && info->imageMemoryBarrierCount > PB_STACK_IMAGES ? info->imageMemoryBarrierCount : 0;
    size_t heapBytes = memoryHeap * sizeof(VkMemoryBarrier2) + bufferHeap * sizeof(VkBufferMemoryBarrier2) +
                       imageHeap * sizeof(VkImageMemoryBarrier2);
    char* heap = heapBytes ? malloc(heapBytes) : NULL;
    if (heapBytes && !heap) return info;
    c->heap = heap;
    c->info = *info;
    if (firstMemory != PB_NONE) {
        VkMemoryBarrier2* copy = memoryHeap ? (VkMemoryBarrier2*)heap : c->memory;
        heap += memoryHeap * sizeof(VkMemoryBarrier2);
        memcpy(copy, info->pMemoryBarriers, info->memoryBarrierCount * sizeof(*copy));
        for (uint32_t i = firstMemory; i < info->memoryBarrierCount; i++) {
            VkPipelineStageFlags2 dst = pb_memory_dst(mode, g, &copy[i], pb_memory_class(&copy[i]));
            n->narrowed += dst != copy[i].dstStageMask;
            copy[i].dstStageMask = dst;
        }
        c->info.pMemoryBarriers = copy;
    }
    if (firstBuffer != PB_NONE) {
        VkBufferMemoryBarrier2* copy = bufferHeap ? (VkBufferMemoryBarrier2*)heap : c->buffers;
        heap += bufferHeap * sizeof(VkBufferMemoryBarrier2);
        memcpy(copy, info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount * sizeof(*copy));
        for (uint32_t i = firstBuffer; i < info->bufferMemoryBarrierCount; i++) {
            VkPipelineStageFlags2 dst = pb_buffer_dst(mode, g, &copy[i], pb_buffer_class(&copy[i]));
            n->narrowed += dst != copy[i].dstStageMask;
            copy[i].dstStageMask = dst;
        }
        c->info.pBufferMemoryBarriers = copy;
    }
    if (firstImage != PB_NONE) {
        VkImageMemoryBarrier2* copy = imageHeap ? (VkImageMemoryBarrier2*)heap : c->images;
        memcpy(copy, info->pImageMemoryBarriers, info->imageMemoryBarrierCount * sizeof(*copy));
        for (uint32_t i = firstImage; i < info->imageMemoryBarrierCount; i++) {
            VkPipelineStageFlags2 dst = pb_image_dst(mode, g, &copy[i], pb_image_class(&copy[i]));
            n->narrowed += dst != copy[i].dstStageMask;
            copy[i].dstStageMask = dst;
        }
        c->info.pImageMemoryBarriers = copy;
    }
    return &c->info;
}

/* ---- the guard of narrow-rt: SPIR-V of the vertex pipeline that reads images ---- */

static inline bool pb_image_read_op(uint32_t op) {
    return op == 60 /* OpImageTexelPointer */ || (op >= 87 && op <= 98) /* OpImageSample*, Fetch, Gather, DrefGather, Read */ ||
           (op >= 305 && op <= 315) /* OpImageSparseSample*, Fetch, Gather, DrefGather */ || op == 320 /* OpImageSparseRead */;
}

/* Instructions <result type> <result id> ... an image operand can come from. */
static inline bool pb_typed_value_op(uint32_t op) {
    switch (op) {
    case 1:   /* OpUndef */
    case 55:  /* OpFunctionParameter */
    case 57:  /* OpFunctionCall */
    case 59:  /* OpVariable */
    case 61:  /* OpLoad */
    case 65:  /* OpAccessChain */
    case 66:  /* OpInBoundsAccessChain */
    case 67:  /* OpPtrAccessChain */
    case 70:  /* OpInBoundsPtrAccessChain */
    case 81:  /* OpCompositeExtract */
    case 83:  /* OpCopyObject */
    case 86:  /* OpSampledImage */
    case 100: /* OpImage */
    case 169: /* OpSelect */
    case 245: /* OpPhi */
    case 400: /* OpCopyLogical */
        return true;
    }
    return false;
}

enum { PB_KIND_TEXEL_BUFFER = 1, PB_KIND_IMAGE = 2 };

/* Whether SPIR-V code has a Vertex, TessellationControl, TessellationEvaluation or Geometry entry
 * point and reads texels of an image (the opcodes of pb_image_read_op) whose type is not Dim Buffer:
 * texel buffers are no images to image barriers. An image operand whose type cannot be followed, or
 * code that cannot be parsed after such an entry point, counts as a read. */
static inline bool pb_spirv_geo_image_reads(const uint32_t* code, size_t words) {
    if (!code || words < 5 || code[0] != 0x07230203u) return false;
    bool geo = false, reads = false;
    for (size_t i = 5; i < words;) {
        uint32_t op = code[i] & 0xffffu, n = code[i] >> 16;
        if (!n || n > words - i) return geo;
        if (op == 15 /* OpEntryPoint */ && n >= 2 && code[i + 1] <= 3) geo = true;
        if (pb_image_read_op(op)) reads = true;
        i += n;
    }
    if (!geo || !reads) return false;
    uint32_t bound = code[3];
    if (!bound || bound > (1u << 24)) return true;
    uint32_t* ref = calloc(bound, sizeof(uint32_t) + 1); /* id -> its type, or the type a type refers to */
    if (!ref) return true;
    uint8_t* kind = (uint8_t*)(ref + bound);
    for (size_t i = 5; i < words; i += code[i] >> 16) {
        uint32_t op = code[i] & 0xffffu, n = code[i] >> 16;
        if (op == 25 /* OpTypeImage */ && n >= 9 && code[i + 1] < bound)
            kind[code[i + 1]] = code[i + 3] == 5 /* Dim Buffer */ ? PB_KIND_TEXEL_BUFFER : PB_KIND_IMAGE;
        else if (op == 27 /* OpTypeSampledImage */ && n >= 3 && code[i + 1] < bound)
            ref[code[i + 1]] = code[i + 2];
        else if (op == 32 /* OpTypePointer */ && n >= 4 && code[i + 1] < bound)
            ref[code[i + 1]] = code[i + 3];
        else if (pb_typed_value_op(op) && n >= 3 && code[i + 2] < bound)
            ref[code[i + 2]] = code[i + 1];
    }
    bool found = false;
    for (size_t i = 5; i < words && !found; i += code[i] >> 16) {
        uint32_t op = code[i] & 0xffffu, n = code[i] >> 16;
        if (!pb_image_read_op(op)) continue;
        uint32_t id = n >= 4 ? code[i + 3] : 0, k = 0;
        for (int hop = 0; hop < 4 && id && id < bound && !k; hop++) {
            k = kind[id];
            id = ref[id];
        }
        found = k != PB_KIND_TEXEL_BUFFER;
    }
    free(ref);
    return found;
}

#endif
