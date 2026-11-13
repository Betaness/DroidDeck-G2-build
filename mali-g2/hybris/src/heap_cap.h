/*
 * [mali-g2] The device-local heap size reported on devices with 12800 MB of RAM or less (guards.c).
 * Behaviour after MaliBridge's default heap cap (re-implemented, not copied). r56p0 has no
 * VK_EXT_memory_budget and one device-local heap the size of the RAM; DXGI reports that heap as
 * dedicated video memory and games size their texture pools from it. DXVK enforces no heap size
 * on an integrated GPU (dxvk_memory.cpp, enforceBudget): over the reported size it frees empty
 * chunks sooner and defragments, and refuses no allocation. No Vulkan calls: tests/guards_test.c
 * includes this file alone.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_HYBRIS_HEAP_CAP_H
#define DROIDDECK_HYBRIS_HEAP_CAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#define HEAPCAP_RAM_MAX_MB 12800u
#define HEAPCAP_MIN_MB 2048u
#define HEAPCAP_MAX_MB 4096u

/* The default cap in MB for `ramMb` of RAM, 0 for none (more RAM, or RAM unknown). */
static inline uint32_t heapcap_default_mb(uint64_t ramMb) {
    if (!ramMb || ramMb > HEAPCAP_RAM_MAX_MB) return 0;
    uint64_t cap = (ramMb / 2) & ~(uint64_t)511;
    return cap < HEAPCAP_MIN_MB ? HEAPCAP_MIN_MB : cap > HEAPCAP_MAX_MB ? HEAPCAP_MAX_MB : (uint32_t)cap;
}

/* Unchanged without a cap or when free memory is unknown (0). */
static inline uint32_t heapcap_follow_free(uint32_t capMb, uint64_t freeMb) {
    if (!capMb || !freeMb) return capMb;
    uint64_t mb = freeMb & ~(uint64_t)511;
    if (mb < HEAPCAP_MIN_MB) mb = HEAPCAP_MIN_MB;
    return mb < capMb ? (uint32_t)mb : capMb;
}

/* The value of `key` ("MemTotal", "MemAvailable") in /proc/meminfo's text, in kB; 0 when absent. */
static inline uint64_t heapcap_meminfo_kb(const char* text, const char* key) {
    size_t n = strlen(key);
    for (const char* line = text; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (strncmp(line, key, n) || line[n] != ':') continue;
        return strtoull(line + n + 1, NULL, 10);
    }
    return 0;
}

/* A number of MB from the environment: the value, or -1 when it is not a plain number. */
static inline int64_t heapcap_parse_mb(const char* text) {
    if (!text || *text < '0' || *text > '9') return -1;
    char* end;
    unsigned long long mb = strtoull(text, &end, 10);
    if (*end || mb > 0xffffffffull) return -1;
    return (int64_t)mb;
}

/* The usage stays the driver's, so memory over the cap reads as over budget. Returns a mask of the
 * heaps changed. */
static inline uint32_t heapcap_apply(VkPhysicalDeviceMemoryProperties* memory, VkPhysicalDeviceMemoryBudgetPropertiesEXT* budget,
                                     uint64_t capBytes) {
    uint32_t changed = 0;
    if (!capBytes) return 0;
    for (uint32_t h = 0; h < memory->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        VkMemoryHeap* heap = &memory->memoryHeaps[h];
        if ((heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && heap->size > capBytes) {
            heap->size = capBytes;
            changed |= 1u << h;
        }
        if (budget && budget->heapBudget[h] > heap->size) budget->heapBudget[h] = heap->size;
    }
    return changed;
}

#endif
