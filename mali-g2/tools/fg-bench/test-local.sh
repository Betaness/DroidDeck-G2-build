#!/bin/sh
# Offline check of fg-bench on the host's lavapipe (Mesa's CPU Vulkan; correctness only, its times mean nothing):
# small pictures, both modes, the three formats, serial and pipelined, consecutive and fresh pairs. Every run must
# exit 0 and print FGBENCH_JSON lines with "ok":true, no near copy of an input, and an output nearer the exact
# midpoint than the plain blend. Builds fg-bench first unless FGBENCH is given.
# On CPUs whose kernel keeps SVE off for user space (Qualcomm: the K70) LLVM still picks the core by its part
# number and llvmpipe's JIT emits SVE (SIGILL): tests/cpuinfo_shim.c shows /proc/cpuinfo as Cortex-A76, and the
# Mesa shader cache stays off (a cached SVE shader from an earlier run would crash the same way).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/fgbench-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM
if [ -z "${FGBENCH:-}" ]; then
  sh "$HERE/build.sh" "$TMP/out" >/dev/null
  FGBENCH=$TMP/out/fg-bench
fi
LVP=${LVP_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
[ -r "$LVP" ] || { echo "no lavapipe ICD at $LVP (mesa-vulkan-drivers)" >&2; exit 2; }
${CC:-cc} -O2 -shared -fPIC -o "$TMP/cpuinfo_shim.so" "$HERE/tests/cpuinfo_shim.c" -ldl

fail=0
run() {
  echo "== $*"
  if ! VK_ICD_FILENAMES=$LVP VK_DRIVER_FILES=$LVP MESA_SHADER_CACHE_DISABLE=true LD_PRELOAD="$TMP/cpuinfo_shim.so" \
       "$FGBENCH" --json --iters 3 --warmup 1 "$@" > "$TMP/log" 2>&1; then
    cat "$TMP/log"; echo "FAIL: exit status"; fail=1; return
  fi
  grep -v '^FGBENCH_JSON' "$TMP/log" | grep -E 'GPU ms|check' || true
  n=$(grep -c '^FGBENCH_JSON' "$TMP/log" || true)
  [ "$n" -gt 0 ] || { echo "FAIL: no result line"; fail=1; return; }
  if grep '^FGBENCH_JSON' "$TMP/log" | python3 -c '
import json, sys
bad = 0
for line in sys.stdin:
    d = json.loads(line.split(" ", 1)[1])
    if not d["ok"] or d["near_copy"] or not d["mae_out"] < d["mae_blend"] or not d["gpu_median_ms"] > 0:
        print("bad result:", d); bad = 1
sys.exit(bad)'; then :; else fail=1; fi
}

run --size 256x160
run --size 200x128 --in rgba8 --out bgra8 --serial
run --size 256x144 --in a2b10 --out a2b10 --mode reallyzero
run --size 192x128 --fresh --backend compat --motion 4,2
if [ $fail -ne 0 ]; then echo "test-local: FAILED"; exit 1; fi
echo "test-local: all ok"
