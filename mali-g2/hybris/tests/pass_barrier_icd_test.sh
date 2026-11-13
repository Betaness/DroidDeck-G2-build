#!/bin/sh
# DROIDDECK_HYBRIS_PASS_BARRIER end to end: pass_barrier_icd_test.c through the Vulkan loader and the
# build-tree ICD (run.sh), with pass_barrier_next_icd.c as DROIDDECK_HYBRIS_NEXT_ICD printing the
# destination masks the driver receives. The next ICD behind that is PB_NEXT_ICD: lavapipe by default
# (libvulkan_lvp.so, mesa-vulkan-drivers; no Android needed), or on the device
#   PB_NEXT_ICD=$PWD/out/guest/usr/local/lib/droiddeck-hybris/lib/libhybris-vulkan-icd.so.0
# for the Mali. Per case: the masks of the three calls (a transition, then DXVK's barriers after a
# pass before and after the shader code of pass_barrier_icd_test.c's arguments reaches the driver),
# the "narrowed" count of the line logged at device destruction, the guard lines, that the calls came
# through vkCmdPipelineBarrier2KHR when the test used it.
#   sh tests/pass_barrier_icd_test.sh      after build.sh; needs libvulkan-dev. Prints PBICD-PASS or PBICD-FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$HERE/../out/tests
mkdir -p "$TESTS"
cc -O2 -Wall -o "$TESTS/pass_barrier_icd_test" "$HERE/pass_barrier_icd_test.c" -lvulkan || exit 1
cc -O2 -Wall -shared -fPIC -fvisibility=hidden -o "$TESTS/pass_barrier_next_icd.so" "$HERE/pass_barrier_next_icd.c" -ldl || exit 1
unset DROIDDECK_HYBRIS_PASS_BARRIER DROIDDECK_HYBRIS_PASS_BARRIER_LOG_S DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failed=0
T='driver: image 0x10000'
UNCHANGED="$T|driver: memory 0x1001ff8 image 0x1cf8|driver: memory 0x1001ff8 image 0x1cf8"
run() { # name mode(@unset: not set) narrowed(- for no line) masks guard-lines(;-separated) [test arguments]
  name=$1 mode=$2 narrowed=$3 masks=$4 guards=$5
  shift 5
  args=$*
  case " $args " in *" khr "*) khr=3 ;; *) khr=0 ;; esac
  if [ "$mode" = @unset ]; then set -- -u DROIDDECK_HYBRIS_PASS_BARRIER
  else set -- DROIDDECK_HYBRIS_PASS_BARRIER="$mode"
  fi
  DROIDDECK_HYBRIS_NEXT_ICD="$TESTS/pass_barrier_next_icd.so" timeout 120 env "$@" \
    sh "$HERE/run.sh" "$TESTS/pass_barrier_icd_test" $args >"$tmp/$name.out" 2>&1
  rc=$?
  got=$(grep '^driver:' "$tmp/$name.out" | paste -sd'|' -)
  line=$(grep 'hybris-icd: pass barrier: ' "$tmp/$name.out")
  ok=1
  [ $rc = 0 ] && grep -q '^PB-ICD-DONE' "$tmp/$name.out" && ! grep -q '^FAIL' "$tmp/$name.out" || ok=0
  [ "$got" = "$masks" ] || ok=0
  [ "$(grep -c '^driver-khr$' "$tmp/$name.out")" = $khr ] || ok=0
  if [ "$narrowed" = - ]; then [ -z "$line" ] || ok=0
  else echo "$line" | grep -q "calls 3, .* narrowed $narrowed," || ok=0
  fi
  old=$IFS
  IFS=';'
  for guard in $guards; do grep -qF "pass barrier guard: $guard" "$tmp/$name.out" || ok=0; done
  IFS=$old
  if [ -z "$guards" ] && grep -q 'pass barrier guard' "$tmp/$name.out"; then ok=0; fi
  case $mode in @unset | '' | off) ! grep -q DROIDDECK_HYBRIS_PASS_BARRIER "$tmp/$name.out" || ok=0 ;; esac
  if [ $ok = 1 ]; then
    echo "$name: ok"
  else
    echo "$name: FAIL (exit $rc)"
    echo "  expected $masks"
    echo "  got      $got"
    grep -v '^driver' "$tmp/$name.out" | tail -8 | sed 's/^/  /'
    failed=1
  fi
}
NARROW="$T|driver: memory 0x1f80 image 0x1cf8|driver: memory 0x1f80 image 0x1cf8"
NARROW_RT="$T|driver: memory 0x1f80 image 0x1c80|driver: memory 0x1f80 image 0x1cf8"
READS='vertex pipeline image reads (shader module 0x'
run unset @unset - "$UNCHANGED" ''
run empty '' - "$UNCHANGED" ''
run off off - "$UNCHANGED" ''
run log log 0 "$UNCHANGED" "$READS"
run narrow narrow 2 "$NARROW" "$READS"
run narrow-rt narrow-rt 3 "$NARROW_RT" "$READS"
run nogeo nogeo 4 "$T|driver: memory 0x1f80 image 0x1c80|driver: memory 0x1f80 image 0x1c80" "$READS"
run narrow-rt-stores narrow-rt 0 "$UNCHANGED" "vertexPipelineStoresAndAtomics enabled;$READS" stores
run narrow-rt-khr narrow-rt 3 "$NARROW_RT" "$READS" khr
run off-khr off - "$UNCHANGED" '' khr
run narrow-xfb narrow 1 "$T|driver: memory 0x1f80 image 0x1cf8|driver: memory 0x1001ff8 image 0x1cf8" 'transform feedback (shader module 0x' xfb
run narrow-rt-inline narrow-rt 3 "$NARROW_RT" 'vertex pipeline image reads (inline stage)' inline
run narrow-rt-writes narrow-rt 2 "$T|driver: memory 0x1f80 image 0x1c80|driver: memory 0x1001ff8 image 0x1cf8" \
  "$READS;vertex pipeline memory writes (shader module 0x" sample writes
run unknown narrow_rt - "$UNCHANGED" ''
grep -q 'DROIDDECK_HYBRIS_PASS_BARRIER=narrow_rt ignored' "$tmp/unknown.out" || { echo "unknown: no 'ignored' line"; failed=1; }
if [ $failed = 0 ]; then echo PBICD-PASS; else echo PBICD-FAIL; exit 1; fi
