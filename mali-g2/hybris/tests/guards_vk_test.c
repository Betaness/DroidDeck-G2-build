/*
 * guards.c through the loader and the device's driver (tests/guards_vk_test.sh runs it under several
 * settings and reads the log lines): the heap sizes reported (GV_HEAP_MB: the cap expected, 0: the
 * driver's sizes); fence and timeline waits that time out, also (GV_STORM_US=<us>) with SIGALRM at
 * that interval and no SA_RESTART (never a result a wait may not return, never before the
 * deadline); an occlusion query held "not ready" by a submission that waits on a timeline semaphore
 * the host signals 700 ms later (GV_VISIBLE=1: answered visible after 500 ms); a compute pipeline
 * and command buffers (the crash guard's calls). Prints GV-PASS or GV-FAIL; GV-FAIL after 60 s in
 * a call that does not return. Adreno's driver restarts an interrupted fence wait with its whole
 * timeout, so a storm faster than the timeout never ends there.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "guards_comp.h"

static int failures;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "GV FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static volatile int alarms;

static void onAlarm(int sig) {
    alarms++;
}

/* SIGALRM every `us` without SA_RESTART (0: none): blocking calls see EINTR. */
static void storm(long us) {
    struct sigaction sa = {0};
    sa.sa_handler = onAlarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval t = {{us / 1000000, us % 1000000}, {us / 1000000, us % 1000000}};
    setitimer(ITIMER_REAL, &t, NULL);
}

static void* watchdog(void* arg) {
    sleep(60);
    printf("GV-FAIL: a call did not return in 60 s\n");
    fflush(stdout);
    _exit(2);
}

static void testHeaps(VkPhysicalDevice physical) {
    long cap = getenv("GV_HEAP_MB") ? atol(getenv("GV_HEAP_MB")) : 0;
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(physical, &m);
    VkPhysicalDeviceMemoryProperties2 m2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    vkGetPhysicalDeviceMemoryProperties2(physical, &m2);
    for (uint32_t h = 0; h < m.memoryHeapCount; h++) {
        printf("heap %u: %llu MB%s\n", h, (unsigned long long)(m.memoryHeaps[h].size >> 20),
               m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device-local" : "");
        CHECK(m.memoryHeaps[h].size == m2.memoryProperties.memoryHeaps[h].size, "both queries agree on heap %u", h);
        if (cap && (m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            CHECK(m.memoryHeaps[h].size <= (VkDeviceSize)cap << 20, "heap %u under the cap", h);
    }
}

static void testWaits(void) {
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    VkSemaphore timeline;
    vkCreateSemaphore(dev, &sci, NULL, &timeline);
    uint64_t value = 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &value};

    long stormUs = getenv("GV_STORM_US") ? atol(getenv("GV_STORM_US")) : 0;
    for (int round = 0; round < (stormUs > 0 ? 2 : 1); round++) {
        storm(round ? stormUs : 0);
        double t = now();
        VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 300000000ull);
        double took = now() - t;
        CHECK(r == VK_TIMEOUT && took >= 0.298, "fence wait (storm %d): %d after %.3f s", round, r, took);
        t = now();
        r = vkWaitSemaphores(dev, &wi, 300000000ull);
        took = now() - t;
        CHECK(r == VK_TIMEOUT && took >= 0.298, "timeline wait (storm %d): %d after %.3f s", round, r, took);
        CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS, "queue idle");
        CHECK(vkDeviceWaitIdle(dev) == VK_SUCCESS, "device idle");
        printf("waits, storm %d: %d signals\n", round, alarms);
    }
    storm(0);
    vkDestroySemaphore(dev, timeline, NULL);
    vkDestroyFence(dev, fence, NULL);
}

static void testQuery(void) {
    bool visible = getenv("GV_VISIBLE") && atoi(getenv("GV_VISIBLE"));
    VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 4};
    VkQueryPool pool;
    CHECK(vkCreateQueryPool(dev, &qci, NULL, &pool) == VK_SUCCESS, "query pool");
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool cmdPool;
    vkCreateCommandPool(dev, &pci, NULL, &cmdPool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmdPool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdResetQueryPool(cmd, pool, 0, 4);
    vkCmdBeginQuery(cmd, pool, 0, 0);
    vkCmdEndQuery(cmd, pool, 0);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS, "end command buffer");

    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    VkSemaphore gate;
    vkCreateSemaphore(dev, &sci, NULL, &gate);
    uint64_t one = 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1,
                                        .pWaitSemaphoreValues = &one};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .waitSemaphoreCount = 1, .pWaitSemaphores = &gate,
                       .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS, "submit");

    double start = now(), answeredAt = 0;
    int notReady = 0;
    uint64_t result[2] = {0};
    while (now() - start < 0.7) {
        VkResult r = vkGetQueryPoolResults(dev, pool, 0, 1, sizeof(result), result, sizeof(result),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r == VK_NOT_READY) {
            notReady++;
        } else if (r == VK_SUCCESS && !answeredAt) {
            answeredAt = now() - start;
            CHECK(result[0] == 1 && result[1] == 1, "answered visible: %llu samples, available %llu", (unsigned long long)result[0],
                  (unsigned long long)result[1]);
        }
        usleep(5000);
    }
    if (visible) CHECK(answeredAt >= 0.49 && answeredAt < 0.65, "answered visible at %.3f s", answeredAt);
    else CHECK(!answeredAt, "the driver's answers only (answered at %.3f s)", answeredAt);
    printf("query: %d not-ready polls, %s\n", notReady, answeredAt ? "answered visible" : "never answered");

    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    vkSignalSemaphore(dev, &signal);
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS, "the submission finishes");
    VkResult r = vkGetQueryPoolResults(dev, pool, 0, 1, sizeof(result), result, sizeof(result),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    CHECK(r == VK_SUCCESS && result[0] == 0 && result[1] == 1, "the driver's own result: %d, %llu samples", r,
          (unsigned long long)result[0]);

    vkDestroyFence(dev, fence, NULL);
    vkDestroySemaphore(dev, gate, NULL);
    vkDestroyCommandPool(dev, cmdPool, NULL);
    vkDestroyQueryPool(dev, pool, NULL);
}

static void testPipeline(void) {
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(guards_comp),
                                    .pCode = guards_comp};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(dev, &mci, NULL, &module) == VK_SUCCESS, "shader module");
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    vkCreatePipelineLayout(dev, &lci, NULL, &layout);
    VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
                                       .layout = layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline) == VK_SUCCESS && pipeline, "compute pipeline");
    vkDestroyPipeline(dev, pipeline, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyShaderModule(dev, module, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t dog;
    pthread_create(&dog, NULL, watchdog, NULL);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "guards_vk_test", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    if (vkCreateInstance(&ici, NULL, &instance)) {
        printf("GV-FAIL: no instance\n");
        return 1;
    }
    uint32_t n = 1;
    VkPhysicalDevice physical;
    if (vkEnumeratePhysicalDevices(instance, &n, &physical) < 0 || !n) {
        printf("GV-FAIL: no device\n");
        return 1;
    }
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties props[16];
    if (families > 16) families = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, props);
    for (family = 0; family < families && !(props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++) {
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1,
                                   .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    if (vkCreateDevice(physical, &dci, NULL, &dev)) {
        printf("GV-FAIL: no device with timeline semaphores\n");
        return 1;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    testHeaps(physical);
    testWaits();
    testQuery();
    testPipeline();
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    if (failures) printf("GV-FAIL (%d)\n", failures);
    else printf("GV-PASS\n");
    return failures ? 1 : 0;
}
