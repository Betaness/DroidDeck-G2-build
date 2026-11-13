#!/bin/sh
# Builds the tests and runs the X11 WSI on a private Xvfb display with the device's Mali driver, in
# PalmCode's Ubuntu or any aarch64 glibc shell on the device that can load the vendor driver
# (/system, /vendor and /apex visible). Needs build.sh's output, Xvfb, libxcb1-dev, libvulkan-dev.
#   tests/wsi_test.sh            all scenarios; exit status 0 when all pass
# Scenarios:
#   copy        no compositor: frames are copied into the window (MIT-SHM), checked with GetImage
#   copy-rgba   the same with an RGBA swapchain (R/B swizzle)
#   zero-copy   mock_compositor.c as the compositor: AHardwareBuffer images, acquire and release
#               fences, pixels checked through CPU-readable buffers (DROIDDECK_HYBRIS_AHB_CPU_READ=1)
#   zero-copy-late  the same, the mock releasing without fences
#   zero-copy-shared-queue  the application takes every queue and submits from a second thread too:
#               acquire shares a queue, under a lock
#   zero-copy-afbc  buffers as the session allocates them (likely AFBC: pixels not checked)
#   cover-partly, cover-fully  another window over the window: X11 copies, paced frames (10 a second), back
#   cover-fully-default  the same at the default pace of hidden windows (VORTEK_COVERED_FPS unset: 1 a
#               second), fewer frames
#   vkcube      vkcube zero-copy, its last frame in out/tests/vkcube.ppm
#   resize      the window shrinks halfway: VK_SUBOPTIMAL_KHR, swapchain recreated
#   fshack, fshack-copy  ../vortek/tests/fshack_test.c: Proton's fullscreen hack (an emulated display
#               mode) sizes its host swapchain from minImageExtent and writes it with a compute shader;
#               the swapchain must be the client window's size and never recreated (zero-copy with the
#               mock checking the pixels, and copied into the window)
#   fshack-plain, fshack-windowed  no fullscreen hack: the window's size; and A.V.A.'s windowed layout
#               (the client window in a 1x1 parent, copied)
#   recreate-loop  an application recreating a mismatched swapchain with the same parameters on every
#               SUBOPTIMAL: the loop stops after VORTEK_RECREATE_LIMIT, and again after the window
#               changes size
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=${OUT:-$ROOT/out/guest}
GUEST_DIR=${GUEST_DIR:-/usr/local/lib/droiddeck-hybris}
BIN=$ROOT/out/tests
mkdir -p "$BIN"
STAGE_INC=$ROOT/work/stage$GUEST_DIR/include
cc -O2 -Wall -o "$BIN/swapchain_test" "$HERE/swapchain_test.c" -lvulkan -lxcb -lpthread || exit 1
cc -O2 -Wall -I"$ROOT/../vortek/tests" -o "$BIN/fshack_test" "$ROOT/../vortek/tests/fshack_test.c" -lvulkan -lxcb || exit 1
cc -O2 -Wall -I"$ROOT/src/compat" -I"$STAGE_INC" -o "$BIN/mock_compositor" "$HERE/mock_compositor.c" "$ROOT/src/ahb.c" \
    "$OUT$GUEST_DIR/lib/libhybris-common.so.1" -lpthread || exit 1

TMP=$(mktemp -d /tmp/hybris-wsi.XXXXXX)
disp=90
while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >"$TMP/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -e /tmp/.X11-unix/X$disp ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=:$disp

failed=""
run() { # name, then environment assignments for the test
    name=$1
    shift
    echo "=== $name"
    env "$@" timeout 120 sh "$HERE/run.sh" "$BIN/${TEST:-swapchain_test}" >"$TMP/$name.log" 2>&1
    rc=$?
    if [ "${TEST:-}" = fshack_test ]; then
        # one line per swapchain otherwise (thousands when the recreation loop is not stopped)
        grep -E '^(screen|window [0-9]|B8G8|frame |[0-9]+ frames|FS-)|times within a second|again|failed' "$TMP/$name.log"
        echo "($(grep -c '^hybris-wsi: swapchain [0-9]*x[0-9]* format' "$TMP/$name.log") swapchains logged)"
    else
        grep -v -E '^WARNING: linker|normalize_path|library "lib(binder_ndk|nativewindow|sync).so" not found|MEMPROF' "$TMP/$name.log"
    fi
    [ $rc -eq 0 ] || failed="$failed $name"
}

run_zc() { # name, MOCK_RELEASE, then environment for the test
    name=$1 release=$2
    shift 2
    sock=$TMP/present-$name.sock
    MOCK_RELEASE=$release sh "$HERE/run.sh" "$BIN/mock_compositor" "$sock" 1 >"$TMP/mock-$name.log" 2>&1 &
    mpid=$!
    i=0
    while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    run "$name" VORTEK_PRESENT_SOCKET="$sock" "$@"
    i=0
    while kill -0 $mpid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill $mpid 2>/dev/null
    grep '^MOCK' "$TMP/mock-$name.log"
    grep -q 'MOCK-RESULT ok' "$TMP/mock-$name.log" || failed="$failed $name(mock)"
}

run copy SC_EXPECT_COPY=1
run copy-rgba SC_EXPECT_COPY=1 SC_FORMAT=rgba
run_zc zero-copy early DROIDDECK_HYBRIS_AHB_CPU_READ=1 VORTEK_X11_STATS=1
run_zc zero-copy-late late DROIDDECK_HYBRIS_AHB_CPU_READ=1
run_zc zero-copy-shared-queue early DROIDDECK_HYBRIS_AHB_CPU_READ=1 SC_QUEUES=4 SC_SUBMIT_THREAD=1
grep -q 'a shared queue for acquire' "$TMP/zero-copy-shared-queue.log" || failed="$failed zero-copy-shared-queue(not-shared)"
export MOCK_CHECK=0
run_zc zero-copy-afbc early
unset MOCK_CHECK
run resize SC_RESIZE=1 SC_EXPECT_COPY=1
export MOCK_GAPS=1
run_zc cover-partly early SC_COVER=partly SC_FRAMES=180 DROIDDECK_HYBRIS_AHB_CPU_READ=1
grep -q 'now through X11, partly covered by' "$TMP/cover-partly.log" || failed="$failed cover-partly(no-X11-path)"
run_zc cover-fully early SC_COVER=fully SC_FRAMES=180 DROIDDECK_HYBRIS_AHB_CPU_READ=1 VORTEK_COVERED_FPS=10
grep -q 'now hidden, frames paced' "$TMP/cover-fully.log" || failed="$failed cover-fully(not-paced)"
run_zc cover-fully-default early SC_COVER=fully SC_FRAMES=30 DROIDDECK_HYBRIS_AHB_CPU_READ=1
grep -q 'now hidden, frames paced' "$TMP/cover-fully-default.log" || failed="$failed cover-fully-default(not-paced)"
grep -q 'hidden windows paced to 1 a second' "$TMP/cover-fully-default.log" || failed="$failed cover-fully-default(not-1)"
unset MOCK_GAPS
export TEST=fshack_test
run_zc fshack early DROIDDECK_HYBRIS_AHB_CPU_READ=1 FS_MODE=fshack
run fshack-copy FS_MODE=fshack
run_zc fshack-plain early DROIDDECK_HYBRIS_AHB_CPU_READ=1 FS_MODE=plain
run fshack-windowed FS_MODE=plain FS_LAYOUT=offscreen FS_APP=1024x768
run recreate-loop FS_MODE=fixed FS_RESIZE=1 FS_FRAMES=300
grep -q 'created 11 times within a second with the same parameters' "$TMP/recreate-loop.log" || failed="$failed recreate-loop(not-logged)"
grep -q 'reports VK_SUBOPTIMAL_KHR again' "$TMP/recreate-loop.log" || failed="$failed recreate-loop(no-again)"
unset TEST
if command -v vkcube >/dev/null; then
    # A real application (textures, depth, its own swapchain handling); its last frame goes to vkcube.ppm.
    sock=$TMP/present-vkcube.sock
    MOCK_CHECK=0 MOCK_DUMP="$BIN/vkcube.ppm" sh "$HERE/run.sh" "$BIN/mock_compositor" "$sock" 1 >"$TMP/mock-vkcube.log" 2>&1 &
    mpid=$!
    i=0
    while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    echo "=== vkcube"
    VORTEK_PRESENT_SOCKET=$sock DROIDDECK_HYBRIS_AHB_CPU_READ=1 timeout 120 sh "$HERE/run.sh" vkcube --c 300 >"$TMP/vkcube.log" 2>&1 || failed="$failed vkcube"
    grep -E '^hybris' "$TMP/vkcube.log"
    i=0
    while kill -0 $mpid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    grep '^MOCK' "$TMP/mock-vkcube.log"
    grep -q 'MOCK-RESULT ok' "$TMP/mock-vkcube.log" || failed="$failed vkcube(mock)"
fi

kill $xpid 2>/dev/null
wait $xpid 2>/dev/null
rm -rf "$TMP"
if [ -n "$failed" ]; then
    echo "FAILED:$failed"
    exit 1
fi
echo "ALL PASSED"
