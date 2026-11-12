/*
 * Unit test for src/crash_guard.c, no GPU: libfake_mali.so (tests/fake_mali.c) stands in for the
 * driver, and a SIGSEGV/SIGBUS handler installed first stands in for Wine's (it resumes a write to its
 * watched page, and abandons the call otherwise, as Wine does with a fault inside a unix call).
 *   sh tests/guards_test.sh        (builds and runs this and tests/guards_test.c)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crash_guard.h"

int fake_mali_ok(void* ctx);
int fake_mali_read(void* ctx);
int fake_mali_write(void* ctx);
int fake_mali_copy(void* ctx);
int fake_mali_call(void* ctx);
int fake_mali_exit(void* ctx);
int fake_mali_ill(void* ctx);
int fake_mali_trap(void* ctx);

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

/* ---- the handler before ours ---- */

static __thread sigjmp_buf abandonTo;
static __thread volatile int abandonReady;
static volatile int previousHits, previousResumes;
static char* watchPage;
static long pageSize;

static void previousHandler(int sig, siginfo_t* info, void* context) {
    char* at = info->si_addr;
    if (sig == SIGSEGV && info->si_code == SEGV_ACCERR && watchPage && at >= watchPage && at < watchPage + pageSize) {
        mprotect(watchPage, pageSize, PROT_READ | PROT_WRITE);
        previousResumes++;
        return;
    }
    previousHits++;
    if (abandonReady) {
        abandonReady = 0;
        siglongjmp(abandonTo, 1);
    }
    static const char msg[] = "crash_guard_test: unexpected fault\n";
    if (write(2, msg, sizeof(msg) - 1) < 0) _exit(98);
    _exit(99);
}

static void installPrevious(void) {
    static char altStack[64 * 1024];
    stack_t ss = {.ss_sp = altStack, .ss_size = sizeof(altStack)};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = previousHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &sa, NULL);
}

/* The statement's fault is abandoned by the previous handler. */
#define EXPECT_ABANDONED(what, ...)                                            \
    do {                                                                       \
        int hits = previousHits;                                               \
        abandonReady = 1;                                                      \
        if (!sigsetjmp(abandonTo, 1)) {                                        \
            __VA_ARGS__;                                                       \
            abandonReady = 0;                                                  \
            CHECK(0, "%s: not abandoned", what);                              \
        }                                                                      \
        CHECK(previousHits == hits + 1, "%s: previous handler hits %d", what, previousHits - hits); \
    } while (0)

/* ---- callbacks outside the fake driver ---- */

static volatile uintptr_t unmapped = 0x20;

__attribute__((noinline)) static int innerFault(void) {
    return *(volatile int*)unmapped;
}

__attribute__((noinline)) static int deepFault(void) {
    return innerFault() + 1;
}

static int selfSignal(void* ctx) {
    pthread_kill(pthread_self(), SIGSEGV);
    return 1;
}

static int abandonDirectly(void* ctx) {
    abandonReady = 0;
    siglongjmp(abandonTo, 1);
}

static int callAbort(void* ctx) {
    abort();
}

static int nestedOuter(void* ctx) {
    bool innerFaulted = false;
    int r = crash_guard_call("inner", fake_mali_read, (void*)(uintptr_t)0x18, &innerFaulted);
    return innerFaulted ? r + 100 : -1;
}

static int contain(const char* what, int (*fn)(void*), void* ctx) {
    bool faulted = false;
    int before = crash_guard_contained();
    int r = crash_guard_call(what, fn, ctx, &faulted);
    CHECK(faulted && r == 0, "%s: contained (faulted %d, result %d)", what, faulted, r);
    CHECK(crash_guard_contained() == before + 1, "%s: count %d", what, crash_guard_contained());
    return faulted;
}

static void* threadExit(void* arg) {
    bool faulted;
    crash_guard_call("exit", fake_mali_exit, NULL, &faulted);
    return NULL;
}

static void* threadContain(void* arg) {
    bool faulted = false;
    int r = crash_guard_call("thread", fake_mali_read, (void*)(uintptr_t)0x30, &faulted);
    CHECK(faulted && r == 0, "thread: contained (faulted %d, result %d)", faulted, r);
    return NULL;
}

/* ---- tests ---- */

static void testMapsLine(void) {
    bool mali = false;
    const char* line = "7f1000-7f3000 r-xp 00001000 fd:01 1234                       /vendor/lib64/hw/vulkan.mali.so";
    CHECK(crash_guard_maps_line(line, strlen(line), 0x7f2000, &mali) && mali, "mali library");
    CHECK(!crash_guard_maps_line(line, strlen(line), 0x7f3000, &mali), "past the end");
    const char* libc = "7f1000-7f3000 r-xp 00000000 fd:01 99 /usr/lib/aarch64-linux-gnu/libc.so.6";
    CHECK(crash_guard_maps_line(libc, strlen(libc), 0x7f1000, &mali) && !mali, "libc");
    const char* dir = "7f1000-7f3000 r-xp 00000000 fd:01 99 /root/mali-g2/out/libother.so";
    CHECK(crash_guard_maps_line(dir, strlen(dir), 0x7f1000, &mali) && !mali, "mali only in the directory");
    const char* anon = "7f1000-7f3000 rw-p 00000000 00:00 0";
    CHECK(crash_guard_maps_line(anon, strlen(anon), 0x7f1000, &mali) && !mali, "anonymous");
    const char* spaces = "7f1000-7f3000 r-xp 00000000 fd:01 99   /data/my apps/libGLES_mali.so";
    CHECK(crash_guard_maps_line(spaces, strlen(spaces), 0x7f1000, &mali) && mali, "path with spaces");
    CHECK(!crash_guard_maps_line("garbage", 7, 1, &mali), "garbage");
}

static void testOff(void) {
    installPrevious();
    CHECK(!crash_guard_on(), "off without DROIDDECK_HYBRIS_CRASH_GUARD");
    struct sigaction now;
    sigaction(SIGSEGV, NULL, &now);
    CHECK(now.sa_sigaction == previousHandler, "no handler installed");
    bool faulted = true;
    CHECK(crash_guard_call("ok", fake_mali_ok, NULL, &faulted) == 42 && !faulted, "a plain call");
    EXPECT_ABANDONED("off: mali fault", crash_guard_call("read", fake_mali_read, (void*)(uintptr_t)0x18, &faulted));
}

static void testOn(void) {
    installPrevious();
    pageSize = sysconf(_SC_PAGESIZE);
    watchPage = mmap(NULL, pageSize, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(crash_guard_on(), "on");
    struct sigaction now;
    sigaction(SIGSEGV, NULL, &now);
    CHECK(now.sa_sigaction != previousHandler, "ours installed");
    CHECK((now.sa_flags & SA_ONSTACK) && (now.sa_flags & SA_SIGINFO) && sigismember(&now.sa_mask, SIGUSR1),
          "the previous handler's flags and mask: %#x", now.sa_flags);

    bool faulted = true;
    CHECK(crash_guard_call("ok", fake_mali_ok, NULL, &faulted) == 42 && !faulted, "a call that returns");

    int hits = previousHits;
    contain("null read", fake_mali_read, (void*)(uintptr_t)0x18);
    contain("leaf libc call from the driver", fake_mali_copy, (void*)(uintptr_t)0x40);
    CHECK(previousHits == hits, "contained faults do not reach the previous handler");

    /* a fault two calls away from the driver: passed on, the call abandoned */
    EXPECT_ABANDONED("deep fault outside the driver", crash_guard_call("deep", fake_mali_call, (void*)deepFault, &faulted));
    contain("after an abandoned call", fake_mali_read, (void*)(uintptr_t)0x18);

    /* a permission fault the previous handler resolves: the call goes on */
    faulted = true;
    int r = crash_guard_call("watched write", fake_mali_write, watchPage, &faulted);
    CHECK(r == 5 && !faulted && previousResumes == 1, "resumed: result %d, faulted %d, resumes %d", r, faulted, previousResumes);
    contain("after a resumed call", fake_mali_read, (void*)(uintptr_t)0x18);

    /* SIGBUS: a mapping past the end of its file */
    char path[] = "/tmp/crash_guard_test.XXXXXX";
    int fd = mkstemp(path);
    char* mapped = MAP_FAILED;
    if (fd >= 0 && ftruncate(fd, 2 * pageSize) == 0) mapped = mmap(NULL, 2 * pageSize, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped != MAP_FAILED && ftruncate(fd, 0) == 0) contain("SIGBUS", fake_mali_read, mapped + pageSize);
    else CHECK(0, "no file mapping for SIGBUS: %s", strerror(errno));
    if (mapped != MAP_FAILED) munmap(mapped, 2 * pageSize);
    if (fd >= 0) close(fd), unlink(path);

    /* a fault in the driver outside any guarded call */
    EXPECT_ABANDONED("unguarded driver fault", fake_mali_read((void*)(uintptr_t)0x18));

    /* SIGSEGV sent, not raised by a fault */
    EXPECT_ABANDONED("sent signal", crash_guard_call("sent", selfSignal, NULL, &faulted));

    /* nested (not used that way): the inner call is guarded, the outer goes on unguarded */
    faulted = true;
    int before = crash_guard_contained();
    r = crash_guard_call("outer", nestedOuter, NULL, &faulted);
    CHECK(!faulted && r == 100 && crash_guard_contained() == before + 1, "nested: faulted %d result %d", faulted, r);
    CHECK(!crash_guard_thread_armed(), "nothing armed after the outer call");

    /* SIGILL in the driver: contained */
    contain("SIGILL", fake_mali_ill, NULL);

    /* SIGTRAP and SIGABRT are passed on, and disarm the abandoned call */
    EXPECT_ABANDONED("SIGTRAP", crash_guard_call("trap", fake_mali_trap, NULL, &faulted));
    CHECK(!crash_guard_thread_armed(), "disarmed by SIGTRAP");
    EXPECT_ABANDONED("SIGABRT", crash_guard_call("abort", callAbort, NULL, &faulted));
    CHECK(!crash_guard_thread_armed(), "disarmed by SIGABRT");
    EXPECT_ABANDONED("deep fault", crash_guard_call("deep", fake_mali_call, (void*)deepFault, &faulted));
    CHECK(!crash_guard_thread_armed(), "disarmed by a fault passed on");

    /* a call abandoned without a signal (none of ours ran): still armed, but a later driver fault
     * from a shallower frame is not jumped to it; the next guarded call arms afresh */
    abandonReady = 1;
    if (!sigsetjmp(abandonTo, 1)) crash_guard_call("abandoned", abandonDirectly, NULL, &faulted);
    CHECK(crash_guard_thread_armed(), "left armed");
    EXPECT_ABANDONED("fault after an abandoned frame", fake_mali_read((void*)(uintptr_t)0x18));
    CHECK(!crash_guard_thread_armed(), "disarmed by the fault passed on");
    abandonReady = 1;
    if (!sigsetjmp(abandonTo, 1)) crash_guard_call("abandoned again", abandonDirectly, NULL, &faulted);
    contain("armed afresh over a stale slot", fake_mali_read, (void*)(uintptr_t)0x18);

    /* threads that end inside guarded calls give their slots back (more threads than slots) */
    for (int i = 0; i < 100; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, threadExit, NULL)) {
            CHECK(0, "pthread_create");
            break;
        }
        pthread_join(t, NULL);
    }
    int beforeThreads = crash_guard_contained();
    pthread_t a, b;
    pthread_create(&a, NULL, threadContain, NULL);
    pthread_create(&b, NULL, threadContain, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    CHECK(crash_guard_contained() == beforeThreads + 2, "two threads: %d", crash_guard_contained() - beforeThreads);

    /* the limit (DROIDDECK_HYBRIS_CRASH_GUARD_MAX=16 from main) */
    while (crash_guard_contained() < 16 && !failures) contain("up to the limit", fake_mali_read, (void*)(uintptr_t)0x18);
    EXPECT_ABANDONED("over the limit", crash_guard_call("over", fake_mali_read, (void*)(uintptr_t)0x18, &faulted));
    CHECK(crash_guard_call("ok", fake_mali_ok, NULL, &faulted) == 42 && !faulted, "calls still work");
}

int main(int argc, char** argv) {
    testMapsLine();
    if (argc > 1 && !strcmp(argv[1], "off")) {
        testOff();
    } else {
        unsetenv("DROIDDECK_HYBRIS_CRASH_GUARD");
        pid_t child = fork();
        if (child == 0) {
            execl("/proc/self/exe", argv[0], "off", (char*)NULL);
            _exit(98);
        }
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the off run: status %#x", status);
        setenv("DROIDDECK_HYBRIS_CRASH_GUARD", "1", 1);
        setenv("DROIDDECK_HYBRIS_CRASH_GUARD_MAX", "16", 1);
        testOn();
    }
    printf("crash_guard_test%s: %d checks, %d failed\n", argc > 1 ? " (off)" : "", checks, failures);
    return failures ? 1 : 0;
}
