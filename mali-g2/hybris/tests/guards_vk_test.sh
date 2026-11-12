#!/bin/sh
# tests/guards_vk_test.c through build.sh's ICD and the device's driver, under several settings of
# guards.c's switches, checking the log lines each prints. Needs the device's Vulkan driver
# (tests/run.sh), glslangValidator and libvulkan-dev. GV_STORM_US=<us> (passed on): the waits again
# with SIGALRM at that interval.
#   sh tests/guards_vk_test.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT_TESTS:-$HERE/../out/tests}
mkdir -p "$OUT"
glslangValidator -V --vn guards_comp -o "$OUT/guards_comp.h" "$HERE/guards.comp" >/dev/null
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$OUT" \
    -o "$OUT/guards_vk_test" "$HERE/guards_vk_test.c" -lvulkan -lpthread
LOG=$OUT/guards_vk_test.log
failed=0

# run <name> <expected log line (grep -E), or -> [VAR=value ...]
run() {
    name=$1 expect=$2
    shift 2
    if env "$@" sh "$HERE/run.sh" "$OUT/guards_vk_test" > "$LOG" 2>&1 && grep -q '^GV-PASS' "$LOG" &&
        { [ "$expect" = - ] || grep -Eq "$expect" "$LOG"; }; then
        echo "ok   $name"
    else
        echo "FAIL $name"
        sed 's/^/     /' "$LOG"
        failed=1
    fi
}

run "defaults: stuck query counted" 'query stuck: occlusion query 0 of pool 0x[0-9a-f]+ answered not ready for [0-9]+ ms' GV_VISIBLE=0
run "visible answer" 'answered as visible after' DROIDDECK_HYBRIS_QUERY_VISIBLE=1 GV_VISIBLE=1
run "query watch off" - DROIDDECK_HYBRIS_QUERY_STUCK_MS=0 GV_VISIBLE=0
run "heap cap 3072" 'device-local heap 0 reported as 3072 MB' DROIDDECK_HYBRIS_HEAP_MAX_MB=3072 GV_HEAP_MB=3072
run "heap cap off" 'heap cap: off' DROIDDECK_HYBRIS_HEAP_MAX_MB=0
run "wait retry off" - DROIDDECK_HYBRIS_WAIT_RETRY=0
run "crash guard on" 'crash guard on' DROIDDECK_HYBRIS_CRASH_GUARD=1
run "everything on" 'answered as visible after' DROIDDECK_HYBRIS_CRASH_GUARD=1 DROIDDECK_HYBRIS_QUERY_VISIBLE=1 GV_VISIBLE=1 \
    DROIDDECK_HYBRIS_HEAP_MAX_MB=2048 GV_HEAP_MB=2048
exit $failed
