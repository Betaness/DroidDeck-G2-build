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
