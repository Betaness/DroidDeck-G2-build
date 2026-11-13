/*
 * [mali-g2] Containment of driver crashes inside chosen driver calls; see crash_guard.h. Behaviour
 * after MaliBridge's crash guard (re-implemented, not copied).
 *
 * In a game process the handlers before ours are Wine's (FEX runs as a Wine DLL and installs none of
 * its own). Ours is installed with the flags and mask of the handler it replaces, so it runs where
 * that one would (Wine's signal stack, with Wine's signals blocked), and every signal it does not
 * contain goes to that handler with the same arguments. It contains only a signal that is all of: on
 * a thread inside crash_guard_call, raised by the kernel, a SIGSEGV for an unmapped address
 * (SEGV_MAPERR: a permission fault may be one Wine resolves and resumes, such as a write watch), a
 * SIGBUS, SIGILL or SIGFPE, and with the pc or the return address in a mapping of a file with "mali"
 * as a word of its name (a fault deeper inside malloc or elsewhere outside the driver is not
 * contained: the jump would leave that code's locks held for every thread).
 *
 * Armed threads are found by pthread_self() in a fixed table, not in thread-local storage: a library
 * loaded with dlopen gets dynamic TLS, whose first use on a thread may allocate, which a signal
 * handler must not do. A guarded call can be abandoned without returning: Wine turns a fault inside
 * a unix call into an exception or an error return without unwinding, and ends threads with
 * pthread_exit. So every SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP and SIGABRT ours passes on first
 * disarms the thread's slot, a thread's exit disarms it (pthread key destructor), every guarded call
 * arms its slot afresh, and the handler jumps only when the guarded call's frame record is on the
 * fault's frame chain (x29 upwards, AAPCS64 frame records) holding what it held when the call was
 * armed, on the same stack mapping as the fault's sp. Driver code that uses x29 for something else
 * breaks the chain: such a fault is passed on.
 *
 * The jump abandons the driver's frames: whatever lock the driver held then stays held, and a later
 * driver call that wants it waits for ever. DROIDDECK_HYBRIS_CRASH_GUARD_MAX (default 8) contained
 * faults per process; after that a fault ends the process as it did before.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include "crash_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#define SLOTS 64
#define DEFAULT_LIMIT 8

typedef struct {
    unsigned long owner; /* pthread_self() of the thread in a guarded call, 0: free */
    int armed;
    uintptr_t frame;           /* the guarded call's frame record */
    uintptr_t frameUp, frameRet; /* its two words when armed */
    int sig, code;
    uintptr_t address, pc, lr;
    sigset_t mask; /* the thread's signal mask when it faulted */
    sigjmp_buf env;
} Slot;

static Slot slots[SLOTS];
static const int guardedSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
#define SIGNAL_COUNT (sizeof(guardedSignals) / sizeof(guardedSignals[0]))
static struct sigaction previous[SIGNAL_COUNT];
static int contained, limit = DEFAULT_LIMIT, enabled;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t exitKey;

static bool alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* "mali" as a word of the file name (vulkan.mali.so, libGLES_mali.so, libmali.so), not inside one
 * (libnormalizer.so). */
static bool nameHasMali(const char* name, size_t length) {
    for (size_t i = 0; i + 4 <= length; i++) {
        if (name[i] != 'm' || name[i + 1] != 'a' || name[i + 2] != 'l' || name[i + 3] != 'i') continue;
        bool before = i == 0 || !alnum(name[i - 1]) || (i == 3 && name[0] == 'l' && name[1] == 'i' && name[2] == 'b');
        if (before && (i + 4 == length || !alnum(name[i + 4]))) return true;
    }
    return false;
}

static const char* hex(const char* p, const char* end, uintptr_t* value) {
    uintptr_t v = 0;
    const char* start = p;
    for (; p < end; p++) {
        char c = *p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v << 4 | (uintptr_t)d;
    }
    *value = v;
    return p > start ? p : NULL;
}

static bool mapsRange(const char* line, size_t length, uintptr_t* start, uintptr_t* stop, const char** rest) {
    const char *p = line, *end = line + length;
    if (!(p = hex(p, end, start)) || p >= end || *p != '-' || !(p = hex(p + 1, end, stop))) return false;
    *rest = p;
    return true;
}

bool crash_guard_maps_line(const char* line, size_t length, uintptr_t address, bool* mali) {
    const char *p, *end = line + length;
    uintptr_t start, stop;
    if (!mapsRange(line, length, &start, &stop, &p) || address < start || address >= stop) return false;
    /* perms, offset, dev, inode, then the path (which may hold spaces) */
    for (int field = 0; field < 4; field++) {
        while (p < end && *p == ' ') p++;
        while (p < end && *p != ' ') p++;
    }
    while (p < end && *p == ' ') p++;
    const char* name = p;
    for (const char* q = p; q < end; q++)
        if (*q == '/') name = q + 1;
    *mali = name < end && nameHasMali(name, (size_t)(end - name));
    return true;
}

typedef struct {
    bool mali;  /* the pc or the return address is in a library of the driver */
    bool stack; /* [sp, top) is inside the mapping that holds sp */
} MapsScan;

/* Signal context: open, read and close only. */
static MapsScan scanMaps(uintptr_t pc, uintptr_t lr, uintptr_t sp, uintptr_t top) {
    MapsScan scan = {false, false};
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return scan;
    char buffer[2048], line[512];
    size_t length = 0;
    bool pcSeen = false, lrSeen = !lr, spSeen = false, m;
    ssize_t n;
    while (!(pcSeen && lrSeen && spSeen) && (n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buffer[i] != '\n') {
                if (length < sizeof(line)) line[length++] = buffer[i];
                continue;
            }
            if (crash_guard_maps_line(line, length, pc, &m)) pcSeen = true, scan.mali |= m;
            if (lr && crash_guard_maps_line(line, length, lr, &m)) lrSeen = true, scan.mali |= m;
            uintptr_t start, stop;
            const char* rest;
            if (mapsRange(line, length, &start, &stop, &rest) && sp >= start && sp < stop) {
                spSeen = true;
                scan.stack = top > sp && top <= stop;
            }
            length = 0;
        }
    }
    close(fd);
    return scan;
}

static uint32_t firstSlot(unsigned long self) {
    return (uint32_t)(((uint64_t)self * 0x9E3779B97F4A7C15ull) >> 58) % SLOTS;
}

static Slot* slotOf(unsigned long self) {
    uint32_t first = firstSlot(self);
    for (uint32_t i = 0; i < SLOTS; i++) {
        Slot* s = &slots[(first + i) % SLOTS];
        if (__atomic_load_n(&s->owner, __ATOMIC_ACQUIRE) == self) return s;
    }
    return NULL;
}

/* The guarded call's frame record is on the chain from `fp`, unchanged since the call was armed. Reads
 * only [sp, frame + 16), which scanMaps found mapped. */
static bool reaches(const Slot* s, uintptr_t fp, uintptr_t sp, uintptr_t frame) {
    for (int i = 0; fp != frame; i++) {
        if (i == 4096 || fp < sp || fp >= frame || (fp & 7)) return false;
        uintptr_t up = *(volatile uintptr_t*)fp;
        if (up <= fp) return false;
        fp = up;
    }
    const volatile uintptr_t* record = (const volatile uintptr_t*)frame;
    return record[0] == s->frameUp && record[1] == s->frameRet;
}

/* Called on the owning thread only (the handler runs on the thread it interrupts). */
static void release(Slot* s) {
    __atomic_store_n(&s->armed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->frame, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->owner, 0, __ATOMIC_RELEASE);
}

static void releaseAtExit(void* value) {
    Slot* s = value;
    if (s && __atomic_load_n(&s->owner, __ATOMIC_ACQUIRE) == (unsigned long)pthread_self()) release(s);
}

/* The thread's slot, armed afresh by the caller: a slot it still holds was left by a call that never
 * returned (a guarded call never calls another one). */
static Slot* arm(unsigned long self) {
    Slot* mine = slotOf(self);
    if (mine) {
        __atomic_store_n(&mine->armed, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&mine->frame, 0, __ATOMIC_RELEASE);
        return mine;
    }
    uint32_t first = firstSlot(self);
    for (uint32_t i = 0; i < SLOTS; i++) {
        Slot* s = &slots[(first + i) % SLOTS];
        unsigned long expected = 0;
        if (__atomic_compare_exchange_n(&s->owner, &expected, self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return s;
    }
    return NULL;
}

static bool takeContainment(void) {
    int n = __atomic_load_n(&contained, __ATOMIC_RELAXED);
    do {
        if (n >= limit) return false;
    } while (!__atomic_compare_exchange_n(&contained, &n, n + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return true;
}

static int signalIndex(int sig) {
    for (unsigned i = 0; i < SIGNAL_COUNT; i++)
        if (guardedSignals[i] == sig) return (int)i;
    return 0;
}

static void chain(int sig, siginfo_t* info, void* context) {
    const struct sigaction* p = &previous[signalIndex(sig)];
    void* handler = (p->sa_flags & SA_SIGINFO) ? (void*)p->sa_sigaction : (void*)p->sa_handler;
    if (handler == (void*)SIG_IGN && info->si_code <= 0) return;
    if (handler != (void*)SIG_DFL && handler != (void*)SIG_IGN) {
        if (p->sa_flags & SA_RESETHAND) {
            /* what the kernel does on delivery to a one-shot handler */
            struct sigaction dfl;
            memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            sigaction(sig, &dfl, NULL);
        }
        if (p->sa_flags & SA_SIGINFO) p->sa_sigaction(sig, info, context);
        else p->sa_handler(sig);
        return;
    }
    /* The action before ours, for the fault that happens again on return (or the signal sent). */
    sigaction(sig, p, NULL);
    if (info->si_code <= 0) raise(sig);
}

static bool containable(int sig, int code) {
    if (code <= 0) return false; /* sent, not raised by the kernel */
    switch (sig) {
    case SIGSEGV: return code == SEGV_MAPERR;
    case SIGBUS: return code == BUS_ADRALN || code == BUS_ADRERR;
    case SIGILL:
    case SIGFPE: return true;
    default: return false;
    }
}

static void note(const char* text) {
    size_t n = 0;
    while (text[n]) n++;
    if (write(2, text, n) < 0) return;
}

static void onFault(int sig, siginfo_t* info, void* context) {
    int savedErrno = errno;
    Slot* s = slotOf((unsigned long)pthread_self());
    ucontext_t* uc = context;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc, lr = (uintptr_t)uc->uc_mcontext.regs[30], sp = (uintptr_t)uc->uc_mcontext.sp;
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
#else
    uintptr_t pc = 0, lr = 0, sp = 0, fp = 0;
#endif
    if (s && __atomic_load_n(&s->armed, __ATOMIC_ACQUIRE)) {
        uintptr_t frame = __atomic_load_n(&s->frame, __ATOMIC_ACQUIRE);
        if (frame && pc && containable(sig, info->si_code)) {
            MapsScan scan = scanMaps(pc, lr, sp, frame + 2 * sizeof(uintptr_t));
            if (scan.mali && !(scan.stack && reaches(s, fp, sp, frame)))
                note("hybris-icd: crash guard: a Mali driver fault whose frame chain does not reach the guarded call (an abandoned "
                     "call, another stack, or code without frame records): passed on\n");
            else if (scan.mali && takeContainment()) {
                s->sig = sig;
                s->code = info->si_code;
                s->address = (uintptr_t)info->si_addr;
                s->pc = pc;
                s->lr = lr;
                s->mask = uc->uc_sigmask;
                __atomic_store_n(&s->armed, 0, __ATOMIC_RELEASE);
                siglongjmp(s->env, 1);
            }
        }
        release(s); /* the handler before ours may resume the call, or abandon it */
    }
    errno = savedErrno;
    chain(sig, info, context);
}

static void install(void) {
    for (unsigned i = 0; i < SIGNAL_COUNT; i++) {
        int sig = guardedSignals[i];
        struct sigaction current;
        if (sigaction(sig, NULL, &current)) continue;
        previous[i] = current;
        void* handler = (current.sa_flags & SA_SIGINFO) ? (void*)current.sa_sigaction : (void*)current.sa_handler;
        struct sigaction mine = current;
        if (handler == (void*)SIG_DFL || handler == (void*)SIG_IGN) {
            sigemptyset(&mine.sa_mask);
            mine.sa_flags = SA_ONSTACK;
        }
        mine.sa_sigaction = onFault;
        mine.sa_flags = (mine.sa_flags | SA_SIGINFO) & ~SA_RESETHAND;
        struct sigaction was;
        if (sigaction(sig, &mine, &was)) continue;
        void* wasHandler = (was.sa_flags & SA_SIGINFO) ? (void*)was.sa_sigaction : (void*)was.sa_handler;
        if (wasHandler != handler) previous[i] = was; /* installed by someone else meanwhile */
    }
}

static void readConfig(void) {
    const char* on = getenv("DROIDDECK_HYBRIS_CRASH_GUARD");
    enabled = on && *on && *on != '0';
    const char* max = getenv("DROIDDECK_HYBRIS_CRASH_GUARD_MAX");
    if (max && *max >= '0' && *max <= '9') limit = atoi(max);
    if (!enabled) {
        fprintf(stderr, "hybris-icd: crash guard: off (DROIDDECK_HYBRIS_CRASH_GUARD=1 turns it on)\n");
        return;
    }
    if (pthread_key_create(&exitKey, releaseAtExit)) {
        enabled = 0;
        fprintf(stderr, "hybris-icd: crash guard: off, no thread key for it\n");
        return;
    }
    install();
    fprintf(stderr, "hybris-icd: crash guard: on: a fault of the Mali driver in pipeline creation or vkEndCommandBuffer fails that "
            "call, %d time(s) at most (DROIDDECK_HYBRIS_CRASH_GUARD_MAX); a lock the driver held then stays held\n", limit);
}

bool crash_guard_on(void) {
    pthread_once(&once, readConfig);
    return enabled;
}

bool crash_guard_thread_armed(void) {
    Slot* s = slotOf((unsigned long)pthread_self());
    return s && __atomic_load_n(&s->armed, __ATOMIC_ACQUIRE);
}

int crash_guard_contained(void) {
    return __atomic_load_n(&contained, __ATOMIC_RELAXED);
}

static const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "signal";
    }
}

/* "libGLES_mali.so+0x1234" for an address, from /proc/self/maps (normal context). */
static void describe(uintptr_t address, char* out, size_t cap) {
    snprintf(out, cap, "%#lx", (unsigned long)address);
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return;
    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start, stop, offset;
        int pathAt = 0;
        if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %n", &start, &stop, &offset, &pathAt) < 3) continue;
        if (address < start || address >= stop) continue;
        char* path = pathAt ? line + pathAt : NULL;
        if (path) path[strcspn(path, "\n")] = 0;
        const char* name = path && *path ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "anonymous";
        snprintf(out, cap, "%s+%#lx", name, (unsigned long)(address - start + offset));
        break;
    }
    fclose(maps);
}

int crash_guard_call(const char* what, int (*fn)(void* ctx), void* ctx, bool* faulted) {
    *faulted = false;
    if (!crash_guard_on() || __atomic_load_n(&contained, __ATOMIC_RELAXED) >= limit) return fn(ctx);
    unsigned long self = (unsigned long)pthread_self();
    Slot* volatile s = arm(self);
    if (!s) return fn(ctx);
    if (sigsetjmp(s->env, 0)) {
        Slot* f = s;
        pthread_sigmask(SIG_SETMASK, &f->mask, NULL);
        int sig = f->sig, code = f->code;
        uintptr_t address = f->address, pc = f->pc, lr = f->lr;
        pthread_setspecific(exitKey, NULL);
        release(f);
        char where[256], back[256];
        describe(pc, where, sizeof(where));
        describe(lr, back, sizeof(back));
        fprintf(stderr, "hybris-icd: crash guard: %s (code %d, address %#lx) in %s at %s (return address %s), contained %d of %d: "
                "the call fails; a lock the driver held stays held\n", signalName(sig), code, (unsigned long)address, what, where,
                back, crash_guard_contained(), limit);
        *faulted = true;
        return 0;
    }
    /* x29 here, and while fn runs: the record every frame of fn's chains up to */
    const uintptr_t* frame = __builtin_frame_address(0);
    s->frameUp = frame[0];
    s->frameRet = frame[1];
    __atomic_store_n(&s->frame, (uintptr_t)frame, __ATOMIC_RELEASE);
    pthread_setspecific(exitKey, s);
    __atomic_store_n(&s->armed, 1, __ATOMIC_RELEASE);
    int result = fn(ctx);
    pthread_setspecific(exitKey, NULL);
    if (__atomic_load_n(&s->owner, __ATOMIC_ACQUIRE) == self) release(s);
    return result;
}
