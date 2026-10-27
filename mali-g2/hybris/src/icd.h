/*
 * libdroiddeck_hybris_vk.so: the guest's Vulkan ICD for the in-process Mali path. It sits in front of
 * libhybris's glibc ICD (libhybris-vulkan-icd.so.0, which runs the Android Vulkan HAL in this process)
 * and adds what DroidDeck needs on top of the bare driver:
 *   - X11 WSI (wsi.c): VK_KHR_surface, VK_KHR_xcb_surface, VK_KHR_xlib_surface, VK_KHR_swapchain,
 *     with AHardwareBuffer images shown zero-copy by DroidDeck's compositor, or copied into the window;
 *   - the Mali workarounds the Vortek client carries, and the command memory release the Vortek
 *     server does (mali_fixes.c).
 * Dispatchable handles are libhybris's (the HAL's) own: nothing is wrapped, calls this library does
 * not intercept go straight to libhybris.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_HYBRIS_ICD_H
#define DROIDDECK_HYBRIS_ICD_H

#define VK_NO_PROTOTYPES
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_xcb.h>
/* Xlib's types, without Xlib's headers (the Display comes from the application). */
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#include <vulkan/vulkan_xlib.h>
#include <android/hardware_buffer.h>
#include <vulkan/vulkan_android.h>

#define EXPORT __attribute__((visibility("default")))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifndef VK_KHR_vertex_attribute_divisor
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR ((VkStructureType)1000525000)
typedef struct VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR {
    VkStructureType sType;
    void* pNext;
    uint32_t maxVertexAttribDivisor;
    VkBool32 supportsNonZeroFirstInstance;
} VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR;
#endif

typedef struct Instance {
    VkInstance handle;
    struct Instance* next;
    bool vkd3d;  /* the application is vkd3d-proton */
#define INSTANCE_FUNCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(EnumeratePhysicalDeviceGroups) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFeatures2) \
    X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define X(name) PFN_vk##name name;
    INSTANCE_FUNCS(X)
#undef X
} Instance;

#ifndef VK_EXT_map_memory_placed
#define VK_EXT_map_memory_placed 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT ((VkStructureType)1000272000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT ((VkStructureType)1000272001)
#define VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT ((VkStructureType)1000272002)
#define VK_MEMORY_MAP_PLACED_BIT_EXT 0x00000001
#define VK_MEMORY_UNMAP_RESERVE_BIT_EXT 0x00000001
typedef struct VkPhysicalDeviceMapMemoryPlacedFeaturesEXT {
    VkStructureType sType;
    void* pNext;
    VkBool32 memoryMapPlaced;
    VkBool32 memoryMapRangePlaced;
    VkBool32 memoryUnmapReserve;
} VkPhysicalDeviceMapMemoryPlacedFeaturesEXT;
typedef struct VkPhysicalDeviceMapMemoryPlacedPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkDeviceSize minPlacedMemoryMapAlignment;
} VkPhysicalDeviceMapMemoryPlacedPropertiesEXT;
typedef struct VkMemoryMapPlacedInfoEXT {
    VkStructureType sType;
    const void* pNext;
    void* pPlacedAddress;
} VkMemoryMapPlacedInfoEXT;
#endif

#define MAX_QUEUES 32
#define PLACED_TABLE_BITS 8
struct PlacedMemory;

typedef struct Device {
    VkDevice handle;
    struct Device* next;
    VkPhysicalDevice physical;
    Instance* instance;
    VkPhysicalDeviceMemoryProperties memory;
    bool swapchainEnabled;
    bool ahb;            /* VK_ANDROID_external_memory_android_hardware_buffer enabled */
    bool syncFd;         /* VK_KHR_external_fence_fd and VK_KHR_external_semaphore_fd enabled */
    uint32_t queueCount;
    VkQueue queues[MAX_QUEUES];
    uint32_t queueFamilies[MAX_QUEUES];
    /* Acquire signals the application's semaphore or fence on this queue. It is one created for us
     * when the graphics family had a queue to spare; otherwise one of the application's, and then
     * every submission to it takes queueLock (lockQueue). */
    VkQueue internalQueue;
    uint32_t internalFamily;
    bool lockQueue;
    pthread_mutex_t queueLock;
    /* VK_EXT_map_memory_placed enabled (placed.c): host-visible memory by VkDeviceMemory. */
    bool placed;
    pthread_mutex_t placedLock;
    struct PlacedMemory* placedTable[1u << PLACED_TABLE_BITS];
    PFN_vkMapMemory2KHR MapMemory2;
    PFN_vkUnmapMemory2KHR UnmapMemory2;
#define DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(QueueSubmit) X(QueueSubmit2) X(QueueSubmit2KHR) X(QueueWaitIdle) \
    X(QueueBindSparse) X(DeviceWaitIdle) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) \
    X(FreeMemory) X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus) X(CreateSemaphore) X(DestroySemaphore) \
    X(ImportSemaphoreFdKHR) X(ImportFenceFdKHR) X(GetFenceFdKHR) X(GetAndroidHardwareBufferPropertiesANDROID) \
    X(CreateShaderModule) X(CreateGraphicsPipelines) X(ResetCommandPool) X(ResetCommandBuffer) X(MapMemory2KHR) X(UnmapMemory2KHR) \
    X(FlushMappedMemoryRanges) X(FreeCommandBuffers)
#define X(name) PFN_vk##name name;
    DEVICE_FUNCS(X)
#undef X
} Device;

Instance* icd_instance(VkInstance instance);
Instance* icd_instance_of(VkPhysicalDevice physical);
Device* icd_device(VkDevice device);
Device* icd_device_of_queue(VkQueue queue, uint32_t* family);

/* vkQueueSubmit on a queue this library also uses, with queueLock held when needed. */
VkResult icd_queue_submit(Device* device, VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence);
VkResult icd_queue_wait_idle(Device* device, VkQueue queue);

int icd_env_int(const char* name, int defaultValue);

/* wsi.c */
PFN_vkVoidFunction wsi_instance_proc(const char* name);
PFN_vkVoidFunction wsi_device_proc(const char* name);
extern const VkExtensionProperties wsi_instance_extensions[];
extern const uint32_t wsi_instance_extension_count;
void wsi_device_destroyed(Device* device);

/* mali_fixes.c */
PFN_vkVoidFunction mali_instance_proc(const char* name);
/* `handle` may be VK_NULL_HANDLE (vkGetInstanceProcAddr). */
PFN_vkVoidFunction mali_device_proc(VkDevice handle, const char* name);
/* The device's core features as the application may enable them: the ones reported as emulated
 * but missing in the driver are turned off in `enabled`. */
void mali_drop_emulated_features(Instance* instance, VkPhysicalDevice physical, VkPhysicalDeviceFeatures* enabled);
void mali_device_created(Device* device);
void mali_device_destroyed(Device* device);

/* placed.c */
extern const char* const placed_extension;
extern const char* const placed_needs[];
extern const uint32_t placed_need_count;
bool placed_wanted(void);
bool icd_placed_available(Instance* instance, VkPhysicalDevice physical);
void placed_fill_features(void* chain);
void placed_fill_properties(void* chain);
void placed_device_created(Device* device);
void placed_device_destroyed(Device* device);
PFN_vkVoidFunction placed_device_proc(Device* device, const char* name);

#endif
