/* [mali-g2] A Vulkan ICD for DROIDDECK_HYBRIS_NEXT_ICD that passes everything to the ICD named by
 * PB_NEXT_ICD (default libvulkan_lvp.so) and prints, for every vkCmdPipelineBarrier2 and
 * vkCmdPipelineBarrier2KHR it gets, the destination stage masks of the memory and image barriers:
 * what the driver receives (pass_barrier_icd_test.sh). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))

static void* next;
static PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr;
static PFN_vkCmdPipelineBarrier2 nextPipelineBarrier2, nextPipelineBarrier2KHR;

static void* nextSymbol(const char* name) {
    if (!next) {
        const char* path = getenv("PB_NEXT_ICD");
        if (!(next = dlopen(path && *path ? path : "libvulkan_lvp.so", RTLD_NOW | RTLD_LOCAL))) {
            fprintf(stderr, "pass_barrier_next_icd: %s\n", dlerror());
            exit(1);
        }
        nextGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(next, "vk_icdGetInstanceProcAddr");
    }
    return dlsym(next, name);
}

static void printBarriers(const char* entry, const VkDependencyInfo* info) {
    printf("%s:", entry);
    for (uint32_t i = 0; i < info->memoryBarrierCount; i++) printf(" memory 0x%llx", (unsigned long long)info->pMemoryBarriers[i].dstStageMask);
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++) printf(" image 0x%llx", (unsigned long long)info->pImageMemoryBarriers[i].dstStageMask);
    printf("\n");
    fflush(stdout);
}

static void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* info) {
    printBarriers("driver", info);
    nextPipelineBarrier2(commandBuffer, info);
}

static void VKAPI_CALL CmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const VkDependencyInfo* info) {
    printBarriers("driver", info);
    printf("driver-khr\n");
    nextPipelineBarrier2KHR(commandBuffer, info);
}

static PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkVoidFunction function = nextGetDeviceProcAddr(device, name);
    if (!function) return function;
    if (!strcmp(name, "vkCmdPipelineBarrier2")) {
        nextPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)function;
        return (PFN_vkVoidFunction)CmdPipelineBarrier2;
    }
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) {
        nextPipelineBarrier2KHR = (PFN_vkCmdPipelineBarrier2)function;
        return (PFN_vkVoidFunction)CmdPipelineBarrier2KHR;
    }
    return function;
}

EXPORT VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    return ((VkResult(*)(uint32_t*))nextSymbol("vk_icdNegotiateLoaderICDInterfaceVersion"))(version);
}

EXPORT PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    PFN_vkVoidFunction (*gpdpa)(VkInstance, const char*) = (PFN_vkVoidFunction(*)(VkInstance, const char*))nextSymbol("vk_icdGetPhysicalDeviceProcAddr");
    return gpdpa ? gpdpa(instance, name) : NULL;
}

EXPORT PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    nextSymbol("vk_icdGetInstanceProcAddr");
    if (strcmp(name, "vkGetDeviceProcAddr")) return nextGetInstanceProcAddr(instance, name);
    nextGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)nextGetInstanceProcAddr(instance, name);
    return (PFN_vkVoidFunction)GetDeviceProcAddr;
}
