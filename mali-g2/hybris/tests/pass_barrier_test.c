/* [mali-g2] Unit test of ../src/pass_barrier.h (DROIDDECK_HYBRIS_PASS_BARRIER), no GPU:
 *   1 the mode names;
 *   2 the rules on the barriers DXVK records after a render pass (the masks it uses on the Mali), and
 *     on barriers each rule must leave alone: guards, access masks, layouts, queue families, pNext,
 *     destinations that would be empty or no longer cover their access;
 *   3 pb_rewrite: the application's arrays are never written, only the arrays with a change are
 *     copied (on the stack, or on the heap when long), everything but dstStageMask is copied as is,
 *     the counts;
 *   4 pb_spirv_geo_scan on glslang output (pass_barrier_gen.sh -> pass_barrier_spv.h) and on
 *     hand-made code (operands and pointers that cannot be followed, damaged and byte-swapped code,
 *     texel pointers, the storage classes written through).
 *   cc -O2 -Wall -Wextra -I../src -o pass_barrier_test pass_barrier_test.c && ./pass_barrier_test
 * (CI also runs it built by clang with -fsanitize=undefined,pointer-overflow.)
 * Prints PASS-BARRIER-PASS or the failed checks. */
#include <stdarg.h>
#include <stdio.h>

#include "pass_barrier.h"
#include "pass_barrier_spv.h"

static int failures;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #c);             \
            failures++;                                             \
        }                                                           \
    } while (0)

#define VS VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
#define FS VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define GS VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT
#define COLOR_OUT VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
#define EARLY VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
#define LATE VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
#define TRANSFER VK_PIPELINE_STAGE_2_TRANSFER_BIT
#define VERTEX_INPUT VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
#define ALL_SHADERS (VS | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | GS | FS | CS)

/* DXVK's barriers on the Mali: the global one's destination, a sampled render target's release,
 * a sampled depth buffer's release (GENERAL: the source has every shader stage). */
#define DXVK_GLOBAL_DST 0x01001ff8ull
#define DXVK_RT_DST 0x1cf8ull
#define DXVK_RT_ACCESS (VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | \
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)

static VkMemoryBarrier2 globalBarrier(void) {
    return (VkMemoryBarrier2){VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VERTEX_INPUT | VS | FS, 0, DXVK_GLOBAL_DST, 0};
}

static VkImageMemoryBarrier2 colorRelease(void) {
    return (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = COLOR_OUT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, .dstStageMask = DXVK_RT_DST, .dstAccessMask = DXVK_RT_ACCESS,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = (VkImage)(uintptr_t)0x1000, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}

static VkImageMemoryBarrier2 depthRelease(void) {
    VkImageMemoryBarrier2 b = colorRelease();
    b.srcStageMask = EARLY | LATE | ALL_SHADERS;
    b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = TRANSFER | ALL_SHADERS | EARLY | LATE;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.image = (VkImage)(uintptr_t)0x2000;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    return b;
}

static VkPipelineStageFlags2 memoryDst(PbMode mode, PbGuards g, VkMemoryBarrier2 b) {
    return pb_memory_dst(mode, &g, &b, pb_memory_class(&b));
}

static VkPipelineStageFlags2 imageDst(PbMode mode, PbGuards g, VkImageMemoryBarrier2 b) {
    return pb_image_dst(mode, &g, &b, pb_image_class(&b));
}

static VkPipelineStageFlags2 bufferDst(PbMode mode, PbGuards g, VkBufferMemoryBarrier2 b) {
    return pb_buffer_dst(mode, &g, &b, pb_buffer_class(&b));
}

static void testModes(void) {
    bool unknown;
    CHECK(pb_parse_mode(NULL, &unknown) == PB_OFF && !unknown);
    CHECK(pb_parse_mode("", &unknown) == PB_OFF && !unknown);
    CHECK(pb_parse_mode("0", &unknown) == PB_OFF && !unknown);
    CHECK(pb_parse_mode("off", &unknown) == PB_OFF && !unknown);
    CHECK(pb_parse_mode("log", &unknown) == PB_LOG && !unknown);
    CHECK(pb_parse_mode("narrow", &unknown) == PB_NARROW && !unknown);
    CHECK(pb_parse_mode("narrow-rt", &unknown) == PB_NARROW_RT && !unknown);
    CHECK(pb_parse_mode("nogeo", &unknown) == PB_NOGEO && !unknown);
    CHECK(pb_parse_mode("narrow_rt", &unknown) == PB_OFF && unknown);
    CHECK(pb_parse_mode("1", &unknown) == PB_OFF && unknown);
}

static void testRules(void) {
    const PbGuards none = {0}, xfb = {.xfb = true}, stores = {.vertexStores = true}, reads = {.geoImageReads = true};
    const VkPipelineStageFlags2 narrowedGlobal = FS | EARLY | LATE | COLOR_OUT | CS | TRANSFER;
    CHECK(narrowedGlobal == 0x1f80);

    /* the global barrier */
    VkMemoryBarrier2 g = globalBarrier();
    CHECK(pb_memory_class(&g) == (PB_GLOBAL | PB_FRAG_TO_GEO));
    CHECK(memoryDst(PB_OFF, none, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_LOG, none, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NARROW, none, g) == narrowedGlobal);
    CHECK(memoryDst(PB_NARROW_RT, none, g) == narrowedGlobal);
    CHECK(memoryDst(PB_NOGEO, none, g) == narrowedGlobal);
    CHECK(memoryDst(PB_NARROW, xfb, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NARROW, stores, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NARROW, reads, g) == narrowedGlobal); /* that guard is the image barriers' */
    CHECK(memoryDst(PB_NOGEO, xfb, g) == narrowedGlobal);    /* no guards */
    /* without transform feedback on the device */
    g.dstStageMask = DXVK_GLOBAL_DST & ~(VkPipelineStageFlags2)PB_XFB;
    CHECK(memoryDst(PB_NARROW, none, g) == narrowedGlobal);
    /* a pass with UAV writes: the source access stays, so does the barrier */
    g = globalBarrier();
    g.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    CHECK(pb_memory_class(&g) == (PB_GLOBAL | PB_GLOBAL_WRITES | PB_FRAG_TO_GEO));
    CHECK(memoryDst(PB_NARROW, none, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NARROW_RT, none, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NOGEO, none, g) == narrowedGlobal);
    /* a destination access, a missing destination stage of DXVK's set, a pNext: not the global barrier */
    g = globalBarrier();
    g.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    CHECK(!(pb_memory_class(&g) & PB_GLOBAL) && memoryDst(PB_NARROW, none, g) == DXVK_GLOBAL_DST);
    g = globalBarrier();
    g.dstStageMask &= ~(VkPipelineStageFlags2)TRANSFER;
    CHECK(!(pb_memory_class(&g) & PB_GLOBAL) && memoryDst(PB_NARROW, none, g) == g.dstStageMask);
    g = globalBarrier();
    VkMemoryBarrier2 chained = globalBarrier();
    g.pNext = &chained;
    CHECK(!(pb_memory_class(&g) & PB_GLOBAL) && memoryDst(PB_NARROW, none, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NOGEO, none, g) == DXVK_GLOBAL_DST); /* nogeo leaves a pNext too */
    /* meta stages: matched only by their own bits, so not DXVK's shape */
    g = globalBarrier();
    g.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    CHECK(pb_memory_class(&g) == 0 && memoryDst(PB_NARROW, none, g) == g.dstStageMask && memoryDst(PB_NOGEO, none, g) == g.dstStageMask);
    /* not from a fragment stage: nogeo leaves it */
    g = globalBarrier();
    g.srcStageMask = TRANSFER;
    CHECK(pb_memory_class(&g) == PB_GLOBAL && memoryDst(PB_NOGEO, none, g) == DXVK_GLOBAL_DST);
    CHECK(memoryDst(PB_NARROW, none, g) == narrowedGlobal);

    /* attachment releases: a sampled render target, a sampled depth buffer */
    VkImageMemoryBarrier2 c = colorRelease();
    CHECK(pb_image_class(&c) == (PB_ATTACHMENT | PB_FRAG_TO_GEO));
    CHECK(imageDst(PB_LOG, none, c) == DXVK_RT_DST);
    CHECK(imageDst(PB_NARROW, none, c) == DXVK_RT_DST);
    CHECK(imageDst(PB_NARROW_RT, none, c) == (TRANSFER | CS | FS | COLOR_OUT));
    CHECK(imageDst(PB_NARROW_RT, none, c) == 0x1c80);
    CHECK(imageDst(PB_NARROW_RT, reads, c) == DXVK_RT_DST);
    CHECK(imageDst(PB_NARROW_RT, stores, c) == DXVK_RT_DST);
    CHECK(imageDst(PB_NARROW_RT, xfb, c) == 0x1c80);
    CHECK(imageDst(PB_NOGEO, reads, c) == 0x1c80);
    VkImageMemoryBarrier2 d = depthRelease();
    CHECK(pb_image_class(&d) == (PB_ATTACHMENT | PB_FRAG_TO_GEO));
    CHECK(imageDst(PB_NARROW_RT, none, d) == (TRANSFER | FS | CS | EARLY | LATE));
    /* a read-only depth buffer: no source access */
    d.srcAccessMask = 0;
    CHECK(imageDst(PB_NARROW_RT, none, d) == (TRANSFER | FS | CS | EARLY | LATE));
    /* layout transitions, queue family transfers, other source accesses or stages, a pNext: left */
    c = colorRelease();
    c.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CHECK(!(pb_image_class(&c) & PB_ATTACHMENT) && imageDst(PB_NARROW_RT, none, c) == DXVK_RT_DST);
    c = colorRelease();
    c.srcQueueFamilyIndex = 0;
    c.dstQueueFamilyIndex = 1;
    CHECK(!(pb_image_class(&c) & PB_ATTACHMENT) && imageDst(PB_NARROW_RT, none, c) == DXVK_RT_DST);
    c = colorRelease();
    c.srcAccessMask |= VK_ACCESS_2_SHADER_WRITE_BIT;
    CHECK(!(pb_image_class(&c) & PB_ATTACHMENT) && imageDst(PB_NARROW_RT, none, c) == DXVK_RT_DST);
    c = colorRelease();
    c.srcStageMask = TRANSFER;
    c.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    CHECK(pb_image_class(&c) == 0 && imageDst(PB_NARROW_RT, none, c) == DXVK_RT_DST && imageDst(PB_NOGEO, none, c) == DXVK_RT_DST);
    c = colorRelease();
    VkImageMemoryBarrier2 next = colorRelease();
    c.pNext = &next;
    CHECK(!(pb_image_class(&c) & PB_ATTACHMENT) && imageDst(PB_NARROW_RT, none, c) == DXVK_RT_DST);
    CHECK(imageDst(PB_NOGEO, none, c) == DXVK_RT_DST);
    /* nothing of the vertex pipeline to drop */
    c = colorRelease();
    c.dstStageMask = TRANSFER | FS;
    CHECK(pb_image_class(&c) == 0 && imageDst(PB_NARROW_RT, none, c) == (TRANSFER | FS));
    /* a shader read left without a shader stage, or nothing left: as it was */
    c = colorRelease();
    c.dstStageMask = VS | TRANSFER;
    c.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    CHECK(pb_image_class(&c) == (PB_ATTACHMENT | PB_FRAG_TO_GEO) && imageDst(PB_NARROW_RT, none, c) == (VS | TRANSFER));
    c.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    CHECK(imageDst(PB_NARROW_RT, none, c) == TRANSFER);
    c.dstStageMask = VS;
    c.dstAccessMask = 0;
    CHECK(imageDst(PB_NARROW_RT, none, c) == VS && imageDst(PB_NOGEO, none, c) == VS);

    /* nogeo on buffers; transform feedback access keeps its stage; unlisted access types keep all */
    VkBufferMemoryBarrier2 b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, NULL, FS, VK_ACCESS_2_SHADER_WRITE_BIT, VS | CS,
                                VK_ACCESS_2_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                (VkBuffer)(uintptr_t)0x3000, 0, VK_WHOLE_SIZE};
    CHECK(pb_buffer_class(&b) == PB_FRAG_TO_GEO);
    CHECK(bufferDst(PB_NARROW_RT, none, b) == (VS | CS));
    CHECK(bufferDst(PB_NOGEO, none, b) == CS);
    VkBufferMemoryBarrier2 chainedBuffer = b;
    b.pNext = &chainedBuffer;
    CHECK(bufferDst(PB_NOGEO, none, b) == (VS | CS));
    b.pNext = NULL;
    b.dstStageMask = VS;
    CHECK(bufferDst(PB_NOGEO, none, b) == VS);
    b.dstStageMask = VS | PB_XFB | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;
    CHECK(bufferDst(PB_NOGEO, none, b) == (PB_XFB | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT));
    b.dstStageMask = VS | CS;
    b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    CHECK(bufferDst(PB_NOGEO, none, b) == (VS | CS));
    CHECK(pb_drop(VS | FS, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT, PB_GEO) == FS);
    CHECK(pb_drop(VS | TRANSFER, VK_ACCESS_2_UNIFORM_READ_BIT, PB_GEO) == (VS | TRANSFER));
}

static VkDependencyInfo dependency(VkMemoryBarrier2* memory, uint32_t memoryCount, VkImageMemoryBarrier2* images, uint32_t imageCount) {
    return (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = (const void*)(uintptr_t)0x40,
                              .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT, .memoryBarrierCount = memoryCount,
                              .pMemoryBarriers = memory, .imageMemoryBarrierCount = imageCount, .pImageMemoryBarriers = images};
}

/* a and b byte for byte alike but for dstStageMask */
static bool sameButDst(const void* a, const void* b, size_t size, size_t dstOffset) {
    return !memcmp(a, b, dstOffset) &&
           !memcmp((const char*)a + dstOffset + 8, (const char*)b + dstOffset + 8, size - dstOffset - 8);
}

static void testRewrite(void) {
    const PbGuards none = {0}, reads = {.geoImageReads = true};
    /* the call DXVK makes after a pass with a colour and a depth attachment */
    VkMemoryBarrier2 memory[1] = {globalBarrier()};
    VkImageMemoryBarrier2 images[2] = {colorRelease(), depthRelease()};
    VkMemoryBarrier2 memorySaved[1];
    VkImageMemoryBarrier2 imagesSaved[2];
    memcpy(memorySaved, memory, sizeof(memory));
    memcpy(imagesSaved, images, sizeof(images));
    VkDependencyInfo info = dependency(memory, 1, images, 2);
    PbCopy c;
    PbCounts n = {0};

    CHECK(pb_rewrite(PB_LOG, &none, &info, &c, &n) == &info && !c.heap);
    CHECK(n.calls == 1 && n.global == 1 && n.globalWrites == 0 && n.attachment == 2 && n.fragToGeo == 3 && n.narrowed == 0);
    CHECK(n.globalDst == DXVK_GLOBAL_DST && n.attachmentDst == images[1].dstStageMask);
    CHECK(pb_rewrite(PB_OFF, &none, &info, &c, &n) == &info && n.calls == 2);

    memset(&n, 0, sizeof(n));
    const VkDependencyInfo* out = pb_rewrite(PB_NARROW, &none, &info, &c, &n);
    CHECK(out == &c.info && !c.heap && n.narrowed == 1);
    CHECK(out->pMemoryBarriers == c.memory && out->pImageMemoryBarriers == images); /* only what changes is copied */
    CHECK(out->pMemoryBarriers[0].dstStageMask == 0x1f80);
    CHECK(sameButDst(&out->pMemoryBarriers[0], &memory[0], sizeof(memory[0]), offsetof(VkMemoryBarrier2, dstStageMask)));
    CHECK(out->pNext == info.pNext && out->dependencyFlags == info.dependencyFlags && out->memoryBarrierCount == 1 &&
          out->imageMemoryBarrierCount == 2 && out->bufferMemoryBarrierCount == 0 && !out->pBufferMemoryBarriers);

    memset(&n, 0, sizeof(n));
    out = pb_rewrite(PB_NARROW_RT, &none, &info, &c, &n);
    CHECK(out == &c.info && !c.heap && n.narrowed == 3 && n.attachment == 2);
    CHECK(out->pMemoryBarriers == c.memory && out->pImageMemoryBarriers == c.images);
    CHECK(out->pImageMemoryBarriers[0].dstStageMask == 0x1c80);
    CHECK(out->pImageMemoryBarriers[1].dstStageMask == (TRANSFER | FS | CS | EARLY | LATE));
    for (int i = 0; i < 2; i++)
        CHECK(sameButDst(&out->pImageMemoryBarriers[i], &images[i], sizeof(images[i]), offsetof(VkImageMemoryBarrier2, dstStageMask)));
    /* the guard: only the global barrier */
    memset(&n, 0, sizeof(n));
    out = pb_rewrite(PB_NARROW_RT, &reads, &info, &c, &n);
    CHECK(out == &c.info && n.narrowed == 1 && out->pImageMemoryBarriers == images);
    CHECK(!memcmp(memory, memorySaved, sizeof(memory)) && !memcmp(images, imagesSaved, sizeof(images)));

    /* an image barrier that changes after one that does not */
    VkImageMemoryBarrier2 mixed[2] = {colorRelease(), colorRelease()};
    mixed[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkDependencyInfo mixedInfo = dependency(NULL, 0, mixed, 2);
    memset(&n, 0, sizeof(n));
    out = pb_rewrite(PB_NARROW_RT, &none, &mixedInfo, &c, &n);
    CHECK(out == &c.info && n.narrowed == 1 && out->pImageMemoryBarriers == c.images && !out->pMemoryBarriers);
    CHECK(out->pImageMemoryBarriers[0].dstStageMask == DXVK_RT_DST && out->pImageMemoryBarriers[1].dstStageMask == 0x1c80);
    CHECK(out->pImageMemoryBarriers[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    /* more barriers than the stack copies hold: the heap */
    enum { MANY = PB_STACK_IMAGES * 3 + 1, MANY_MEMORY = PB_STACK_MEMORY + 2 };
    VkImageMemoryBarrier2 many[MANY];
    VkMemoryBarrier2 manyMemory[MANY_MEMORY];
    for (int i = 0; i < MANY; i++) {
        many[i] = colorRelease();
        many[i].image = (VkImage)(uintptr_t)(0x10000 + i);
    }
    for (int i = 0; i < MANY_MEMORY; i++) manyMemory[i] = globalBarrier();
    manyMemory[0].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo manyInfo = dependency(manyMemory, MANY_MEMORY, many, MANY);
    memset(&n, 0, sizeof(n));
    out = pb_rewrite(PB_NARROW_RT, &none, &manyInfo, &c, &n);
    CHECK(out == &c.info && c.heap && n.narrowed == MANY + MANY_MEMORY - 1 && n.global == MANY_MEMORY && n.globalWrites == 1);
    CHECK(out->pImageMemoryBarriers != many && out->pImageMemoryBarriers != c.images);
    CHECK(out->pMemoryBarriers != manyMemory && out->pMemoryBarriers != c.memory);
    bool allNarrowed = out->pMemoryBarriers[0].dstStageMask == DXVK_GLOBAL_DST;
    for (int i = 1; i < MANY_MEMORY; i++) allNarrowed = allNarrowed && out->pMemoryBarriers[i].dstStageMask == 0x1f80;
    for (int i = 0; i < MANY; i++)
        allNarrowed = allNarrowed && out->pImageMemoryBarriers[i].dstStageMask == 0x1c80 &&
                      out->pImageMemoryBarriers[i].image == (VkImage)(uintptr_t)(0x10000 + i);
    CHECK(allNarrowed);
    CHECK(many[MANY - 1].dstStageMask == DXVK_RT_DST && manyMemory[1].dstStageMask == DXVK_GLOBAL_DST);
    free(c.heap);
    /* all three arrays on the heap, one after the other */
    enum { MANY_BUFFERS = PB_STACK_BUFFERS * 2 + 1 };
    VkBufferMemoryBarrier2 manyBuffers[MANY_BUFFERS];
    for (int i = 0; i < MANY_BUFFERS; i++)
        manyBuffers[i] = (VkBufferMemoryBarrier2){VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, NULL, FS, VK_ACCESS_2_SHADER_WRITE_BIT, VS | CS,
                                                  VK_ACCESS_2_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                                  (VkBuffer)(uintptr_t)(0x20000 + i), 0, VK_WHOLE_SIZE};
    manyInfo.bufferMemoryBarrierCount = MANY_BUFFERS;
    manyInfo.pBufferMemoryBarriers = manyBuffers;
    memset(&n, 0, sizeof(n));
    out = pb_rewrite(PB_NOGEO, &none, &manyInfo, &c, &n);
    CHECK(out == &c.info && c.heap && n.narrowed == MANY + MANY_MEMORY + MANY_BUFFERS);
    CHECK(out->pBufferMemoryBarriers != manyBuffers && out->pBufferMemoryBarriers != c.buffers);
    allNarrowed = true;
    for (int i = 0; i < MANY_MEMORY; i++) allNarrowed = allNarrowed && out->pMemoryBarriers[i].dstStageMask == 0x1f80;
    for (int i = 0; i < MANY_BUFFERS; i++)
        allNarrowed = allNarrowed && out->pBufferMemoryBarriers[i].dstStageMask == CS &&
                      out->pBufferMemoryBarriers[i].buffer == (VkBuffer)(uintptr_t)(0x20000 + i);
    for (int i = 0; i < MANY; i++)
        allNarrowed = allNarrowed && out->pImageMemoryBarriers[i].dstStageMask == 0x1c80 &&
                      out->pImageMemoryBarriers[i].image == (VkImage)(uintptr_t)(0x10000 + i);
    CHECK(allNarrowed);
    CHECK(manyBuffers[0].dstStageMask == (VS | CS));
    free(c.heap);

    /* nothing to change, and no barriers at all */
    VkMemoryBarrier2 writes[1] = {globalBarrier()};
    writes[0].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo writesInfo = dependency(writes, 1, NULL, 0);
    CHECK(pb_rewrite(PB_NARROW_RT, &none, &writesInfo, &c, &n) == &writesInfo && !c.heap);
    VkDependencyInfo empty = dependency(NULL, 0, NULL, 0);
    CHECK(pb_rewrite(PB_NOGEO, &none, &empty, &c, &n) == &empty && !c.heap);
    CHECK(pb_rewrite(PB_NOGEO, &none, NULL, &c, &n) == NULL && !c.heap);
}

/* ---- hand-made SPIR-V ---- */

#define R PB_SCAN_IMAGE_READS
#define W PB_SCAN_WRITES

static unsigned scan(const uint32_t* code, size_t words) {
    return pb_spirv_geo_scan(code, words, R | W);
}

typedef struct { uint32_t words[256]; size_t n; } Code;

static void op(Code* c, uint32_t opcode, int count, ...) {
    va_list args;
    va_start(args, count);
    c->words[c->n++] = (uint32_t)(count + 1) << 16 | opcode;
    for (int i = 0; i < count; i++) c->words[c->n++] = va_arg(args, uint32_t);
    va_end(args);
}

/* An entry point of `model` that runs `image op` on an image of Dim `dim` (5: Buffer): ids 1 main,
 * 2 void, 3 fn type, 4 float, 5 vec4, 6 image type, 7 sampled image type, 8 pointer, 9 variable,
 * 10 int, 11 coordinate, 12 label, 13 loaded, 14 result; the image operand is `operand`. */
static Code module(uint32_t model, uint32_t imageOp, uint32_t dim, uint32_t operand) {
    Code c = {.words = {0x07230203u, 0x00010000u, 0, 20, 0}, .n = 5};
    op(&c, 17, 1, 1u);                           /* OpCapability Shader */
    op(&c, 14, 2, 0u, 1u);                       /* OpMemoryModel Logical GLSL450 */
    op(&c, 15, 4, model, 1u, 0x6e69616du, 0u);   /* OpEntryPoint model %1 "main" */
    op(&c, 19, 1, 2u);                           /* OpTypeVoid %2 */
    op(&c, 33, 2, 3u, 2u);                       /* OpTypeFunction %3 %2 */
    op(&c, 22, 2, 4u, 32u);                      /* OpTypeFloat %4 32 */
    op(&c, 23, 3, 5u, 4u, 4u);                   /* OpTypeVector %5 %4 4 */
    op(&c, 25, 8, 6u, 4u, dim, 0u, 0u, 0u, 1u, 0u); /* OpTypeImage %6 %4 dim ... */
    op(&c, 27, 2, 7u, 6u);                       /* OpTypeSampledImage %7 %6 */
    op(&c, 32, 3, 8u, 0u, 7u);                   /* OpTypePointer %8 UniformConstant %7 */
    op(&c, 59, 3, 8u, 9u, 0u);                   /* OpVariable %8 %9 UniformConstant */
    op(&c, 21, 3, 10u, 32u, 1u);                 /* OpTypeInt %10 32 1 */
    op(&c, 43, 3, 10u, 11u, 0u);                 /* OpConstant %10 %11 0 */
    op(&c, 54, 4, 2u, 1u, 0u, 3u);               /* OpFunction %2 %1 None %3 */
    op(&c, 248, 1, 12u);                         /* OpLabel %12 */
    op(&c, 61, 3, 7u, 13u, 9u);                  /* OpLoad %7 %13 %9 */
    op(&c, imageOp, 4, 5u, 14u, operand, 11u);   /* imageOp %5 %14 operand %11 */
    op(&c, 253, 0);                              /* OpReturn */
    op(&c, 56, 0);                               /* OpFunctionEnd */
    return c;
}

enum { THROUGH_VARIABLE, THROUGH_ACCESS_CHAIN, THROUGH_UNDEFINED, THROUGH_UNKNOWN_OP, THROUGH_COPY, FORWARD_ONLY };

/* An entry point of `model` storing a float through a pointer of `storageClass` (`how`): ids 1 main,
 * 2 void, 3 fn type, 4 float, 5 pointer type, 6 variable, 7 constant, 8 label, 9 int, 10 index,
 * 11 struct, 12 struct pointer type, 13 struct variable, 14 access chain or converted pointer, 15 a
 * 64-bit int, 16 its constant, 17 a Function variable, 18 its pointer type; 19 is not defined. */
static Code storeModule(uint32_t model, uint32_t storageClass, int how) {
    Code c = {.words = {0x07230203u, 0x00010000u, 0, 20, 0}, .n = 5};
    op(&c, 17, 1, 1u);                               /* OpCapability Shader */
    op(&c, 14, 2, 0u, 1u);                           /* OpMemoryModel Logical GLSL450 */
    op(&c, 15, 4, model, 1u, 0x6e69616du, 0u);       /* OpEntryPoint model %1 "main" */
    op(&c, 19, 1, 2u);                               /* OpTypeVoid %2 */
    op(&c, 33, 2, 3u, 2u);                           /* OpTypeFunction %3 %2 */
    op(&c, 22, 2, 4u, 32u);                          /* OpTypeFloat %4 32 */
    if (how == FORWARD_ONLY) op(&c, 39, 2, 5u, storageClass); /* OpTypeForwardPointer %5 storageClass */
    else op(&c, 32, 3, 5u, storageClass, 4u);        /* OpTypePointer %5 storageClass %4 */
    op(&c, 59, 3, 5u, 6u, storageClass);             /* OpVariable %5 %6 storageClass */
    op(&c, 43, 3, 4u, 7u, 0x3f800000u);              /* OpConstant %4 %7 1.0 */
    op(&c, 21, 3, 9u, 32u, 1u);                      /* OpTypeInt %9 32 1 */
    op(&c, 43, 3, 9u, 10u, 0u);                      /* OpConstant %9 %10 0 */
    op(&c, 30, 2, 11u, 4u);                          /* OpTypeStruct %11 %4 */
    op(&c, 32, 3, 12u, storageClass, 11u);           /* OpTypePointer %12 storageClass %11 */
    op(&c, 59, 3, 12u, 13u, storageClass);           /* OpVariable %12 %13 storageClass */
    op(&c, 21, 3, 15u, 64u, 0u);                     /* OpTypeInt %15 64 0 */
    op(&c, 43, 4, 15u, 16u, 0x1000u, 0u);            /* OpConstant %15 %16 0x1000 */
    op(&c, 32, 3, 18u, 7u, 4u);                      /* OpTypePointer %18 Function %4 */
    op(&c, 54, 4, 2u, 1u, 0u, 3u);                   /* OpFunction %2 %1 None %3 */
    op(&c, 248, 1, 8u);                              /* OpLabel %8 */
    op(&c, 59, 3, 18u, 17u, 7u);                     /* OpVariable %18 %17 Function */
    switch (how) {
    case THROUGH_VARIABLE:
    case FORWARD_ONLY:
        op(&c, 62, 2, 6u, 7u);                       /* OpStore %6 %7 */
        break;
    case THROUGH_ACCESS_CHAIN:
        op(&c, 65, 4, 5u, 14u, 13u, 10u);            /* OpAccessChain %5 %14 %13 %10 */
        op(&c, 62, 2, 14u, 7u);                      /* OpStore %14 %7 */
        break;
    case THROUGH_UNDEFINED:
        op(&c, 62, 2, 19u, 7u);                      /* OpStore %19 %7: nothing defines %19 */
        break;
    case THROUGH_UNKNOWN_OP:
        op(&c, 120, 3, 5u, 14u, 16u);                /* OpConvertUToPtr %5 %14 %16 */
        op(&c, 62, 2, 14u, 7u);                      /* OpStore %14 %7 */
        break;
    case THROUGH_COPY:
        op(&c, 63, 2, 6u, 17u);                      /* OpCopyMemory %6 %17 */
        break;
    }
    op(&c, 253, 0);                                  /* OpReturn */
    op(&c, 56, 0);                                   /* OpFunctionEnd */
    return c;
}

/* storeModule's with the store replaced by `atomic` on %6, a Function variable (the operands after
 * the pointer are not looked at). */
static Code atomicModule(uint32_t model, uint32_t atomic) {
    Code c = storeModule(model, 7 /* Function */, THROUGH_VARIABLE);
    c.n -= 5; /* OpStore, OpReturn, OpFunctionEnd */
    if (atomic == 227 /* OpAtomicLoad */) op(&c, atomic, 5, 4u, 19u, 6u, 10u, 10u);
    else op(&c, atomic, 6, 4u, 19u, 6u, 10u, 10u, 7u);
    op(&c, 253, 0);
    op(&c, 56, 0);
    return c;
}

/* The length of `code` cut in the middle of its first OpFunction. */
static size_t cutInFunction(const uint32_t* code, size_t words) {
    for (size_t i = 5; i < words; i += code[i] >> 16)
        if ((code[i] & 0xffffu) == 54) return i + 2;
    return words;
}

static void testSpirv(void) {
#define SCAN(array) scan(array, sizeof(array) / 4)
#define WORDS(array) (sizeof(array) / 4)
    CHECK(SCAN(pb_vert_sample_reads) == R);
    CHECK(SCAN(pb_vert_texel_buffer_none) == 0);
    CHECK(SCAN(pb_vert_storage_image_reads) == R);
    CHECK(SCAN(pb_vert_plain_none) == 0);
    CHECK(SCAN(pb_tese_separate_reads) == R);
    CHECK(SCAN(pb_geom_fetch_reads) == R);
    CHECK(SCAN(pb_frag_sample_none) == 0);
    CHECK(SCAN(pb_vert_ssbo_writes) == W);
    CHECK(SCAN(pb_vert_ssbo_uniform_writes) == W);
    CHECK(SCAN(pb_vert_ssbo_readonly_none) == 0);
    CHECK(SCAN(pb_vert_image_store_writes) == W);
    CHECK(SCAN(pb_vert_atomic_writes) == W);
    CHECK(SCAN(pb_vert_buffer_reference_writes) == W);
    CHECK(SCAN(pb_geom_ssbo_writes) == W);
    CHECK(SCAN(pb_vert_sample_ssbo_reads_writes) == (R | W));
    CHECK(SCAN(pb_frag_ssbo_none) == 0);
    CHECK(SCAN(pb_vert_xfb_none) == 0);
    /* only what is asked for (the guards not tripped yet) */
    CHECK(pb_spirv_geo_scan(pb_vert_sample_ssbo_reads_writes, WORDS(pb_vert_sample_ssbo_reads_writes), R) == R);
    CHECK(pb_spirv_geo_scan(pb_vert_sample_ssbo_reads_writes, WORDS(pb_vert_sample_ssbo_reads_writes), W) == W);
    CHECK(pb_spirv_geo_scan(pb_vert_sample_ssbo_reads_writes, WORDS(pb_vert_sample_ssbo_reads_writes), 0) == 0);
    CHECK(pb_spirv_geo_scan(pb_vert_buffer_reference_writes, WORDS(pb_vert_buffer_reference_writes), R) == 0);
    CHECK(pb_spirv_geo_scan(pb_vert_sample_reads, WORDS(pb_vert_sample_reads), W) == 0);
    /* damaged: past a Vertex entry point it counts as both, before one or without one as neither */
    CHECK(scan(pb_vert_sample_reads, cutInFunction(pb_vert_sample_reads, WORDS(pb_vert_sample_reads))) == (R | W));
    CHECK(scan(pb_vert_plain_none, cutInFunction(pb_vert_plain_none, WORDS(pb_vert_plain_none))) == (R | W));
    CHECK(pb_spirv_geo_scan(pb_vert_plain_none, cutInFunction(pb_vert_plain_none, WORDS(pb_vert_plain_none)), W) == W);
    CHECK(scan(pb_frag_sample_none, cutInFunction(pb_frag_sample_none, WORDS(pb_frag_sample_none))) == 0);
    CHECK(scan(pb_vert_sample_reads, 12) == 0);
    CHECK(scan(pb_vert_sample_reads, 4) == 0 && scan(NULL, 100) == 0);
    uint32_t wrongMagic[WORDS(pb_vert_sample_reads)];
    memcpy(wrongMagic, pb_vert_sample_reads, sizeof(wrongMagic));
    wrongMagic[0] = 0;
    CHECK(SCAN(wrongMagic) == 0);
    /* byte-swapped: not parsed, counts as everything asked for */
    uint32_t swapped[WORDS(pb_frag_sample_none)];
    for (size_t i = 0; i < WORDS(swapped); i++) swapped[i] = __builtin_bswap32(pb_frag_sample_none[i]);
    CHECK(swapped[0] == 0x03022307u);
    CHECK(SCAN(swapped) == (R | W) && pb_spirv_geo_scan(swapped, WORDS(swapped), R) == R && pb_spirv_geo_scan(swapped, 4, R) == 0);

    Code c = module(0, 95 /* OpImageFetch */, 1 /* 2D */, 13);
    CHECK(scan(c.words, c.n) == R);
    c = module(0, 95, 5 /* Buffer */, 13);
    CHECK(scan(c.words, c.n) == 0);
    c = module(4 /* Fragment */, 95, 1, 13);
    CHECK(scan(c.words, c.n) == 0);
    for (uint32_t model = 1; model <= 3; model++) {
        c = module(model, 87 /* OpImageSampleImplicitLod */, 1, 13);
        CHECK(scan(c.words, c.n) == R);
    }
    c = module(5 /* GLCompute */, 87, 1, 13);
    CHECK(scan(c.words, c.n) == 0);
    c = module(0, 99 /* OpImageWrite: a write, not a read */, 1, 13);
    CHECK(scan(c.words, c.n) == W);
    c = module(0, 320 /* OpImageSparseRead */, 1, 13);
    CHECK(scan(c.words, c.n) == R);
    c = module(0, 313 /* OpImageSparseFetch */, 5, 13);
    CHECK(scan(c.words, c.n) == 0);
    /* the operand names an id nothing defines, or one past the bound: a read */
    c = module(0, 95, 5, 15);
    CHECK(scan(c.words, c.n) == R);
    c = module(0, 95, 5, 99);
    CHECK(scan(c.words, c.n) == R);
    /* OpImageTexelPointer names the variable: pointer -> sampled image -> image (the shape is not valid
     * SPIR-V, the walk is the point) */
    c = module(0, 60, 1, 9);
    CHECK(scan(c.words, c.n) == R);
    c = module(0, 60, 5, 9);
    CHECK(scan(c.words, c.n) == 0);
    /* an absurd bound */
    c = module(0, 95, 5, 13);
    c.words[3] = 1u << 30;
    CHECK(scan(c.words, c.n) == R);
    /* an instruction running past the end */
    c = module(0, 95, 5, 13);
    c.words[c.n - 2] = (100u << 16) | 253u;
    CHECK(scan(c.words, c.n) == (R | W));

    /* stores and copies: the storage classes written through */
    const uint32_t writable[] = {2 /* Uniform */, 11 /* Image */, 12 /* StorageBuffer */, 5349 /* PhysicalStorageBuffer */};
    const uint32_t local[] = {3 /* Output */, 6 /* Private */, 7 /* Function */, 4 /* Workgroup */};
    for (size_t i = 0; i < sizeof(writable) / sizeof(writable[0]); i++) {
        for (int how = THROUGH_VARIABLE; how <= FORWARD_ONLY; how++) {
            c = storeModule(0, writable[i], how);
            CHECK(scan(c.words, c.n) == W);
        }
        c = storeModule(4 /* Fragment */, writable[i], THROUGH_ACCESS_CHAIN);
        CHECK(scan(c.words, c.n) == 0);
        c = storeModule(2 /* TessellationEvaluation */, writable[i], THROUGH_VARIABLE);
        CHECK(scan(c.words, c.n) == W);
    }
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++) {
        c = storeModule(0, local[i], THROUGH_VARIABLE);
        CHECK(scan(c.words, c.n) == 0);
        c = storeModule(3 /* Geometry */, local[i], THROUGH_ACCESS_CHAIN);
        CHECK(scan(c.words, c.n) == 0);
        c = storeModule(0, local[i], THROUGH_COPY);
        CHECK(scan(c.words, c.n) == 0);
        /* pointers whose type cannot be followed: a write */
        c = storeModule(0, local[i], THROUGH_UNDEFINED);
        CHECK(scan(c.words, c.n) == W);
        c = storeModule(0, local[i], THROUGH_UNKNOWN_OP);
        CHECK(scan(c.words, c.n) == W);
    }
    c = storeModule(0, 7, THROUGH_VARIABLE);
    c.words[3] = 1u << 30; /* an absurd bound with a store to follow: a write */
    CHECK(scan(c.words, c.n) == W);
    c = storeModule(0, 7, THROUGH_VARIABLE);
    c.words[3] = 6; /* the store's pointer past the bound: a write */
    CHECK(scan(c.words, c.n) == W);
    /* atomics write, OpAtomicLoad does not */
    const uint32_t atomics[] = {228, 229, 232, 234, 242, 5614, 5615, 6035};
    for (size_t i = 0; i < sizeof(atomics) / sizeof(atomics[0]); i++) {
        c = atomicModule(0, atomics[i]);
        CHECK(scan(c.words, c.n) == W);
        c = atomicModule(4 /* Fragment */, atomics[i]);
        CHECK(scan(c.words, c.n) == 0);
    }
    c = atomicModule(0, 227);
    CHECK(scan(c.words, c.n) == 0);
#undef SCAN
#undef WORDS
}

int main(void) {
    testModes();
    testRules();
    testRewrite();
    testSpirv();
    if (failures) {
        printf("PASS-BARRIER-FAIL (%d)\n", failures);
        return 1;
    }
    printf("PASS-BARRIER-PASS\n");
    return 0;
}
