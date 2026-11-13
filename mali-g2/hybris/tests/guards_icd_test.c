/*
 * guards.c, crash_guard.c and their wiring in icd.c and wsi.c, through this ICD's entry points and
 * a fake driver (tests/fake_icd.c as libfake_mali_icd.so, DROIDDECK_HYBRIS_NEXT_ICD), no GPU and no
 * libhybris. Each setting of the switches runs in a process of its own (the switches are read once),
 * whose log lines are checked: every switch prints one line with its state.
 *   sh tests/guards_test.sh        (builds and runs this with the ICD built from src/)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fake_icd.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

static int failures, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

static FakeIcd* fk;
static PFN_vkVoidFunction (*fakeProc)(const char* name);
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static PFN_vkGetDeviceProcAddr gdpa;
static VkQueue queues[FAKE_QUEUES];
static uintptr_t commandBufferObject;

#define INST(name) ((PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name))
#define DEV(name) ((PFN_vk##name)gdpa(device, "vk" #name))
#define FAKE(name) fakeProc("vk" #name)

static void script(FakeScript* s, VkResult otherwise, int count, ...) {
    memset(s, 0, sizeof(*s));
    s->otherwise = otherwise;
    va_list results;
    va_start(results, count);
    for (int i = 0; i < count; i++) s->results[s->count++] = va_arg(results, VkResult);
    va_end(results);
}

static void setUp(void) {
    void* lib = dlopen(getenv("DROIDDECK_HYBRIS_NEXT_ICD"), RTLD_NOW | RTLD_LOCAL);
    fk = lib ? dlsym(lib, "fake_icd") : NULL;
    fakeProc = lib ? (PFN_vkVoidFunction(*)(const char*))dlsym(lib, "fake_icd_device_proc") : NULL;
    if (!fk || !fakeProc) {
        fprintf(stderr, "FAIL: no fake driver (DROIDDECK_HYBRIS_NEXT_ICD): %s\n", dlerror());
        exit(1);
    }
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(createInstance && createInstance(&instanceInfo, NULL, &instance) == VK_SUCCESS, "instance");
    uint32_t count = 1;
    CHECK(INST(EnumeratePhysicalDevices)(instance, &count, &physical) == VK_SUCCESS && physical, "physical device");
    float priorities[FAKE_QUEUES] = {0};
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = FAKE_QUEUES;
    queueInfo.pQueuePriorities = priorities;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    CHECK(INST(CreateDevice)(physical, &deviceInfo, NULL, &device) == VK_SUCCESS, "device");
    gdpa = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    for (uint32_t i = 0; i < FAKE_QUEUES; i++) DEV(GetDeviceQueue)(device, 0, i, &queues[i]);
    CHECK(queues[0] && queues[FAKE_QUEUES - 1], "queues");
}

static void tearDown(void) {
    DEV(DestroyDevice)(device, NULL);
    INST(DestroyInstance)(instance, NULL);
}

static uint64_t heapMb(void) {
    VkPhysicalDeviceMemoryProperties memory;
    INST(GetPhysicalDeviceMemoryProperties)(physical, &memory);
    VkPhysicalDeviceMemoryProperties2 memory2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    ((PFN_vkGetPhysicalDeviceMemoryProperties2)vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2KHR"))(physical,
                                                                                                                             &memory2);
    CHECK(memory.memoryHeaps[0].size == memory2.memoryProperties.memoryHeaps[0].size, "the same heap from both queries");
    CHECK(memory.memoryHeaps[1].size == 2048ull << 20, "the host heap left alone: %llu", (unsigned long long)memory.memoryHeaps[1].size);
    return memory.memoryHeaps[0].size >> 20;
}

static const VkFence fence = (VkFence)(uintptr_t)0x10;

static VkResult waitFence(void) {
    return DEV(WaitForFences)(device, 1, &fence, VK_TRUE, UINT64_MAX);
}

static VkResult waitSemaphore(void) {
    VkSemaphore semaphore = (VkSemaphore)(uintptr_t)0x20;
    uint64_t value = 1;
    VkSemaphoreWaitInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore;
    info.pValues = &value;
    return DEV(WaitSemaphores)(device, &info, UINT64_MAX);
}

static VkResult readQuery(VkQueryPool pool, VkQueryResultFlags flags) {
    uint32_t data = 7;
    return DEV(GetQueryPoolResults)(device, pool, 0, 1, sizeof(data), &data, sizeof(data), flags);
}

/* ---- the settings ---- */

static void runDefaults(void) {
    setUp();
    heapMb();
    CHECK(DEV(WaitForFences) != (PFN_vkWaitForFences)FAKE(WaitForFences), "vkWaitForFences wrapped");
    script(&fk->fences, VK_SUCCESS, 1, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(waitFence() == VK_SUCCESS && fk->fences.calls == 2, "a -3 retried: calls %d", fk->fences.calls);
    script(&fk->semaphores, VK_SUCCESS, 1, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(waitSemaphore() == VK_SUCCESS && fk->semaphores.calls == 2, "semaphore wait retried: calls %d", fk->semaphores.calls);
    CHECK(DEV(QueueWaitIdle) != (PFN_vkQueueWaitIdle)FAKE(QueueWaitIdle), "vkQueueWaitIdle wrapped");
    script(&fk->queueIdle, VK_SUCCESS, 1, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(DEV(QueueWaitIdle)(queues[0]) == VK_SUCCESS && fk->queueIdle.calls == 2, "queue idle retried: calls %d", fk->queueIdle.calls);
    script(&fk->deviceIdle, VK_SUCCESS, 1, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(DEV(DeviceWaitIdle)(device) == VK_SUCCESS && fk->deviceIdle.calls == 2, "device idle retried: calls %d", fk->deviceIdle.calls);
    script(&fk->queries, VK_SUCCESS, 1, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(readQuery((VkQueryPool)(uintptr_t)0x6000, VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && fk->queries.calls == 2,
          "query wait retried: calls %d", fk->queries.calls);

    /* a queue past the 32 registered goes to the driver */
    script(&fk->queueIdle, VK_SUCCESS, 0);
    CHECK(DEV(QueueWaitIdle)(queues[FAKE_QUEUES - 1]) == VK_SUCCESS && fk->lastIdleQueue == queues[FAKE_QUEUES - 1],
          "unregistered queue: idle wait in the driver");
    CHECK(DEV(QueueSubmit) == (PFN_vkQueueSubmit)FAKE(QueueSubmit) && DEV(QueueSubmit2) == (PFN_vkQueueSubmit2)FAKE(QueueSubmit2),
          "submissions not wrapped");

    /* an occlusion query not ready for 500 ms: counted, then available */
    VkQueryPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
    poolInfo.queryCount = 4;
    VkQueryPool pool = VK_NULL_HANDLE;
    CHECK(DEV(CreateQueryPool)(device, &poolInfo, NULL, &pool) == VK_SUCCESS, "query pool");
    script(&fk->queries, VK_NOT_READY, 0);
    CHECK(readQuery(pool, 0) == VK_NOT_READY, "not ready");
    nanosleep(&(struct timespec){0, 600 * 1000000L}, NULL);
    CHECK(readQuery(pool, 0) == VK_NOT_READY, "still not ready, unchanged");
    script(&fk->queries, VK_SUCCESS, 0);
    CHECK(readQuery(pool, 0) == VK_SUCCESS, "available");
    DEV(DestroyQueryPool)(device, pool, NULL);
    tearDown();
}

/* Every switch off: the driver's own functions, the driver's answers. */
static void runOff(void) {
    setUp();
    CHECK(heapMb() == FAKE_HEAP_MB, "the driver's heap");
    static const char* const names[] = {"vkWaitForFences", "vkWaitSemaphores", "vkGetQueryPoolResults", "vkCreateQueryPool",
                                        "vkDestroyQueryPool", "vkResetQueryPool", "vkCmdResetQueryPool", "vkQueueWaitIdle",
                                        "vkQueueSubmit", "vkQueueSubmit2"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        CHECK(gdpa(device, names[i]) == fakeProc(names[i]), "%s is the driver's", names[i]);
    script(&fk->deviceIdle, VK_SUCCESS, 1, VK_ERROR_INITIALIZATION_FAILED);
    CHECK(DEV(DeviceWaitIdle)(device) == VK_ERROR_INITIALIZATION_FAILED && fk->deviceIdle.calls == 1, "device idle: the driver's -3");
    tearDown();
}

static void runHeap(void) {
    setUp();
    CHECK(heapMb() == 3072, "capped: %llu MB", (unsigned long long)heapMb());
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memory = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    INST(GetPhysicalDeviceMemoryProperties2)(physical, &memory);
    CHECK(fk->budgets == 1 && memory.memoryProperties.memoryHeaps[0].size == 3072ull << 20, "heap with a budget");
    CHECK(budget.heapBudget[0] == 3072ull << 20 && budget.heapUsage[0] == 100ull << 20 && budget.heapBudget[1] == 1500ull << 20,
          "budget held under the heap: %llu", (unsigned long long)(budget.heapBudget[0] >> 20));
    tearDown();
}

static void runCrash(void) {
    setUp();
    heapMb();
    CHECK(DEV(WaitForFences) != (PFN_vkWaitForFences)FAKE(WaitForFences), "vkWaitForFences wrapped for the crash guard alone");
    CHECK(DEV(QueueWaitIdle) != (PFN_vkQueueWaitIdle)FAKE(QueueWaitIdle), "vkQueueWaitIdle wrapped for the crash guard alone");
    CHECK(DEV(QueueSubmit) != (PFN_vkQueueSubmit)FAKE(QueueSubmit), "vkQueueSubmit wrapped");
    script(&fk->fences, VK_ERROR_INITIALIZATION_FAILED, 0);
    CHECK(waitFence() == VK_ERROR_INITIALIZATION_FAILED && fk->fences.calls == 1, "wait retry off: the driver's -3");
    CHECK(DEV(QueueSubmit)(queues[1], 0, NULL, VK_NULL_HANDLE) == VK_SUCCESS && fk->submits == 1, "a submission");

    VkComputePipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = (VkShaderModule)(uintptr_t)0x99;
    pipelineInfo.stage.pName = "main";
    VkPipeline pipeline = (VkPipeline)(uintptr_t)0xdead;
    fk->pipelineFault = 0x18;
    CHECK(DEV(CreateComputePipelines)(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline) == VK_ERROR_UNKNOWN &&
              pipeline == VK_NULL_HANDLE && fk->pipelines == 1,
          "a driver fault in pipeline creation: the call fails");
    fk->pipelineFault = 0;
    CHECK(DEV(CreateComputePipelines)(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline) == VK_SUCCESS &&
              pipeline == (VkPipeline)(uintptr_t)0x7000,
          "the next one is created");
    CHECK(DEV(QueueSubmit)(queues[1], 0, NULL, VK_NULL_HANDLE) == VK_SUCCESS && fk->submits == 2, "not lost after a pipeline");

    VkCommandBuffer commandBuffer = (VkCommandBuffer)&commandBufferObject;
    fk->endFault = 0x18;
    CHECK(DEV(EndCommandBuffer)(commandBuffer) == VK_SUCCESS && fk->ends == 1, "a driver fault in vkEndCommandBuffer: success");
    fk->endFault = 0;

    /* lost: nothing more reaches the driver, nothing waits */
    CHECK(DEV(QueueSubmit)(queues[1], 0, NULL, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST && fk->submits == 2, "submission refused");
    CHECK(DEV(QueueSubmit2)(queues[1], 0, NULL, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST && fk->submits2 == 0, "vkQueueSubmit2 refused");
    script(&fk->fences, VK_SUCCESS, 0);
    CHECK(waitFence() == VK_ERROR_DEVICE_LOST && fk->fences.calls == 0, "fence wait");
    script(&fk->semaphores, VK_SUCCESS, 0);
    CHECK(waitSemaphore() == VK_ERROR_DEVICE_LOST && fk->semaphores.calls == 0, "semaphore wait");
    script(&fk->queueIdle, VK_SUCCESS, 0);
    CHECK(DEV(QueueWaitIdle)(queues[0]) == VK_ERROR_DEVICE_LOST && DEV(QueueWaitIdle)(queues[FAKE_QUEUES - 1]) == VK_ERROR_DEVICE_LOST &&
              fk->queueIdle.calls == 0,
          "queue idle waits");
    script(&fk->deviceIdle, VK_SUCCESS, 0);
    CHECK(DEV(DeviceWaitIdle)(device) == VK_ERROR_DEVICE_LOST && fk->deviceIdle.calls == 0, "device idle wait");
    script(&fk->queries, VK_SUCCESS, 0);
    CHECK(readQuery((VkQueryPool)(uintptr_t)0x6000, VK_QUERY_RESULT_WAIT_BIT) == VK_ERROR_DEVICE_LOST && fk->queries.calls == 0,
          "query read that waits");
    CHECK(readQuery((VkQueryPool)(uintptr_t)0x6000, 0) == VK_SUCCESS && fk->queries.calls == 1, "a query read that does not wait");
    VkSwapchainKHR swapchain = (VkSwapchainKHR)(uintptr_t)0x1234; /* never looked at once lost */
    uint32_t index = 0;
    VkResult results[1] = {VK_SUCCESS};
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    present.pResults = results;
    CHECK(DEV(QueuePresentKHR)(queues[0], &present) == VK_ERROR_DEVICE_LOST && results[0] == VK_ERROR_DEVICE_LOST, "present");
    CHECK(DEV(AcquireNextImageKHR)(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) == VK_ERROR_DEVICE_LOST, "acquire");
    VkAcquireNextImageInfoKHR acquire = {VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};
    acquire.swapchain = swapchain;
    acquire.timeout = UINT64_MAX;
    CHECK(DEV(AcquireNextImage2KHR)(device, &acquire, &index) == VK_ERROR_DEVICE_LOST, "acquire 2");
    tearDown();
}

/* ---- the processes and their logs ---- */

typedef struct {
    const char* name;
    void (*run)(void);
    const char* env[6];      /* VAR=value */
    const char* states[4];   /* the state line of each switch (prefix) */
    const char* lines[6];    /* more lines that must be there */
} Setting;

static const Setting settings[] = {
    {"defaults", runDefaults, {NULL},
     {"heap cap: o", "wait retry: on, ", "query stuck: on, an occlusion query answered not ready for 500 ms is counted, the answers unchanged",
      "crash guard: off "},
     {"wait retry: vkWaitForFences: the driver answered -3", "query stuck: occlusion query 0 of pool 0x5000 answered not ready for",
      "query stuck: occlusion query 0 of pool 0x5000 became available after", "query stuck: 1 occlusion query run(s)"}},
    {"off", runOff, {"DROIDDECK_HYBRIS_WAIT_RETRY=0", "DROIDDECK_HYBRIS_QUERY_STUCK_MS=0", "DROIDDECK_HYBRIS_HEAP_MAX_MB=0"},
     {"heap cap: off (DROIDDECK_HYBRIS_HEAP_MAX_MB=0)", "wait retry: off ", "query stuck: off ", "crash guard: off "},
     {NULL}},
    {"heap", runHeap, {"DROIDDECK_HYBRIS_HEAP_MAX_MB=3072", "DROIDDECK_HYBRIS_QUERY_STUCK_MS=50", "DROIDDECK_HYBRIS_WAIT_RETRY_S=99999999999"},
     {"heap cap: on, device-local heaps reported as 3072 MB at most", "wait retry: on, a wait answered",
      "query stuck: on, an occlusion query answered not ready for 100 ms (DROIDDECK_HYBRIS_QUERY_STUCK_MS=50 raised to the minimum)",
      "crash guard: off "},
     {"heap cap: device-local heap 0 reported as 3072 MB, not 11377 MB", "until the deadline, without a limit on such answers"}},
    {"crash", runCrash, {"DROIDDECK_HYBRIS_CRASH_GUARD=1", "DROIDDECK_HYBRIS_WAIT_RETRY=0"},
     {"heap cap: o", "wait retry: off ", "query stuck: on, ", "crash guard: on: "},
     {"crash guard: SIGSEGV (code 1, address 0x18) in vkCreateComputePipelines at libfake_mali_icd.so+",
      "crash guard: SIGSEGV (code 1, address 0x18) in vkEndCommandBuffer at libfake_mali_icd.so+",
      "crash guard: submissions, waits, presents and acquires fail with VK_ERROR_DEVICE_LOST from now on"}},
};

static const char* const switches[] = {"hybris-icd: heap cap: ", "hybris-icd: wait retry: ", "hybris-icd: query stuck: ",
                                       "hybris-icd: crash guard: "};

/* Lines of `log` that start with "hybris-icd: <prefix>". */
static int countLines(const char* log, const char* prefix) {
    char want[256];
    snprintf(want, sizeof(want), "hybris-icd: %s", prefix);
    int n = 0;
    for (const char* line = log; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL)
        if (!strncmp(line, want, strlen(want))) n++;
    return n;
}

static bool isStateLine(const char* line, size_t prefix) {
    return !strncmp(line + prefix, "on", 2) || !strncmp(line + prefix, "off", 3);
}

static void checkSetting(const Setting* s, const char* self) {
    char path[] = "/tmp/guards_icd_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        CHECK(0, "%s: no log file", s->name);
        return;
    }
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        dup2(fd, 2);
        const char* clear[] = {"DROIDDECK_HYBRIS_WAIT_RETRY", "DROIDDECK_HYBRIS_WAIT_RETRY_S", "DROIDDECK_HYBRIS_QUERY_STUCK_MS",
                               "DROIDDECK_HYBRIS_QUERY_VISIBLE", "DROIDDECK_HYBRIS_HEAP_MAX_MB", "DROIDDECK_HYBRIS_HEAP_FOLLOW_FREE",
                               "DROIDDECK_HYBRIS_CRASH_GUARD", "DROIDDECK_HYBRIS_CRASH_GUARD_MAX", "DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR"};
        for (unsigned i = 0; i < sizeof(clear) / sizeof(clear[0]); i++) unsetenv(clear[i]);
        for (int i = 0; s->env[i]; i++) putenv((char*)s->env[i]);
        alarm(60);
        execl(self, self, s->name, (char*)NULL);
        _exit(98);
    }
    int status = 0;
    waitpid(child, &status, 0);
    char log[65536];
    ssize_t n = pread(fd, log, sizeof(log) - 1, 0);
    log[n > 0 ? n : 0] = 0;
    close(fd);
    unlink(path);
    int before = failures;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: status %#x", s->name, status);
    for (int i = 0; i < 4; i++) {
        int states = 0;
        size_t prefix = strlen(switches[i]);
        for (const char* line = log; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL)
            if (!strncmp(line, switches[i], prefix) && isStateLine(line, prefix)) states++;
        CHECK(states == 1, "%s: %d state lines of %s", s->name, states, switches[i]);
        CHECK(countLines(log, s->states[i]) == 1, "%s: one line \"%s\"", s->name, s->states[i]);
    }
    for (int i = 0; s->lines[i]; i++) CHECK(strstr(log, s->lines[i]) != NULL, "%s: a line with \"%s\"", s->name, s->lines[i]);
    if (failures != before) fprintf(stderr, "---- %s's log:\n%s----\n", s->name, log);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
            if (!strcmp(argv[1], settings[i].name)) settings[i].run();
        printf("guards_icd_test (%s): %d checks, %d failed\n", argv[1], checks, failures);
        return failures ? 1 : 0;
    }
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) checkSetting(&settings[i], "/proc/self/exe");
    printf("guards_icd_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
