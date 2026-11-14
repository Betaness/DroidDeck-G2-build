#!/bin/sh
# Builds fg-bench into <dest> (default ./out): an aarch64 glibc Linux program for the DroidDeck guest (or any
# aarch64 Linux with a Vulkan loader). libstdc++ and libgcc are linked in, so only glibc has to match: the build
# stops when the binary needs a glibc symbol newer than GLIBC_MAX (default 2.38; the guest runtime has 2.43).
# The Vulkan loader is opened at run time (dlopen libvulkan.so.1), not linked. fg-bench-guest.sh is copied beside.
# Needs a C++20 compiler (CXX, default g++) and the Vulkan 1.3 headers (system ones, or VULKAN_INCLUDE=<dir>).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
Z=$HERE/third_party/zerofg
GLIBC_MAX=${GLIBC_MAX:-2.38}
mkdir -p "$OUT"
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -DVK_NO_PROTOTYPES ${VULKAN_INCLUDE:+-I"$VULKAN_INCLUDE"} \
    -I"$Z/include" -I"$Z/src" -I"$Z/shaders/prebuilt/rc3" \
    -o "$OUT/fg-bench" "$HERE/fg_bench.cc" \
    "$Z/src/zerofg.cc" "$Z/src/df/df_context.cc" "$Z/src/rc7/rc7_context.cc" "$Z/src/hud/hud_layer.cc" \
    -static-libstdc++ -static-libgcc -ldl -lm -s
newest=$(objdump -T "$OUT/fg-bench" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
top=$(printf '%s\nGLIBC_%s\n' "$newest" "$GLIBC_MAX" | sort -V | tail -1)
if [ "$top" != "GLIBC_$GLIBC_MAX" ]; then
  echo "fg-bench needs $newest, newer than GLIBC_$GLIBC_MAX (GLIBC_MAX)" >&2
  exit 1
fi
if objdump -p "$OUT/fg-bench" | grep -q 'NEEDED.*libvulkan'; then
  echo "fg-bench links libvulkan; it must only dlopen it" >&2
  exit 1
fi
cp "$HERE/fg-bench-guest.sh" "$OUT/"
echo "built $OUT/fg-bench (needs $newest; $(objdump -p "$OUT/fg-bench" | sed -n 's/^ *NEEDED *//p' | tr '\n' ' '))"
