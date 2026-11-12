#!/bin/sh
# Builds and runs the GPU-free unit tests of guards.c's pieces: tests/guards_test.c (heap_cap.h,
# wait_retry.h, query_stuck.h) and tests/crash_guard_test.c (src/crash_guard.c, with
# tests/fake_mali.c as libfake_mali.so standing in for the driver). Needs cc and the Vulkan headers.
#   sh tests/guards_test.sh        OUT (default ../out/tests) receives the binaries
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../src
OUT=${OUT:-$HERE/../out/tests}
CC=${CC:-cc}
CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
mkdir -p "$OUT"
$CC $CFLAGS -I"$SRC" -o "$OUT/guards_test" "$HERE/guards_test.c" -lpthread
$CC $CFLAGS -fPIC -shared -o "$OUT/libfake_mali.so" "$HERE/fake_mali.c" -lpthread
$CC $CFLAGS -I"$SRC" -o "$OUT/crash_guard_test" "$HERE/crash_guard_test.c" "$SRC/crash_guard.c" \
    -L"$OUT" -lfake_mali -Wl,-rpath,'$ORIGIN' -lpthread
"$OUT/guards_test"
"$OUT/crash_guard_test"
