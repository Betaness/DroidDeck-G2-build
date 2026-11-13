/*
 * A Vulkan driver of one device for tests/guards_icd_test.c (see tests/fake_icd.h): one queue family
 * of FAKE_QUEUES queues, a device-local heap of FAKE_HEAP_MB and a host heap, no extensions. Waits,
 * idle waits and query reads play their FakeScript; vkEndCommandBuffer and pipeline creation fault
 * on demand.
 */
#include "fake_icd.h"

#include <string.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT FakeIcd fake_icd;

typedef struct {
    uintptr_t loaderMagic;
} Object;

static Object instanceObject, physicalObject, deviceObject, queueObjects[FAKE_QUEUES];

static VkResult play(FakeScript* s) {
    s->calls++;
    return s->next < s->count ? s->results[s->next++] : s->otherwise;
}

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* out) {
    *out = (VkInstance)&instanceObject;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
}

static VkResult VKAPI_CALL EnumerateExtensions(const char* layer, uint32_t* count, VkExtensionProperties* properties) {
    *count = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EnumerateDeviceExtensions(VkPhysicalDevice physical, const char* layer, uint32_t* count,
                                                     VkExtensionProperties* properties) {
    *count = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* physicals) {
    if (physicals && *count) physicals[0] = (VkPhysicalDevice)&physicalObject;
    *count = 1;
    return VK_SUCCESS;
}

static void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties) {
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion = VK_API_VERSION_1_3;
    properties->vendorID = 0x13b5;
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strcpy(properties->deviceName, "fake Mali");
}

static void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures* features) {
    memset(features, 0, sizeof(*features));
}

static void VKAPI_CALL GetQueueFamilies(VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties* families) {
    if (families && *count) {
        memset(families, 0, sizeof(*families));
        families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        families[0].queueCount = FAKE_QUEUES;
    }
    *count = 1;
}

static void VKAPI_CALL GetMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties* memory) {
    memset(memory, 0, sizeof(*memory));
    memory->memoryHeapCount = 2;
    memory->memoryHeaps[0].size = FAKE_HEAP_MB << 20;
    memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory->memoryHeaps[1].size = 2048ull << 20;
    memory->memoryTypeCount = 2;
    memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    memory->memoryTypes[1].heapIndex = 1;
    memory->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

static void VKAPI_CALL GetMemoryProperties2(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2* memory) {
    GetMemoryProperties(physical, &memory->memoryProperties);
    for (VkBaseOutStructure* s = memory->pNext; s; s = s->pNext) {
        if (s->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT* budget = (VkPhysicalDeviceMemoryBudgetPropertiesEXT*)s;
        budget->heapBudget[0] = 9000ull << 20;
        budget->heapUsage[0] = 100ull << 20;
        budget->heapBudget[1] = 1500ull << 20;
        fake_icd.budgets++;
    }
}

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                        VkDevice* out) {
    *out = (VkDevice)&deviceObject;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
}

static void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    *queue = family == 0 && index < FAKE_QUEUES ? (VkQueue)&queueObjects[index] : VK_NULL_HANDLE;
}

static VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    fake_icd.submits++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    fake_icd.submits2++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    fake_icd.lastIdleQueue = queue;
    return play(&fake_icd.queueIdle);
}

static VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    return play(&fake_icd.deviceIdle);
}

static VkResult VKAPI_CALL WaitForFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    return play(&fake_icd.fences);
}

static VkResult VKAPI_CALL WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    return play(&fake_icd.semaphores);
}

static VkResult VKAPI_CALL CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator,
                                           VkQueryPool* pool) {
    *pool = (VkQueryPool)(uintptr_t)0x5000;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks* allocator) {
}

static void VKAPI_CALL ResetQueryPool(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count) {
}

static void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t first, uint32_t count) {
}

static VkResult VKAPI_CALL GetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void* data,
                                               VkDeviceSize stride, VkQueryResultFlags flags) {
    VkResult r = play(&fake_icd.queries);
    if (r == VK_SUCCESS && data && size >= 4) memset(data, 0, size);
    return r;
}

static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* info) {
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    fake_icd.ends++;
    if (fake_icd.endFault) return *(volatile int*)fake_icd.endFault;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    return VK_SUCCESS;
}

static VkResult createPipelines(uint32_t count, VkPipeline* pipelines) {
    fake_icd.pipelines++;
    if (fake_icd.pipelineFault) return *(volatile int*)fake_icd.pipelineFault;
    for (uint32_t i = 0; i < count; i++) pipelines[i] = (VkPipeline)(uintptr_t)(0x7000 + i);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    return createPipelines(count, pipelines);
}

static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                   const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                                   VkPipeline* pipelines) {
    return createPipelines(count, pipelines);
}

static void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
}

#define F(name, fn) {"vk" name, (PFN_vkVoidFunction)fn}
static const struct {
    const char* name;
    PFN_vkVoidFunction fn;
} instanceFns[] = {
    F("CreateInstance", CreateInstance), F("DestroyInstance", DestroyInstance), F("EnumerateInstanceExtensionProperties", EnumerateExtensions),
    F("EnumeratePhysicalDevices", EnumeratePhysicalDevices), F("GetPhysicalDeviceProperties", GetPhysicalDeviceProperties),
    F("GetPhysicalDeviceFeatures", GetPhysicalDeviceFeatures), F("GetPhysicalDeviceQueueFamilyProperties", GetQueueFamilies),
    F("GetPhysicalDeviceMemoryProperties", GetMemoryProperties), F("GetPhysicalDeviceMemoryProperties2", GetMemoryProperties2),
    F("EnumerateDeviceExtensionProperties", EnumerateDeviceExtensions), F("CreateDevice", CreateDevice),
}, deviceFns[] = {
    F("DestroyDevice", DestroyDevice), F("GetDeviceQueue", GetDeviceQueue), F("QueueSubmit", QueueSubmit), F("QueueSubmit2", QueueSubmit2),
    F("QueueWaitIdle", QueueWaitIdle), F("DeviceWaitIdle", DeviceWaitIdle), F("WaitForFences", WaitForFences),
    F("WaitSemaphores", WaitSemaphores), F("CreateQueryPool", CreateQueryPool), F("DestroyQueryPool", DestroyQueryPool),
    F("ResetQueryPool", ResetQueryPool), F("CmdResetQueryPool", CmdResetQueryPool), F("GetQueryPoolResults", GetQueryPoolResults),
    F("BeginCommandBuffer", BeginCommandBuffer), F("EndCommandBuffer", EndCommandBuffer), F("ResetCommandBuffer", ResetCommandBuffer),
    F("CreateComputePipelines", CreateComputePipelines), F("CreateGraphicsPipelines", CreateGraphicsPipelines),
    F("DestroyPipeline", DestroyPipeline),
};

EXPORT PFN_vkVoidFunction fake_icd_device_proc(const char* name) {
    for (size_t i = 0; i < sizeof(deviceFns) / sizeof(deviceFns[0]); i++)
        if (!strcmp(name, deviceFns[i].name)) return deviceFns[i].fn;
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    return fake_icd_device_proc(name);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (*version > 5) *version = 5;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    for (size_t i = 0; i < sizeof(instanceFns) / sizeof(instanceFns[0]); i++)
        if (!strcmp(name, instanceFns[i].name)) return instanceFns[i].fn;
    return instance ? fake_icd_device_proc(name) : NULL;
}
