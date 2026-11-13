#!/bin/sh
# Builds and runs the GPU-free unit tests of guards.c and its pieces: tests/guards_test.c (heap_cap.h,
# wait_retry.h, query_stuck.h), tests/crash_guard_test.c (src/crash_guard.c, with tests/fake_mali.c
# as libfake_mali.so standing in for the driver) and tests/guards_icd_test.c (the ICD built from src/
# without libhybris, over tests/fake_icd.c as libfake_mali_icd.so). Needs cc, the Vulkan headers and
# libxcb/libxcb-shm (headers and libraries).
#   sh tests/guards_test.sh        OUT (default ../out/tests) receives the binaries
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../src
VORTEK=$HERE/../../vortek/src
OUT=${OUT:-$HERE/../out/tests}
CC=${CC:-cc}
CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
mkdir -p "$OUT"
$CC $CFLAGS -I"$SRC" -o "$OUT/guards_test" "$HERE/guards_test.c" -lpthread
$CC $CFLAGS -fPIC -shared -o "$OUT/libfake_mali.so" "$HERE/fake_mali.c" -lpthread
$CC $CFLAGS -I"$SRC" -o "$OUT/crash_guard_test" "$HERE/crash_guard_test.c" "$SRC/crash_guard.c" \
    -L"$OUT" -lfake_mali -Wl,-rpath,'$ORIGIN' -lpthread
ICD_SOURCES=
for c in "$SRC"/*.c; do
    [ "$(basename "$c")" = ahb.c ] || ICD_SOURCES="$ICD_SOURCES $c"
done
# shellcheck disable=SC2086
$CC $CFLAGS -fPIC -fvisibility=hidden -shared -I"$SRC/compat" -I"$SRC" -I"$VORTEK" -o "$OUT/libguards_icd.so" $ICD_SOURCES \
    "$VORTEK/zero_copy.c" "$HERE/ahb_stub.c" -Wl,--no-undefined -lxcb -lxcb-shm -lpthread -ldl
$CC $CFLAGS -fPIC -shared -o "$OUT/libfake_mali_icd.so" "$HERE/fake_icd.c"
$CC $CFLAGS -I"$HERE" -o "$OUT/guards_icd_test" "$HERE/guards_icd_test.c" -L"$OUT" -lguards_icd -Wl,-rpath,'$ORIGIN' -ldl -lpthread
"$OUT/guards_test"
"$OUT/crash_guard_test"
DROIDDECK_HYBRIS_NEXT_ICD="$OUT/libfake_mali_icd.so" "$OUT/guards_icd_test"
