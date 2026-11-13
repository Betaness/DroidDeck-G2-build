/*
 * [mali-g2] Containment of driver crashes inside chosen driver calls (crash_guard.c). Vulkan-free:
 * tests/crash_guard_test.c builds it alone, with a stand-in driver (tests/fake_mali.c).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_HYBRIS_CRASH_GUARD_H
#define DROIDDECK_HYBRIS_CRASH_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DROIDDECK_HYBRIS_CRASH_GUARD=1 (read once). The handlers are installed the first time it is on. */
bool crash_guard_on(void);

/* fn(ctx) on this thread; a SIGSEGV for an unmapped address, a SIGBUS, SIGILL or SIGFPE, raised with
 * the pc (or the return address) in a library with "mali" as a word of its file name, and with this
 * call's frame on the fault's frame chain, returns here with *faulted set and 0 returned. `what`
 * names the call in the log line. Unguarded (a plain call) when the guard is off, the containment
 * limit is reached, or all slots are taken. Not for calls that can call crash_guard_call again on
 * the same thread. */
int crash_guard_call(const char* what, int (*fn)(void* ctx), void* ctx, bool* faulted);

/* Contained faults so far in this process. */
int crash_guard_contained(void);

/* Internals the unit test checks: whether this thread holds an armed slot; whether the line of
 * /proc/self/maps text (without its newline) holds `address`, and whether its file name has "mali"
 * as a word. */
bool crash_guard_thread_armed(void);
bool crash_guard_maps_line(const char* line, size_t length, uintptr_t address, bool* mali);

#endif
