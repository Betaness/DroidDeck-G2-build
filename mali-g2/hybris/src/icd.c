/*
 * Loader entry points and the instance / device bookkeeping; see icd.h.
 *
 * Environment:
 *   DROIDDECK_HYBRIS_NEXT_ICD   libhybris's ICD to load (default: lib/libhybris-vulkan-icd.so.0 next
 *                               to this library). It needs libhybris-common.so.1 in LD_PRELOAD (its
 *                               static TLS), otherwise it does not load and this ICD reports no device.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include "icd.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef VkResult(VKAPI_PTR* PFN_Negotiate)(uint32_t*);
typedef PFN_vkVoidFunction(VKAPI_PTR* PFN_GetPhysicalDeviceProcAddr)(VkInstance, const char*);

static pthread_once_t nextOnce = PTHREAD_ONCE_INIT;
static struct {
    PFN_Negotiate negotiate;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_GetPhysicalDeviceProcAddr gpdpa;
} next;

static void loadNext(void) {
    const char* path = getenv("DROIDDECK_HYBRIS_NEXT_ICD");
    char buf[PATH_MAX];
    Dl_info info;
    if ((!path || !*path) && dladdr((void*)loadNext, &info) && info.dli_fname && strrchr(info.dli_fname, '/')) {
        int dir = (int)(strrchr(info.dli_fname, '/') - info.dli_fname);
        snprintf(buf, sizeof(buf), "%.*s/lib/libhybris-vulkan-icd.so.0", dir, info.dli_fname);
        path = buf;
    }
    if (!path || !*path) path = "libhybris-vulkan-icd.so.0";
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "hybris-icd: %s (libhybris-common.so.1 must be in LD_PRELOAD)\n", dlerror());
        return;
    }
    next.negotiate = (PFN_Negotiate)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    next.gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    next.gpdpa = (PFN_GetPhysicalDeviceProcAddr)dlsym(lib, "vk_icdGetPhysicalDeviceProcAddr");
    if (!next.negotiate || !next.gipa) {
        fprintf(stderr, "hybris-icd: %s is not a Vulkan ICD\n", path);
        next.gipa = NULL;
    }
}

static bool ready(void) {
    pthread_once(&nextOnce, loadNext);
    return next.gipa != NULL;
}

int icd_env_int(const char* name, int defaultValue) {
    const char* value = getenv(name);
    return value && *value ? atoi(value) : defaultValue;
}

/* ---- registries ---- */

typedef struct Physical {
    VkPhysicalDevice handle;
    Instance* instance;
    struct Physical* next;
} Physical;

static pthread_mutex_t registryLock = PTHREAD_MUTEX_INITIALIZER;
static Instance* instances;
static Physical* physicals;
static Device* devices;

Instance* icd_instance(VkInstance instance) {
    pthread_mutex_lock(&registryLock);
    Instance* found = instances;
    while (found && found->handle != instance) found = found->next;
    pthread_mutex_unlock(&registryLock);
    return found;
}

Instance* icd_instance_of(VkPhysicalDevice physical) {
    pthread_mutex_lock(&registryLock);
    Physical* found = physicals;
    while (found && found->handle != physical) found = found->next;
    Instance* instance = found ? found->instance : NULL;
    pthread_mutex_unlock(&registryLock);
    return instance;
}

Device* icd_device(VkDevice device) {
    pthread_mutex_lock(&registryLock);
    Device* found = devices;
    while (found && found->handle != device) found = found->next;
    pthread_mutex_unlock(&registryLock);
    return found;
}

Device* icd_device_of_queue(VkQueue queue, uint32_t* family) {
    Device* found = NULL;
    pthread_mutex_lock(&registryLock);
    for (Device* d = devices; d && !found; d = d->next) {
        for (uint32_t i = 0; i < d->queueCount; i++) {
            if (d->queues[i] == queue) {
                found = d;
                if (family) *family = d->queueFamilies[i];
                break;
            }
        }
        if (!found && d->internalQueue == queue) {
            found = d;
            if (family) *family = d->internalFamily;
        }
    }
    pthread_mutex_unlock(&registryLock);
    return found;
}

static void rememberPhysical(Instance* instance, VkPhysicalDevice handle) {
    pthread_mutex_lock(&registryLock);
    Physical* found = physicals;
    while (found && found->handle != handle) found = found->next;
    if (!found && (found = calloc(1, sizeof(*found)))) {
        found->handle = handle;
        found->next = physicals;
        physicals = found;
    }
    if (found) found->instance = instance;
    pthread_mutex_unlock(&registryLock);
}

/* ---- extension lists ---- */

static bool listed(const VkExtensionProperties* list, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++) if (!strcmp(list[i].extensionName, name)) return true;
    return false;
}

static VkResult copyOut(const VkExtensionProperties* all, uint32_t total, uint32_t* count, VkExtensionProperties* properties) {
    if (!properties) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t copied = *count < total ? *count : total;
    memcpy(properties, all, copied * sizeof(*properties));
    *count = copied;
    return copied < total ? VK_INCOMPLETE : VK_SUCCESS;
}

/* The driver's list with room for `extra` more entries; NULL on failure. */
static VkExtensionProperties* driverDeviceExtensions(Instance* instance, VkPhysicalDevice physical, uint32_t extra, uint32_t* count) {
    *count = 0;
    if (instance->EnumerateDeviceExtensionProperties(physical, NULL, count, NULL) != VK_SUCCESS) return NULL;
    VkExtensionProperties* list = calloc(*count + extra + 1, sizeof(*list));
    if (list && instance->EnumerateDeviceExtensionProperties(physical, NULL, count, list) < 0) {
        free(list);
        list = NULL;
    }
    return list;
}

static bool placedAvailable(const VkExtensionProperties* list, uint32_t count) {
    if (!placed_wanted() || listed(list, count, placed_extension)) return false;
    for (uint32_t i = 0; i < placed_need_count; i++)
        if (!listed(list, count, placed_needs[i])) return false;
    return true;
}

bool icd_placed_available(Instance* instance, VkPhysicalDevice physical) {
    uint32_t n;
    VkExtensionProperties* all = driverDeviceExtensions(instance, physical, 0, &n);
    bool available = all && placedAvailable(all, n);
    free(all);
    return available;
}

static const char* const DIVISOR_EXT = "VK_EXT_vertex_attribute_divisor";
static const char* const DIVISOR_KHR = "VK_KHR_vertex_attribute_divisor";

static VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* layer, uint32_t* count, VkExtensionProperties* properties) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)next.gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (layer) return enumerate(layer, count, properties);
    uint32_t n = 0;
    VkResult result = enumerate(NULL, &n, NULL);
    if (result != VK_SUCCESS) return result;
    VkExtensionProperties* all = calloc(n + wsi_instance_extension_count + 1, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = enumerate(NULL, &n, all);
    if (result < 0) {
        free(all);
        return result;
    }
    for (uint32_t i = 0; i < wsi_instance_extension_count; i++)
        if (!listed(all, n, wsi_instance_extensions[i].extensionName)) all[n++] = wsi_instance_extensions[i];
    result = copyOut(all, n, count, properties);
    free(all);
    return result;
}

static VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char* layer, uint32_t* count,
                                                               VkExtensionProperties* properties) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return instance->EnumerateDeviceExtensionProperties(physical, layer, count, properties);
    uint32_t n;
    VkExtensionProperties* all = driverDeviceExtensions(instance, physical, 3, &n);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* The Android loader's own business (its swapchain is built on it), not the application's. */
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(all[i].extensionName, "VK_ANDROID_native_buffer")) {
            memmove(&all[i], &all[i + 1], (n - i - 1) * sizeof(*all));
            n--;
            break;
        }
    }
    if (!listed(all, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        snprintf(all[n].extensionName, sizeof(all[n].extensionName), "%s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        all[n++].specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
    }
    /* Mali has only the KHR divisor extension; zink (Mesa 26) looks for the EXT name and without it
     * leaves out GL_ARB_instanced_arrays. Same structures; vkCreateDevice turns it back into KHR. */
    if (listed(all, n, DIVISOR_KHR) && !listed(all, n, DIVISOR_EXT)) {
        snprintf(all[n].extensionName, sizeof(all[n].extensionName), "%s", DIVISOR_EXT);
        all[n++].specVersion = 3;
    }
    if (placedAvailable(all, n)) {
        snprintf(all[n].extensionName, sizeof(all[n].extensionName), "%s", placed_extension);
        all[n++].specVersion = 1;
    }
    VkResult result = copyOut(all, n, count, properties);
    free(all);
    return result;
}

/* ---- instance ---- */

static bool isWsiInstanceExtension(const char* name) {
    for (uint32_t i = 0; i < wsi_instance_extension_count; i++)
        if (!strcmp(name, wsi_instance_extensions[i].extensionName)) return true;
    return false;
}

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* out) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    Instance* instance = calloc(1, sizeof(*instance));
    const char** names = calloc(info->enabledExtensionCount + 1, sizeof(*names));
    if (!instance || !names) {
        free(instance);
        free(names);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
        if (!isWsiInstanceExtension(info->ppEnabledExtensionNames[i])) names[count++] = info->ppEnabledExtensionNames[i];
    VkInstanceCreateInfo copy = *info;
    copy.enabledExtensionCount = count;
    copy.ppEnabledExtensionNames = count ? names : NULL;
    VkResult result = create(&copy, allocator, out);
    free(names);
    if (result != VK_SUCCESS) {
        free(instance);
        return result;
    }
    instance->handle = *out;
#define X(name) instance->name = (PFN_vk##name)next.gipa(*out, "vk" #name);
    INSTANCE_FUNCS(X)
#undef X
    if (!instance->EnumeratePhysicalDeviceGroups)
        instance->EnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups)next.gipa(*out, "vkEnumeratePhysicalDeviceGroupsKHR");
    if (!instance->GetPhysicalDeviceFeatures2)
        instance->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)next.gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
    const char* engine = info->pApplicationInfo ? info->pApplicationInfo->pEngineName : NULL;
    instance->vkd3d = engine && !strcmp(engine, "vkd3d");
    pthread_mutex_lock(&registryLock);
    instance->next = instances;
    instances = instance;
    pthread_mutex_unlock(&registryLock);
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyInstance(VkInstance handle, const VkAllocationCallbacks* allocator) {
    if (!handle) return;
    pthread_mutex_lock(&registryLock);
    Instance** link = &instances;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    Instance* instance = *link;
    if (instance) *link = instance->next;
    for (Physical** p = &physicals; *p;) {
        if ((*p)->instance == instance) {
            Physical* gone = *p;
            *p = gone->next;
            free(gone);
        }
        else p = &(*p)->next;
    }
    pthread_mutex_unlock(&registryLock);
    if (!instance) return;
    instance->DestroyInstance(handle, allocator);
    free(instance);
}

static VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance handle, uint32_t* count, VkPhysicalDevice* physicals) {
    Instance* instance = icd_instance(handle);
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = instance->EnumeratePhysicalDevices(handle, count, physicals);
    if (physicals && result >= 0)
        for (uint32_t i = 0; i < *count; i++) rememberPhysical(instance, physicals[i]);
    return result;
}

static VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(VkInstance handle, uint32_t* count, VkPhysicalDeviceGroupProperties* groups) {
    Instance* instance = icd_instance(handle);
    if (!instance || !instance->EnumeratePhysicalDeviceGroups) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = instance->EnumeratePhysicalDeviceGroups(handle, count, groups);
    if (groups && result >= 0)
        for (uint32_t i = 0; i < *count; i++)
            for (uint32_t j = 0; j < groups[i].physicalDeviceCount; j++) rememberPhysical(instance, groups[i].physicalDevices[j]);
    return result;
}

/* ---- device ---- */

/* Enabled with VK_KHR_swapchain when the driver has them: AHardwareBuffer swapchain images, and sync
 * fds for the compositor's acquire and release fences. */
static const char* const swapchainSupport[] = {
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_external_memory",
    "VK_EXT_queue_family_foreign",
    "VK_KHR_dedicated_allocation",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_bind_memory2",
    "VK_KHR_maintenance1",
    "VK_KHR_external_fence",
    "VK_KHR_external_fence_fd",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_fd",
};

static bool named(const char* const* names, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++) if (!strcmp(names[i], name)) return true;
    return false;
}

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                        VkDevice* out) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t driverCount;
    VkExtensionProperties* driverList = driverDeviceExtensions(instance, physical, 0, &driverCount);
    const char** names = calloc(info->enabledExtensionCount + ARRAY_LEN(swapchainSupport) + placed_need_count + 1, sizeof(*names));
    Device* device = calloc(1, sizeof(*device));
    uint32_t familyCount = 0;
    instance->GetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, NULL);
    VkQueueFamilyProperties* families = calloc(familyCount + 1, sizeof(*families));
    VkDeviceQueueCreateInfo* queueInfos = calloc(info->queueCreateInfoCount + 1, sizeof(*queueInfos));
    float* priorities = NULL;
    if (!driverList || !names || !device || !families || !queueInfos) {
        free(driverList); free(names); free(device); free(families); free(queueInfos);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    instance->GetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families);

    uint32_t appCount = 0;
    bool placed = false, ownPlaced = !listed(driverList, driverCount, placed_extension);
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char* name = info->ppEnabledExtensionNames[i];
        if (!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            device->swapchainEnabled = true;
            continue;
        }
        if (ownPlaced && !strcmp(name, placed_extension)) {
            placed = true;
            continue;
        }
        if (!strcmp(name, DIVISOR_EXT)) name = DIVISOR_KHR;
        if (!named(names, appCount, name)) names[appCount++] = name;
    }
    uint32_t count = appCount;
    if (device->swapchainEnabled)
        for (uint32_t i = 0; i < ARRAY_LEN(swapchainSupport); i++)
            if (listed(driverList, driverCount, swapchainSupport[i]) && !named(names, count, swapchainSupport[i]))
                names[count++] = swapchainSupport[i];
    if (placed)
        for (uint32_t i = 0; i < placed_need_count; i++)
            if (!named(names, count, placed_needs[i])) names[count++] = placed_needs[i];

    /* A queue of our own in the first graphics family, when the application leaves one free. */
    uint32_t graphics = UINT32_MAX;
    for (uint32_t f = 0; f < familyCount && graphics == UINT32_MAX; f++)
        if (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphics = f;
    memcpy(queueInfos, info->pQueueCreateInfos, info->queueCreateInfoCount * sizeof(*queueInfos));
    uint32_t queueInfoCount = info->queueCreateInfoCount, ownIndex = UINT32_MAX;
    if (device->swapchainEnabled && graphics != UINT32_MAX) {
        int slot = -1;
        for (uint32_t i = 0; i < queueInfoCount; i++)
            if (queueInfos[i].queueFamilyIndex == graphics && !queueInfos[i].flags) slot = (int)i;
        uint32_t used = slot >= 0 ? queueInfos[slot].queueCount : 0;
        if (used < families[graphics].queueCount && (priorities = calloc(used + 1, sizeof(float)))) {
            if (slot < 0) {
                slot = (int)queueInfoCount++;
                queueInfos[slot] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                             .queueFamilyIndex = graphics};
            }
            else memcpy(priorities, queueInfos[slot].pQueuePriorities, used * sizeof(float));
            priorities[used] = 0.5f;
            queueInfos[slot].queueCount = used + 1;
            queueInfos[slot].pQueuePriorities = priorities;
            ownIndex = used;
        }
    }

    /* Features reported only as emulated (mali_fixes.c) are not asked of the driver. The chained
     * VkPhysicalDeviceFeatures2 is the application's: changed for the call and put back after it. */
    VkPhysicalDeviceFeatures enabled = {0}, chainedSaved = {0};
    VkPhysicalDeviceFeatures2* chained = NULL;
    for (const VkBaseInStructure* s = info->pNext; s && !chained; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) chained = (VkPhysicalDeviceFeatures2*)s;
    if (info->pEnabledFeatures) {
        enabled = *info->pEnabledFeatures;
        mali_drop_emulated_features(instance, physical, &enabled);
    }
    else if (chained) {
        chainedSaved = chained->features;
        mali_drop_emulated_features(instance, physical, &chained->features);
    }

    /* The driver does not have the placed-map features: unlinked for the call, put back after it. */
    VkBaseOutStructure* placedPrev = NULL;
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)info; ownPlaced && s->pNext; s = s->pNext) {
        if (s->pNext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) {
            placedPrev = s;
            break;
        }
    }
    VkBaseOutStructure* placedFeatures = placedPrev ? placedPrev->pNext : NULL;
    if (placedFeatures) placedPrev->pNext = placedFeatures->pNext;

    VkDeviceCreateInfo copy = *info;
    if (info->pEnabledFeatures) copy.pEnabledFeatures = &enabled;
    copy.enabledExtensionCount = count;
    copy.ppEnabledExtensionNames = count ? names : NULL;
    copy.queueCreateInfoCount = queueInfoCount;
    copy.pQueueCreateInfos = queueInfos;
    VkResult result = instance->CreateDevice(physical, &copy, allocator, out);
    if (result != VK_SUCCESS && (count != appCount || ownIndex != UINT32_MAX)) {
        fprintf(stderr, "hybris-icd: vkCreateDevice with the swapchain's extensions and queue failed (%d), without them\n", result);
        copy.enabledExtensionCount = appCount;
        copy.ppEnabledExtensionNames = appCount ? names : NULL;
        copy.queueCreateInfoCount = info->queueCreateInfoCount;
        copy.pQueueCreateInfos = info->pQueueCreateInfos;
        count = appCount;
        ownIndex = UINT32_MAX;
        result = instance->CreateDevice(physical, &copy, allocator, out);
    }
    if (chained && !info->pEnabledFeatures) chained->features = chainedSaved;
    if (placedFeatures) placedPrev->pNext = placedFeatures;
    if (result != VK_SUCCESS) {
        free(driverList); free(names); free(device); free(families); free(queueInfos); free(priorities);
        return result;
    }

    device->handle = *out;
    device->physical = physical;
    device->instance = instance;
    device->placed = placed;
    for (uint32_t i = 0; i < placed_need_count; i++) device->placed = device->placed && named(names, count, placed_needs[i]);
#define X(name) device->name = (PFN_vk##name)instance->GetDeviceProcAddr(*out, "vk" #name);
    DEVICE_FUNCS(X)
#undef X
    device->ahb = named(names, count, "VK_ANDROID_external_memory_android_hardware_buffer") &&
                  device->GetAndroidHardwareBufferPropertiesANDROID;
    device->syncFd = named(names, count, "VK_KHR_external_fence_fd") && named(names, count, "VK_KHR_external_semaphore_fd") &&
                     device->GetFenceFdKHR && device->ImportFenceFdKHR && device->ImportSemaphoreFdKHR;
    instance->GetPhysicalDeviceMemoryProperties(physical, &device->memory);
    pthread_mutex_init(&device->queueLock, NULL);
    mali_device_created(device);
    placed_device_created(device);
    if (placed && !device->placed)
        fprintf(stderr, "hybris-icd: VK_EXT_map_memory_placed asked for, but the device was created without dma-buf import\n");

    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo* q = &info->pQueueCreateInfos[i];
        for (uint32_t j = 0; j < q->queueCount && device->queueCount < MAX_QUEUES; j++) {
            VkQueue queue = VK_NULL_HANDLE;
            if (!q->flags) device->GetDeviceQueue(*out, q->queueFamilyIndex, j, &queue);
            else if (device->GetDeviceQueue2) {
                VkDeviceQueueInfo2 qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .flags = q->flags,
                                         .queueFamilyIndex = q->queueFamilyIndex, .queueIndex = j};
                device->GetDeviceQueue2(*out, &qi, &queue);
            }
            if (!queue) continue;
            device->queueFamilies[device->queueCount] = q->queueFamilyIndex;
            device->queues[device->queueCount++] = queue;
        }
    }
    if (ownIndex != UINT32_MAX) {
        device->GetDeviceQueue(*out, graphics, ownIndex, &device->internalQueue);
        device->internalFamily = graphics;
    }
    if (!device->internalQueue) {
        for (uint32_t i = 0; i < device->queueCount && !device->internalQueue; i++) {
            if (device->queueFamilies[i] < familyCount && (families[device->queueFamilies[i]].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                device->internalQueue = device->queues[i];
                device->internalFamily = device->queueFamilies[i];
            }
        }
        if (!device->internalQueue && device->queueCount) {
            device->internalQueue = device->queues[0];
            device->internalFamily = device->queueFamilies[0];
        }
        device->lockQueue = device->swapchainEnabled;
    }
    if (device->swapchainEnabled)
        fprintf(stderr, "hybris-icd: device with swapchains: AHardwareBuffer images %s, sync fds %s, %s queue for acquire\n",
                device->ahb ? "yes" : "no", device->syncFd ? "yes" : "no", device->lockQueue ? "a shared" : "its own");
    free(driverList); free(names); free(families); free(queueInfos); free(priorities);

    pthread_mutex_lock(&registryLock);
    device->next = devices;
    devices = device;
    pthread_mutex_unlock(&registryLock);
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyDevice(VkDevice handle, const VkAllocationCallbacks* allocator) {
    if (!handle) return;
    Device* device = icd_device(handle);
    if (!device) return;
    wsi_device_destroyed(device);
    mali_device_destroyed(device);
    pthread_mutex_lock(&registryLock);
    Device** link = &devices;
    while (*link && *link != device) link = &(*link)->next;
    if (*link) *link = device->next;
    pthread_mutex_unlock(&registryLock);
    device->DestroyDevice(handle, allocator);
    placed_device_destroyed(device);
    pthread_mutex_destroy(&device->queueLock);
    free(device);
}

/* ---- the acquire queue ---- */

static bool locks(Device* device, VkQueue queue) {
    return queue == device->internalQueue;
}

VkResult icd_queue_submit(Device* device, VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (!locks(device, queue)) return device->QueueSubmit(queue, count, submits, fence);
    pthread_mutex_lock(&device->queueLock);
    VkResult result = device->QueueSubmit(queue, count, submits, fence);
    pthread_mutex_unlock(&device->queueLock);
    return result;
}

VkResult icd_queue_wait_idle(Device* device, VkQueue queue) {
    if (!locks(device, queue)) return device->QueueWaitIdle(queue);
    pthread_mutex_lock(&device->queueLock);
    VkResult result = device->QueueWaitIdle(queue);
    pthread_mutex_unlock(&device->queueLock);
    return result;
}

static VkResult VKAPI_CALL LockedQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    Device* device = icd_device_of_queue(queue, NULL);
    return device ? icd_queue_submit(device, queue, count, submits, fence) : VK_ERROR_DEVICE_LOST;
}

static VkResult lockedSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, bool khr) {
    Device* device = icd_device_of_queue(queue, NULL);
    if (!device) return VK_ERROR_DEVICE_LOST;
    PFN_vkQueueSubmit2 submit = khr && device->QueueSubmit2KHR ? device->QueueSubmit2KHR : device->QueueSubmit2;
    if (!locks(device, queue)) return submit(queue, count, submits, fence);
    pthread_mutex_lock(&device->queueLock);
    VkResult result = submit(queue, count, submits, fence);
    pthread_mutex_unlock(&device->queueLock);
    return result;
}

static VkResult VKAPI_CALL LockedQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    return lockedSubmit2(queue, count, submits, fence, false);
}

static VkResult VKAPI_CALL LockedQueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    return lockedSubmit2(queue, count, submits, fence, true);
}

static VkResult VKAPI_CALL LockedQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* infos, VkFence fence) {
    Device* device = icd_device_of_queue(queue, NULL);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (!locks(device, queue)) return device->QueueBindSparse(queue, count, infos, fence);
    pthread_mutex_lock(&device->queueLock);
    VkResult result = device->QueueBindSparse(queue, count, infos, fence);
    pthread_mutex_unlock(&device->queueLock);
    return result;
}

static VkResult VKAPI_CALL LockedQueueWaitIdle(VkQueue queue) {
    Device* device = icd_device_of_queue(queue, NULL);
    return device ? icd_queue_wait_idle(device, queue) : VK_ERROR_DEVICE_LOST;
}

/* Waits on every queue, ours included: no acquire may submit meanwhile. */
static VkResult VKAPI_CALL DeviceWaitIdle(VkDevice handle) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(&device->queueLock);
    VkResult result = device->DeviceWaitIdle(handle);
    pthread_mutex_unlock(&device->queueLock);
    return result;
}

/* ---- proc lookup ---- */

static PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice handle, const char* name);

static PFN_vkVoidFunction ownDeviceProc(VkDevice handle, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    if (!strcmp(name, "vkDeviceWaitIdle")) return (PFN_vkVoidFunction)DeviceWaitIdle;
    PFN_vkVoidFunction f = wsi_device_proc(name);
    if (!f) f = mali_device_proc(handle, name);
    if (f) return f;
    Device* device = handle ? icd_device(handle) : NULL;
    if ((f = placed_device_proc(device, name))) return f;
    if (device && device->lockQueue) {
        if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)LockedQueueSubmit;
        if (!strcmp(name, "vkQueueSubmit2")) return device->QueueSubmit2 ? (PFN_vkVoidFunction)LockedQueueSubmit2 : NULL;
        if (!strcmp(name, "vkQueueSubmit2KHR")) return device->QueueSubmit2KHR ? (PFN_vkVoidFunction)LockedQueueSubmit2KHR : NULL;
        if (!strcmp(name, "vkQueueBindSparse")) return device->QueueBindSparse ? (PFN_vkVoidFunction)LockedQueueBindSparse : NULL;
        if (!strcmp(name, "vkQueueWaitIdle")) return (PFN_vkVoidFunction)LockedQueueWaitIdle;
    }
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice handle, const char* name) {
    if (!name) return NULL;
    PFN_vkVoidFunction own = ownDeviceProc(handle, name);
    if (own) return own;
    Device* device = icd_device(handle);
    if (device) return device->instance->GetDeviceProcAddr(handle, name);
    pthread_mutex_lock(&registryLock);
    PFN_vkGetDeviceProcAddr any = instances ? instances->GetDeviceProcAddr : NULL;
    pthread_mutex_unlock(&registryLock);
    return any ? any(handle, name) : NULL;
}

static PFN_vkVoidFunction ownPhysicalProc(const char* name) {
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)EnumerateDeviceExtensionProperties;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
    PFN_vkVoidFunction f = wsi_instance_proc(name);
    if (!f) f = mali_instance_proc(name);
    return f;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* name);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (!ready()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    return next.negotiate(version);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    if (!name || !ready()) return NULL;
    if (!strncmp(name, "vkGetPhysicalDevice", 19) || !strcmp(name, "vkEnumerateDeviceExtensionProperties") ||
        !strcmp(name, "vkCreateDevice")) {
        PFN_vkVoidFunction own = ownPhysicalProc(name);
        if (own) return own;
    }
    return next.gpdpa ? next.gpdpa(instance, name) : NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return NULL;
    if (!strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion")) return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    if (!strcmp(name, "vk_icdGetPhysicalDeviceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (!ready()) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)EnumerateInstanceExtensionProperties;
    if (!instance) return next.gipa(instance, name);
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)EnumeratePhysicalDevices;
    if (!strcmp(name, "vkEnumeratePhysicalDeviceGroups") || !strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
        return (PFN_vkVoidFunction)EnumeratePhysicalDeviceGroups;
    PFN_vkVoidFunction own = ownPhysicalProc(name);
    if (!own) own = ownDeviceProc(VK_NULL_HANDLE, name);
    return own ? own : next.gipa(instance, name);
}
