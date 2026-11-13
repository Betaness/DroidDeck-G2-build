#!/usr/bin/env bash
set -euo pipefail
D=${1:?asset directory}
NEED=$("${READELF:-readelf}" -d "$D/libfakeinput.so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
for bad in libstdc++.so.6 libgcc_s.so.1; do
  if printf '%s\n' "$NEED" | grep -qx "$bad"; then
    echo "::error::libfakeinput.so links $bad - the C++ runtime must stay static"; exit 1
  fi
done
syms() { "${READELF:-readelf}" -Ws "$1" | awk '$4 == "FUNC" && $5 == "GLOBAL" {sub(/@.*/, "", $8); print $8}'; }
FAKE=$(syms "$D/libfakeinput.so")
for sym in open openat ioctl read readv close poll ppoll select stat fstat access scandir; do
  grep -qx "$sym" <<<"$FAKE" || { echo "::error::libfakeinput.so does not export $sym"; exit 1; }
done
if grep -qxE '__gxx_personality_v0|__cxa_throw|_Unwind_Resume' <<<"$FAKE"; then
  echo "::error::libfakeinput.so exports the C++ runtime"; exit 1
fi
SESSION=$(syms "$D/libblsession.so")
for sym in socket bind getsockname setsockopt statfs statvfs syscall shm_open shm_unlink mmap mmap64 munmap drmPrimeFDToHandle drmCloseBufferHandle; do
  grep -qx "$sym" <<<"$SESSION" || { echo "::error::libblsession.so does not export $sym"; exit 1; }
done
if grep -qx drmIoctl <<<"$SESSION"; then
  echo "::error::libblsession.so exports drmIoctl"; exit 1
fi
# With LD_AUDIT set (droiddeck-proton), glibc loads the preloads after it has sized the static TLS block, so
# initial-exec TLS in libblsession.so fails every program with "cannot allocate memory in static TLS block".
for lib in "$D/libblsession.so" "$D/x86_64/libblsession.so" "$D/i386/libblsession.so"; do
  [ -f "$lib" ] || continue
  REL=$("${READELF:-readelf}" -rW "$lib")
  DYN=$("${READELF:-readelf}" -dW "$lib")
  if grep -qE 'TLS_TPREL|_TPOFF' <<<"$REL" || grep -q STATIC_TLS <<<"$DYN"; then
    echo "::error::${lib#"$D"/} needs static TLS (initial-exec)"; exit 1
  fi
done
SSBS=$(syms "$D/libssbs.so")
for sym in sigaction signal bsd_signal sysv_signal; do
  grep -qx "$sym" <<<"$SSBS" || { echo "::error::libssbs.so does not export $sym"; exit 1; }
done
XCLIPSE=$(syms "$D/usr/local/lib/droiddeck-xclipse/libVkLayer_droiddeck_xclipse.so")
for sym in vkNegotiateLoaderLayerInterfaceVersion vkGetInstanceProcAddr vkGetDeviceProcAddr; do
  grep -qx "$sym" <<<"$XCLIPSE" || { echo "::error::the Xclipse layer does not export $sym"; exit 1; }
done
test -f "$D/usr/local/lib/droiddeck-xclipse/VkLayer_droiddeck_xclipse.json"
test -f "$D/usr/local/bin/droiddeck-session"
test -f "$D/usr/local/bin/droiddeck-proton-extra"
