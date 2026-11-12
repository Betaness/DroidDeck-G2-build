#!/bin/sh
# prof-device.sh (runs on the host, talks to the device over adb) <command> [args]
# The adb half of the main-thread profile (README.md). Android's own simpleperf, started from adb with --app,
# runs as the app (simpleperf_app_runner switches to the package's uid and SELinux domain, then the simpleperf
# domain): that needs the package to be profileable from the shell (G2's manifest has
# <profileable android:shell="true"/>) and reaches every process of the app's uid, so also the guest's game
# process under proot. Nothing here needs root.
#   check [PID]                       Android version, perf_event_paranoid and the LSM-hook property, simpleperf,
#                                     whether the package is profileable; with PID a 1 s `simpleperf stat` on that
#                                     thread (cpu-clock, then cpu-cycles,instructions) to show it is reachable
#   record TAG TIDS SECONDS [EVENT] [FREQ] [-- simpleperf record args]
#                                     simpleperf record --app on the comma-separated TIDS (default event cpu-clock,
#                                     4000 Hz), then on the device `simpleperf dump` (gzip) and `simpleperf report`
#                                     (per thread, file and symbol), pulled into $OUT/TAG/ and deleted there
#   stat TIDS SECONDS [EVENTS]        simpleperf stat --per-thread on TIDS (default cpu-cycles,instructions,
#                                     branch-misses,cache-references,cache-misses), output to stdout
#   clean                             delete $D on the device
# env: PKG (com.droiddeck.launcher.g2), OUT (./fexprof-out), D (/data/local/tmp/fexprof), ADB (adb)
set -u
PKG=${PKG:-com.droiddeck.launcher.g2}
OUT=${OUT:-./fexprof-out}
D=${D:-/data/local/tmp/fexprof}
ADB=${ADB:-adb}

sh_dev() { "$ADB" shell "$@"; }

case ${1:-} in
  check)
    sh_dev "echo SDK \$(getprop ro.build.version.sdk) RELEASE \$(getprop ro.build.version.release) TYPE \$(getprop ro.build.type);
      echo PERF_HARDEN \$(getprop security.perf_harden) LSM_HOOKS \$(getprop sys.init.perf_lsm_hooks);
      echo PARANOID \$(cat /proc/sys/kernel/perf_event_paranoid) MAX_RATE \$(cat /proc/sys/kernel/perf_event_max_sample_rate) CPU_PCT \$(cat /proc/sys/kernel/perf_cpu_time_max_percent);
      ls -l /system/bin/simpleperf /system/bin/simpleperf_app_runner 2>&1; simpleperf --version 2>&1 | head -2"
    echo "PROFILEABLE $(sh_dev dumpsys package "$PKG" | grep -i -o -E 'PROFILEABLE[A-Z_]*|profileable[A-Za-z]*=[a-z]*' | sort -u | tr '\n' ' ')"
    if [ -n "${2:-}" ]; then
      for ev in cpu-clock cpu-cycles,instructions; do
        echo "STAT $ev on tid $2:"
        sh_dev "simpleperf stat --app $PKG -t $2 -e $ev --duration 1 2>&1" | tail -8
      done
    fi ;;
  record)
    [ $# -ge 4 ] || { echo "record TAG TIDS SECONDS [EVENT] [FREQ] [-- args]"; exit 2; }
    tag=$2; tids=$3; secs=$4; ev=cpu-clock; freq=4000
    shift 4
    if [ $# -gt 0 ] && [ "$1" != "--" ]; then ev=$1; shift; fi
    if [ $# -gt 0 ] && [ "$1" != "--" ]; then freq=$1; shift; fi
    if [ $# -gt 0 ] && [ "$1" = "--" ]; then shift; fi
    case $tag in *[!A-Za-z0-9._-]*|"") echo "TAG: letters, digits, . _ - only"; exit 2 ;; esac
    mkdir -p "$OUT/$tag" || exit 1
    sh_dev "mkdir -p $D && rm -f $D/$tag.*"
    echo "RECORD $tag: tids $tids, $secs s, $ev at $freq Hz $*"
    sh_dev "simpleperf record --app $PKG -t $tids -e $ev -f $freq --duration $secs -o $D/$tag.data $* 2>&1" | tee "$OUT/$tag/record.log" | tail -5
    sh_dev "test -s $D/$tag.data" || { echo "no $D/$tag.data on the device (see $OUT/$tag/record.log)"; exit 1; }
    sh_dev "simpleperf dump -i $D/$tag.data > $D/$tag.dump.txt 2>$D/$tag.dump.err; gzip -f $D/$tag.dump.txt 2>/dev/null;
            simpleperf report -i $D/$tag.data --sort tid,comm,dso,symbol --percent-limit 0.05 > $D/$tag.report.txt 2>&1;
            simpleperf report -i $D/$tag.data --sort tid,dso > $D/$tag.report-dso.txt 2>&1; ls -l $D/$tag.*" | tee "$OUT/$tag/device-files.txt"
    for f in $tag.data $tag.dump.txt.gz $tag.dump.txt $tag.dump.err $tag.report.txt $tag.report-dso.txt; do
      sh_dev "test -e $D/$f" && "$ADB" pull "$D/$f" "$OUT/$tag/$f" >/dev/null
    done
    sh_dev "rm -f $D/$tag.*"
    ls -l "$OUT/$tag" ;;
  stat)
    [ $# -ge 3 ] || { echo "stat TIDS SECONDS [EVENTS]"; exit 2; }
    sh_dev "simpleperf stat --app $PKG -t $2 --per-thread -e ${4:-cpu-cycles,instructions,branch-misses,cache-references,cache-misses} --duration $3 2>&1" ;;
  clean)
    sh_dev "rm -rf $D" && echo "ok removed $D" ;;
  *) sed -n '2,22p' "$0"; exit 2 ;;
esac
