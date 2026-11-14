# fg-bench-guest.sh (runs in the DroidDeck guest: droiddeckctl guest, or /tmp/ab/cpu/g.sh) <command> [args]
# ZeroFG's generation cost on the tablet's Mali, measured in the guest through the in-process driver (mali-g2/hybris).
#   install       copy fg-bench (and this script, when it is there too) from $INBOX (the app's agent-inbox: adb push
#                 them there first) to $Q; then `sh $Q/fg-bench-guest.sh run` works from any guest shell
#   info          what the run needs: the hybris ICD files, /system/lib64/libvulkan.so (bound in for hybris
#                 sessions), a guest Vulkan loader, the guest's glibc; fg-bench --list with the hybris ICD
#   run [args]    $Q/fg-bench [args] with the environment droiddeck-game-env gives hybris games: VK_ICD_FILENAMES and
#                 VK_DRIVER_FILES = the hybris ICD, libhybris-common.so.1 first in LD_PRELOAD,
#                 glibc.rtld.optional_static_tls=16384 in GLIBC_TUNABLES. GPU=vortek: the guest's own Vulkan
#                 environment unchanged (Vortek: every call forwarded to the app). No args: fg-bench's defaults
#                 (1704x1136 and 3408x2272, ReallyZero and Zero, 10+60 iterations, BGRA8 in, RGBA8 out) plus --json.
#   clean         delete $Q and the agent-inbox copy
# env: INBOX, Q (~/.cache/droiddeck-fgbench), GPU (hybris | vortek), FGBENCH_LOADER (a libvulkan.so.1 to use).
INBOX=${INBOX:-/storage/emulated/0/Android/data/com.droiddeck.launcher.g2/files/agent-inbox}
Q=${Q:-$HOME/.cache/droiddeck-fgbench}
H=/usr/local/lib/droiddeck-hybris
ICD=$H/droiddeck_hybris_icd.aarch64.json
PRE=$H/lib/libhybris-common.so.1

# A loader the dynamic linker finds by name, else one of the usual places (FGBENCH_LOADER wins).
loader() {
  [ -n "${FGBENCH_LOADER:-}" ] && { echo "$FGBENCH_LOADER"; return; }
  if command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1 '; then echo libvulkan.so.1; return; fi
  for f in /usr/lib/libvulkan.so.1 /usr/lib64/libvulkan.so.1 /usr/lib/aarch64-linux-gnu/libvulkan.so.1 \
           "$HOME/.local/share/Steam/steamrtarm64/libvulkan.so.1"; do
    [ -e "$f" ] && { echo "$f"; return; }
  done
  echo libvulkan.so.1
}

hybris_env() {
  for f in "$ICD" "$PRE" /system/lib64/libvulkan.so; do
    [ -e "$f" ] || { echo "fg-bench-guest: $f is missing (hybris GPU path off in this session?); GPU=vortek runs on Vortek" >&2; exit 2; }
  done
  export VK_ICD_FILENAMES="$ICD" VK_DRIVER_FILES="$ICD"
  rest=$(printf '%s' "${LD_PRELOAD:-}" | tr ': ' '\n\n' | grep -v '^$' | grep -vxF "$PRE" | tr '\n' ':')
  export LD_PRELOAD="$PRE${rest:+:${rest%:}}"
  tun=$(printf '%s' "${GLIBC_TUNABLES:-}" | tr ':' '\n' | grep -v '^$' | grep -v '^glibc.rtld.optional_static_tls=' | tr '\n' ':')
  export GLIBC_TUNABLES="${tun}glibc.rtld.optional_static_tls=16384"
}

case ${1:-} in
  install)
    [ -r "$INBOX/fg-bench" ] || { echo "no $INBOX/fg-bench (adb push it there first)"; exit 1; }
    mkdir -p "$Q" || exit 1
    cp "$INBOX/fg-bench" "$Q/fg-bench.new" && chmod 755 "$Q/fg-bench.new" && mv "$Q/fg-bench.new" "$Q/fg-bench" || exit 1
    if [ -r "$INBOX/fg-bench-guest.sh" ]; then cp "$INBOX/fg-bench-guest.sh" "$Q/fg-bench-guest.sh" || exit 1; fi
    sha256sum "$Q"/fg-bench* | sed 's/^/SHA /' ;;
  info)
    for f in "$ICD" "$PRE" /system/lib64/libvulkan.so "$Q/fg-bench"; do
      if [ -e "$f" ]; then echo "have $f"; else echo "MISSING $f"; fi
    done
    echo "loader $(loader)"
    echo "glibc $(ldd --version 2>/dev/null | head -1)"
    echo "GPU path of this session: ${DROIDDECK_GPU_PATH:-unset}; guest VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-unset}"
    if [ -x "$Q/fg-bench" ] && [ -e "$ICD" ] && [ -e /system/lib64/libvulkan.so ]; then
      ( hybris_env; FGBENCH_LOADER=$(loader) "$Q/fg-bench" --list )
    fi ;;
  run)
    shift
    [ -x "$Q/fg-bench" ] || { echo "no $Q/fg-bench (install first)"; exit 1; }
    [ "${GPU:-hybris}" = vortek ] || hybris_env
    export FGBENCH_LOADER="$(loader)"
    echo "fg-bench-guest: GPU ${GPU:-hybris}, loader $FGBENCH_LOADER, $(ldd --version 2>/dev/null | head -1)"
    if [ $# -eq 0 ]; then exec "$Q/fg-bench" --json; fi
    exec "$Q/fg-bench" "$@" ;;
  clean)
    rm -rf "$Q"
    rm -f "$INBOX/fg-bench" "$INBOX/fg-bench-guest.sh"
    echo "removed $Q and fg-bench, fg-bench-guest.sh from $INBOX" ;;
  *)
    echo "usage: fg-bench-guest.sh install|info|run [fg-bench args]|clean" >&2
    exit 3 ;;
esac
