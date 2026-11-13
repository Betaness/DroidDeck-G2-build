/*
 * A stand-in for the Mali driver in tests/crash_guard_test.c: built as libfake_mali.so, so that the
 * crash guard sees these functions in a library whose file name contains "mali".
 */
#include <pthread.h>
#include <stddef.h>
#include <string.h>

int fake_mali_ok(void* ctx) {
    return 42;
}

int fake_mali_read(void* ctx) {
    return *(volatile int*)ctx;
}

int fake_mali_write(void* ctx) {
    *(volatile int*)ctx = 5;
    return 5;
}

/* A fault inside libc's memcpy (a leaf: the return address is here). */
int fake_mali_copy(void* ctx) {
    char out[64];
    size_t n = sizeof(out) - ((size_t)ctx & 7);
    memcpy(out, ctx, n);
    return out[0] + 1;
}

/* Calls back into the caller's code, which faults deeper down. */
int fake_mali_call(void* ctx) {
    int (*fn)(void) = (int (*)(void))ctx;
    return fn() + 1;
}

int fake_mali_exit(void* ctx) {
    pthread_exit(NULL);
}

/* An undefined instruction (SIGILL). */
int fake_mali_ill(void* ctx) {
    __asm__ volatile(".inst 0x00000000");
    return 1;
}

/* A breakpoint (SIGTRAP), as __builtin_trap is on aarch64. */
int fake_mali_trap(void* ctx) {
    __asm__ volatile("brk #0x3e8");
    return 1;
}

/* Two more driver frames with frame records, the second 256 KB down the stack, then a read. */
__attribute__((noinline)) static int faultDeep(void* ctx) {
    volatile char pad[256 * 1024];
    pad[0] = 1;
    pad[sizeof(pad) - 1] = 1;
    return *(volatile int*)ctx + pad[0];
}

__attribute__((noinline)) static int faultNested(void* ctx) {
    return faultDeep(ctx) + 1;
}

int fake_mali_nested(void* ctx) {
    return faultNested(ctx) + 1;
}

#if defined(__aarch64__)
/* A read of x0 with x29 cleared: code built without frame records, using x29 as it likes. */
__asm__(".text\n.globl fake_mali_nofp\n.type fake_mali_nofp, %function\nfake_mali_nofp:\n"
        "    mov x29, #0\n    ldr w0, [x0]\n    ret\n.size fake_mali_nofp, .-fake_mali_nofp\n");

/* A read of x0 with sp at x1 and x29 at x2: driver code on a stack of its own. */
__asm__(".text\n.globl fake_mali_other_stack\n.type fake_mali_other_stack, %function\nfake_mali_other_stack:\n"
        "    mov sp, x1\n    mov x29, x2\n    ldr w0, [x0]\n    ret\n.size fake_mali_other_stack, .-fake_mali_other_stack\n");
#endif
