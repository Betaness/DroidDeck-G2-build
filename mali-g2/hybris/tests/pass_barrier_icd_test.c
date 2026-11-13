/* [mali-g2] DROIDDECK_HYBRIS_PASS_BARRIER through the Vulkan loader and the build-tree ICD (run by
 * pass_barrier_icd_test.sh): a transition to GENERAL, then twice the barrier DXVK records after a
 * pass with a sampled render target (the global barrier 0x8c -> 0x01001ff8, the release 0x1cf8),
 * with the shader code of the arguments handed to the driver between the two (the guards):
 *   sample  a vertex shader module that samples (the default)
 *   writes  a vertex shader module that stores through a buffer reference
 *   xfb     a vertex shader module declaring transform feedback
 *   inline  a graphics pipeline whose vertex stage reads a storage image, as inline code
 *           (VK_KHR_maintenance5)
 * and the flags "stores" (vertexPipelineStoresAndAtomics enabled) and "khr" (every barrier through
 * vkCmdPipelineBarrier2KHR, VK_KHR_synchronization2 enabled). Checks that the application's barriers
 * are not written. Prints PB-ICD-DONE. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "pass_barrier_spv.h"

#define VKC(x)                                              \
    do {                                                    \
        VkResult r_ = (x);                                  \
        if (r_ != VK_SUCCESS) {                             \
            printf("FAIL %s = %d\n", #x, r_);               \
            exit(1);                                        \
        }                                                   \
    } while (0)

static bool has(int argc, char** argv, const char* word) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], word)) return true;
    return false;
}

static VkShaderModule createModule(VkDevice device, const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule module;
    VKC(vkCreateShaderModule(device, &info, NULL, &module));
    return module;
}

/* A pipeline of a vertex stage given as inline code, rasterizer discard, no attachments. */
static VkPipeline createInlinePipeline(VkDevice device, VkDescriptorSetLayout* setLayout, VkPipelineLayout* layout) {
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VKC(vkCreateDescriptorSetLayout(device, &setInfo, NULL, setLayout));
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = setLayout};
    VKC(vkCreatePipelineLayout(device, &layoutInfo, NULL, layout));
    VkShaderModuleCreateInfo code = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(pb_vert_storage_image_reads),
                                     .pCode = pb_vert_storage_image_reads};
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &code, .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                             .pName = "main"};
    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                       .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = VK_TRUE,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, .stageCount = 1, .pStages = &stage,
                                         .pVertexInputState = &vertexInput, .pInputAssemblyState = &assembly,
                                         .pRasterizationState = &raster, .layout = *layout};
    VkPipeline pipeline;
    VKC(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
    return pipeline;
}

int main(int argc, char** argv) {
    bool stores = has(argc, argv, "stores"), khr = has(argc, argv, "khr"), writes = has(argc, argv, "writes"), xfb = has(argc, argv, "xfb"),
         inline_ = has(argc, argv, "inline"), sample = has(argc, argv, "sample") || !(writes || xfb || inline_);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    VKC(vkCreateInstance(&instanceInfo, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (!count) {
        printf("FAIL no device\n");
        return 1;
    }
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
                                         .pQueuePriorities = &priority};
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .maintenance5 = VK_TRUE};
    VkPhysicalDeviceVulkan13Features features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, inline_ ? &maintenance5 : NULL,
                                                   .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features13};
    features.features.vertexPipelineStoresAndAtomics = stores;
    const char* extensions[2];
    uint32_t extensionCount = 0;
    if (khr) extensions[extensionCount++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    if (inline_) extensions[extensionCount++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo,
                                     .enabledExtensionCount = extensionCount, .ppEnabledExtensionNames = extensions};
    VkDevice device;
    VKC(vkCreateDevice(physical, &deviceInfo, NULL, &device));
    const char* barrierName = khr ? "vkCmdPipelineBarrier2KHR" : "vkCmdPipelineBarrier2";
    PFN_vkCmdPipelineBarrier2 barrier = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(device, barrierName);
    if (!barrier) {
        printf("FAIL no %s\n", barrierName);
        return 1;
    }
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                   .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .tiling = VK_IMAGE_TILING_OPTIMAL,
                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage image;
    VKC(vkCreateImage(device, &imageInfo, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
                                       .memoryTypeIndex = (uint32_t)__builtin_ctz(requirements.memoryTypeBits)};
    VkDeviceMemory memory;
    VKC(vkAllocateMemory(device, &allocation, NULL, &memory));
    VKC(vkBindImageMemory(device, image, memory, 0));

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool pool;
    VKC(vkCreateCommandPool(device, &poolInfo, NULL, &pool));
    VkCommandBufferAllocateInfo bufferInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer commandBuffer;
    VKC(vkAllocateCommandBuffers(device, &bufferInfo, &commandBuffer));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKC(vkBeginCommandBuffer(commandBuffer, &begin));
    VkImageMemoryBarrier2 toGeneral = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo transition = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral};
    barrier(commandBuffer, &transition);
    VkMemoryBarrier2 global = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, 0x8c, 0, 0x01001ff8ull, 0};
    VkImageMemoryBarrier2 release = toGeneral;
    release.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    release.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    release.dstStageMask = 0x1cf8;
    release.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDependencyInfo pass = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &global,
                             .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &release};
    barrier(commandBuffer, &pass);
    VkShaderModule modules[3];
    uint32_t moduleCount = 0;
    if (sample) modules[moduleCount++] = createModule(device, pb_vert_sample_reads, sizeof(pb_vert_sample_reads));
    if (writes) modules[moduleCount++] = createModule(device, pb_vert_buffer_reference_writes, sizeof(pb_vert_buffer_reference_writes));
    if (xfb) modules[moduleCount++] = createModule(device, pb_vert_xfb_none, sizeof(pb_vert_xfb_none));
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = inline_ ? createInlinePipeline(device, &setLayout, &layout) : VK_NULL_HANDLE;
    barrier(commandBuffer, &pass);
    if (global.dstStageMask != 0x01001ff8ull || release.dstStageMask != 0x1cf8) printf("FAIL the application's barriers were written\n");
    VKC(vkEndCommandBuffer(commandBuffer));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commandBuffer};
    VKC(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(queue));
    for (uint32_t i = 0; i < moduleCount; i++) vkDestroyShaderModule(device, modules[i], NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, setLayout, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("PB-ICD-DONE\n");
    return 0;
}
