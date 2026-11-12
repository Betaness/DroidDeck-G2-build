/* [mali-g2] DROIDDECK_HYBRIS_PASS_BARRIER through the Vulkan loader and the build-tree ICD (run by
 * pass_barrier_icd_test.sh): a transition to GENERAL, then twice the barrier DXVK records after a
 * pass with a sampled render target (the global barrier 0x8c -> 0x01001ff8, the release 0x1cf8),
 * with a vertex shader module that samples created between the two (the guard of narrow-rt).
 * Checks that the application's barriers are not written. "stores" as argument enables
 * vertexPipelineStoresAndAtomics. Prints PB-ICD-DONE. */
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

int main(int argc, char** argv) {
    bool stores = argc > 1 && !strcmp(argv[1], "stores");
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
    VkPhysicalDeviceVulkan13Features features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = VK_TRUE};
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features13};
    features.features.vertexPipelineStoresAndAtomics = stores;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo};
    VkDevice device;
    VKC(vkCreateDevice(physical, &deviceInfo, NULL, &device));
    PFN_vkCmdPipelineBarrier2 barrier = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2");
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
    VkShaderModuleCreateInfo moduleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(pb_vert_sample_reads),
                                           .pCode = pb_vert_sample_reads};
    VkShaderModule module;
    VKC(vkCreateShaderModule(device, &moduleInfo, NULL, &module));
    barrier(commandBuffer, &pass);
    if (global.dstStageMask != 0x01001ff8ull || release.dstStageMask != 0x1cf8) printf("FAIL the application's barriers were written\n");
    VKC(vkEndCommandBuffer(commandBuffer));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commandBuffer};
    VKC(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(queue));
    vkDestroyShaderModule(device, module, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("PB-ICD-DONE\n");
    return 0;
}
