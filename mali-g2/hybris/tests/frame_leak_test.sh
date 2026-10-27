#!/bin/sh
# The Mali's command buffer memory in the game process (mali_fixes.c, vortek/src/cmd_memory.h): runs
# ../vortek/tests/frame_leak_test.c offscreen through the build-tree ICD (tests/run.sh) with a
# DXVK-like frame (16 dynamic rendering passes and a timestamp query per command buffer) and reports
# the process's own heap (first report, peak, steady state = mean of the last half), its growth per
# frame, the wrapper's releases per 1000 frames and the process's CPU time per frame, for:
#   *-off        DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=0: must grow above FL_MAX_GROWTH (about 6.7 KB
#                per frame on the Mali-G2)
#   pool-every32 the fixed interval (DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=32): must stay flat; it
#                releases 31.25 times per 1000 frames here (one pool reset per frame)
#   *-adaptive   the default, release by the estimate: must stay bounded (no growth over the last half,
#                peak at most FL_MAX_PEAK_KB above the first report, default 2 x the total limit +
#                8 MiB) and release at most 1000/32/8 = 3.9 times per 1000 frames
#   pool-total   FL_INFLIGHT=4 pools, *_BYTES=8m, *_TOTAL_BYTES=8m: the limit over all pools (peak at
#                most 24 MiB above the first report)
#   noquery      no query in the frame: no release at all
#   renderpass   VkRenderPass instances with the query (they keep nothing): the backstop releases
#                every 1024th reset
# for vkResetCommandPool (DXVK's), vkResetCommandBuffer (FL_CB_RESET=1) and the implicit reset of
# vkBeginCommandBuffer (pools created with RESET_COMMAND_BUFFER_BIT).
#   sh tests/frame_leak_test.sh          after build.sh; needs libvulkan-dev and libx11-dev
# FL_FRAMES (default 20000), FL_MAX_GROWTH (default 64 bytes per frame). DROIDDECK_HYBRIS_CMD_RELEASE_BYTES
# (and _TOTAL_BYTES) apply to the adaptive runs. Prints FRAMELEAK-PASS or FRAMELEAK-FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$HERE/../out/tests
REPORT=$HERE/../../vortek/tests/frame_leak_report.awk
mkdir -p "$TESTS"
cc -O2 -o "$TESTS/frame_leak_test" "$HERE/../../vortek/tests/frame_leak_test.c" -lvulkan -lX11 || exit 1
: "${FL_FRAMES:=20000}"
export FL_FRAMES FL_PASSES=16 FL_REPORT=1000 FL_SERVER_PID=self DROIDDECK_HYBRIS_CMD_RELEASE_LOG=1
max=${FL_MAX_GROWTH:-64}
bytes=${DROIDDECK_HYBRIS_CMD_RELEASE_BYTES:-16m}
unset DROIDDECK_HYBRIS_CMD_RELEASE_EVERY VORTEK_CMD_RELEASE_EVERY VORTEK_CMD_RELEASE_BYTES VORTEK_CMD_RELEASE_TOTAL_BYTES
kb() { awk -v t="$1" 'BEGIN { v = t + 0; u = substr(t, length(t)); if (u == "g" || u == "G") v *= 1048576; else if (u == "m" || u == "M") v *= 1024; else if (u != "k" && u != "K") v /= 1024; printf "%d", v }'; }
total=${DROIDDECK_HYBRIS_CMD_RELEASE_TOTAL_BYTES:-$(( $(kb "$bytes") * 2 ))k}
peak=${FL_MAX_PEAK_KB:-$(( $(kb "$total") * 2 + 8192 ))}
fail=0

run() { # name expect env... (FL_* and DROIDDECK_HYBRIS_* for this run)
    name=$1 expect=$2
    shift 2
    log=$TESTS/frame_leak_$name.log
    env "$@" timeout 600 sh "$HERE/run.sh" "$TESTS/frame_leak_test" >"$log" 2>&1
    rc=$?
    case $expect in
    bounded) verdict=$(awk -v expect=bounded -v max="$max" -v maxpeak="$peak" -v maxrel=3.9 -f "$REPORT" "$log") ;;
    bounded-total) verdict=$(awk -v expect=bounded -v max="$max" -v maxpeak=$((8192 * 2 + 8192)) -f "$REPORT" "$log") ;;
    *) verdict=$(awk -v expect="$expect" -v max="$max" -f "$REPORT" "$log") ;;
    esac
    echo "$name: $verdict"
    if [ $rc -ne 0 ]; then grep -E "FAIL|rror" "$log" | head -5; fail=1; fi
    case $verdict in *": PASS"*) ;; *) fail=1 ;; esac
}
releases() { grep -c "command memory: release #" "$TESTS/frame_leak_$1.log"; }

POOL=timeline,submit2,draw,query,poolreset
CB=timeline,submit2,draw,query
A="DROIDDECK_HYBRIS_CMD_RELEASE_BYTES=$bytes DROIDDECK_HYBRIS_CMD_RELEASE_TOTAL_BYTES=$total"
# shellcheck disable=SC2086
{
run pool-off grow DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=0 FL_STEPS=$POOL
run pool-every32 flat DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=32 FL_STEPS=$POOL
run pool-adaptive bounded $A FL_STEPS=$POOL
run pool-total bounded-total DROIDDECK_HYBRIS_CMD_RELEASE_BYTES=8m DROIDDECK_HYBRIS_CMD_RELEASE_TOTAL_BYTES=8m FL_INFLIGHT=4 FL_STEPS=$POOL
run cmdbuf-off grow DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=0 FL_STEPS=$CB FL_CB_RESET=1
run cmdbuf-adaptive bounded $A FL_STEPS=$CB FL_CB_RESET=1
run implicit-off grow DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=0 FL_STEPS=$CB
run implicit-adaptive bounded $A FL_STEPS=$CB
run noquery flat $A FL_STEPS=timeline,submit2,draw,poolreset
run renderpass flat $A FL_STEPS=$POOL FL_RENDERPASS=1
}
# pool-total: four pools of ~1.7 MB each per 1000 frames reach the 8m total long before 8m each,
# so each release frees about a quarter of it
n=$(releases pool-total)
if [ "$n" -lt $((FL_FRAMES * 6700 / 8388608 - 2)) ]; then echo "pool-total: only $n releases"; fail=1; fi
n=$(releases noquery)
if [ "$n" -ne 0 ]; then echo "noquery: $n releases without a query"; fail=1; fi
n=$(releases renderpass)
expect=$((FL_FRAMES / 2 / 1024 * 2))
if [ "$n" -lt $((expect - 2)) ] || [ "$n" -gt $((expect + 2)) ]; then echo "renderpass: $n releases, backstop expects about $expect"; fail=1; fi

if [ $fail -ne 0 ]; then echo FRAMELEAK-FAIL; exit 1; fi
echo FRAMELEAK-PASS
