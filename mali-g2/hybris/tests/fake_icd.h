/*
 * The driver below the ICD in tests/guards_icd_test.c (tests/fake_icd.c, built as
 * libfake_mali_icd.so: the crash guard sees its functions in a library of the Mali driver). Its
 * state is shared with the test, which links it too.
 */
#ifndef FAKE_ICD_H
#define FAKE_ICD_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define FAKE_QUEUES 40
#define FAKE_HEAP_MB 11377ull

/* The results a function gives, in order; then `otherwise`. */
typedef struct {
    VkResult results[8];
    int count, next, calls;
    VkResult otherwise;
} FakeScript;

typedef struct {
    FakeScript fences, semaphores, queueIdle, deviceIdle, queries;
    int submits, submits2, ends, pipelines, budgets;
    uintptr_t endFault, pipelineFault; /* read by vkEndCommandBuffer / vkCreate*Pipelines when set */
    VkQueue lastIdleQueue;
} FakeIcd;

extern FakeIcd fake_icd;

/* The fake's own vkGetDeviceProcAddr (what the ICD gets for a device function). */
PFN_vkVoidFunction fake_icd_device_proc(const char* name);

#endif
