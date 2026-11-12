#!/bin/sh
# DROIDDECK_HYBRIS_PASS_BARRIER end to end: pass_barrier_icd_test.c through the Vulkan loader and the
# build-tree ICD (run.sh), with pass_barrier_next_icd.c as DROIDDECK_HYBRIS_NEXT_ICD printing the
# destination masks the driver receives. The next ICD behind that is PB_NEXT_ICD: lavapipe by default
# (libvulkan_lvp.so, mesa-vulkan-drivers; no Android needed), or on the device
#   PB_NEXT_ICD=$PWD/out/guest/usr/local/lib/droiddeck-hybris/lib/libhybris-vulkan-icd.so.0
# for the Mali. Per mode: the masks of the three calls (a transition, then DXVK's barriers after a
# pass before and after a vertex shader module that samples is created), the "narrowed" count of the
# line logged at device destruction, the guard lines.
#   sh tests/pass_barrier_icd_test.sh      after build.sh; needs libvulkan-dev. Prints PBICD-PASS or PBICD-FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$HERE/../out/tests
mkdir -p "$TESTS"
cc -O2 -Wall -o "$TESTS/pass_barrier_icd_test" "$HERE/pass_barrier_icd_test.c" -lvulkan || exit 1
cc -O2 -Wall -shared -fPIC -fvisibility=hidden -o "$TESTS/pass_barrier_next_icd.so" "$HERE/pass_barrier_next_icd.c" -ldl || exit 1
unset DROIDDECK_HYBRIS_PASS_BARRIER DROIDDECK_HYBRIS_PASS_BARRIER_LOG_S
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failed=0
T='driver: image 0x10000'
UNCHANGED="$T|driver: memory 0x1001ff8 image 0x1cf8|driver: memory 0x1001ff8 image 0x1cf8"
run() { # name mode narrowed(- for no line) masks [guard line] [argument]
  name=$1 mode=$2 narrowed=$3 masks=$4 guard=${5:-} arg=${6:-}
  env DROIDDECK_HYBRIS_PASS_BARRIER="$mode" DROIDDECK_HYBRIS_NEXT_ICD="$TESTS/pass_barrier_next_icd.so" \
    timeout 120 sh "$HERE/run.sh" "$TESTS/pass_barrier_icd_test" $arg >"$tmp/$name.out" 2>&1
  rc=$?
  got=$(grep '^driver:' "$tmp/$name.out" | paste -sd'|' -)
  line=$(grep 'hybris-icd: pass barrier: ' "$tmp/$name.out")
  ok=1
  [ $rc = 0 ] && grep -q '^PB-ICD-DONE' "$tmp/$name.out" && ! grep -q '^FAIL' "$tmp/$name.out" || ok=0
  [ "$got" = "$masks" ] || ok=0
  if [ "$narrowed" = - ]; then [ -z "$line" ] || ok=0
  else echo "$line" | grep -q "calls 3, .* narrowed $narrowed," || ok=0
  fi
  [ -z "$guard" ] || grep -q "pass barrier guard: $guard" "$tmp/$name.out" || ok=0
  if [ $ok = 1 ]; then
    echo "$name: ok"
  else
    echo "$name: FAIL (exit $rc)"
    echo "  expected $masks"
    echo "  got      $got"
    grep -v '^driver:' "$tmp/$name.out" | tail -8 | sed 's/^/  /'
    failed=1
  fi
}
NARROW="$T|driver: memory 0x1f80 image 0x1cf8|driver: memory 0x1f80 image 0x1cf8"
run unset '' - "$UNCHANGED"
run off off - "$UNCHANGED"
run log log 0 "$UNCHANGED" 'vertex pipeline image reads (shader module 0x'
run narrow narrow 2 "$NARROW"
run narrow-rt narrow-rt 3 "$T|driver: memory 0x1f80 image 0x1c80|driver: memory 0x1f80 image 0x1cf8" 'vertex pipeline image reads'
run nogeo nogeo 4 "$T|driver: memory 0x1f80 image 0x1c80|driver: memory 0x1f80 image 0x1c80"
run narrow-rt-stores narrow-rt 0 "$UNCHANGED" 'vertexPipelineStoresAndAtomics enabled' stores
run unknown narrow_rt - "$UNCHANGED"
grep -q 'DROIDDECK_HYBRIS_PASS_BARRIER=narrow_rt ignored' "$tmp/unknown.out" || { echo "unknown: no 'ignored' line"; failed=1; }
if [ $failed = 0 ]; then echo PBICD-PASS; else echo PBICD-FAIL; exit 1; fi
