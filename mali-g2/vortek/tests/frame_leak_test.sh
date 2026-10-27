#!/bin/sh
# Runs frame_leak_test against a Vortek server (zero-copy presenter: zc_mock_compositor, on a private
# Xvfb) and reports the server's malloc heap and how it grows per frame. Run on the device, like
# zc_acquire_test.sh:
#   frame_leak_test.sh <libvortekserver.so> <client-dir> <frame_leak_test> <zc_mock_compositor>
# <client-dir> is a build-client.sh output. FL_* pass through to the test (default here: a DXVK-like
# frame with 16 render passes and a timestamp query, FL_STEPS=timeline,submit2,draw,query,present,poolreset
# FL_PASSES=16 FL_FRAMES=12000), VORTEK_* to the server (VORTEK_CMD_RELEASE_LOG=1 is set: every
# release is counted). FL_EXPECT (default bounded) and FL_MAX_GROWTH (default 64 bytes per frame) as
# in frame_leak_report.awk: bounded = no growth over the last half of the reports, the default
# (release by the estimate, cmd_memory.h) and VORTEK_CMD_RELEASE_EVERY=N >= 1 pass; FL_MAX_PEAK_KB and
# FL_MAX_RELEASES (per 1000 frames) add limits. Prints "server heap: ..." (first report, peak, steady
# state, growth, releases per 1000 frames, server CPU per frame) and FRAMELEAK-PASS or FRAMELEAK-FAIL.
# Without RELEASE_RESOURCES (VORTEK_CMD_RELEASE_EVERY=0, FL_EXPECT=grow) the default frame grows the
# server by about 6.7 KB per frame on the Mali-G2. For the default 16m per pool, FL_FRAMES=12000
# reaches the plateau in the first half (2 pools x 16 MiB at 6.7 KB per frame: ~5000 frames).
set -u
srv=$1 cdir=$2 test=$3 mock=$4
id=framelk-$$
dir=/data/local/vortek
tmp=$(mktemp -d /tmp/$id.XXXX)
sock=$tmp/server.sock psock=$tmp/present.sock
: "${FL_STEPS:=timeline,submit2,draw,query,present,poolreset}" "${FL_PASSES:=16}" "${FL_FRAMES:=12000}" "${FL_REPORT:=1000}"
export FL_STEPS FL_PASSES FL_FRAMES FL_REPORT

disp=90
while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >$tmp/xvfb.log 2>&1 &
xpid=$!

cp "$mock" $dir/$id-mock && chmod 700 $dir/$id-mock
$dir/$id-mock $psock >$tmp/mock.log 2>&1 &
mpid=$!
cp "$srv" $dir/$id-srv && chmod 700 $dir/$id-srv
VORTEK_CMD_RELEASE_LOG=1 VORTEK_PRESENT_SOCKET=$psock $dir/$id-srv $sock >$tmp/server.log 2>&1 &
spid=$!

i=0
while { [ ! -S $sock ] || [ ! -S $psock ] || [ ! -e /tmp/.X11-unix/X$disp ]; } && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$cdir/libvulkan_vortek.so\"#" "$cdir/vortek_icd.aarch64.json" >$tmp/icd.json
DISPLAY=:$disp VORTEK_SERVER_PATH=$sock VK_ICD_FILENAMES=$tmp/icd.json VK_DRIVER_FILES=$tmp/icd.json FL_SERVER_PID=$spid \
  timeout 600 "$test" >$tmp/client.log 2>&1
rc=$?

kill $spid 2>/dev/null; wait $spid 2>/dev/null
kill $mpid $xpid 2>/dev/null; wait $mpid $xpid 2>/dev/null
grep -E "^FL (frame|done)|FAIL" $tmp/client.log
grep -E "VORTEK_CMD_RELEASE_EVERY|command memory: (rel|.*full)|abort|Scudo" $tmp/server.log | head -6
verdict="server $(awk -v expect="${FL_EXPECT:-bounded}" -v max="${FL_MAX_GROWTH:-64}" -v maxpeak="${FL_MAX_PEAK_KB:-}" \
  -v maxrel="${FL_MAX_RELEASES:-}" -f "$(dirname "$0")/frame_leak_report.awk" $tmp/client.log $tmp/server.log)"
echo "$verdict"
rm -f $dir/$id-mock $dir/$id-srv
rm -rf $tmp
case $verdict in *": PASS"*) ;; *) rc=1 ;; esac
if [ $rc -ne 0 ]; then echo FRAMELEAK-FAIL; exit 1; fi
echo FRAMELEAK-PASS
