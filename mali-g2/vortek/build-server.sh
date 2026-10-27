#!/bin/sh
# Build the bionic vortek-server with the Android NDK, after ./fetch.sh.
#
#   ./build-server.sh <output-file>   default ./out/libvortekserver.so
#
# The output is a PIE executable named lib*.so so that the package manager
# extracts it into the app's nativeLibraryDir, from where the app may exec it
# (the same trick DroidDeck uses for libdirectaudiorelay.so). It runs from /data,
# which puts it in the linker's "system" section where the exported "sphal"
# namespace (vendor Vulkan HAL) is reachable.
#
#   NDK   NDK root (default $ANDROID_NDK_LATEST_HOME, $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT)
#   API   target API level (default 30)
#   JOBS  parallel compiles (default 2)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}
OUTFILE=${1:-$HERE/out/libvortekserver.so}
API=${API:-30}
JOBS=${JOBS:-2}
NDK=${NDK:-${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}}
[ -n "$NDK" ] && [ -d "$NDK/toolchains/llvm/prebuilt" ] || { echo "no NDK (set NDK)"; exit 1; }
TC=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)

CPP=$WORK/winlator-app/app/src/main/cpp
VR=$CPP/vortekrenderer
WL=$CPP/winlator
OBJ=${OBJ:-$HERE/out/obj-server}
rm -rf "$OBJ"
mkdir -p "$OBJ" "$(dirname "$OUTFILE")"

CC="$TC/clang --target=aarch64-linux-android$API"
CFLAGS="-O2 -g0 -fPIE -fno-stack-protector -mno-outline-atomics
 -DVORTEK_STANDALONE=1 -DVK_USE_PLATFORM_ANDROID_KHR
 -Wno-unused-function -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types
 -Wno-pointer-sign -Wno-format -Wno-deprecated-non-prototype -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 -I$VR/include -I$WL/include -I$HERE/src"

SRCS="$VR/src/vk_object.c $VR/src/request_handler.c $VR/src/vulkan_helper.c $VR/src/vk_context.c
 $VR/src/resource_memory.c $VR/src/texture_decoder.c $VR/src/shader_inspector.c
 $VR/src/async_pipeline_creator.c $VR/src/timeline_semaphore.c
 $WL/src/arrays.c $WL/src/ring_buffer.c
 $HERE/src/standalone_main.c $HERE/src/headless_swapchain.c $HERE/src/zero_copy.c"

export CC CFLAGS OBJ
# shellcheck disable=SC2086
printf '%s\n' $SRCS | xargs -P "$JOBS" -I{} sh -c '
  o=$OBJ/$(basename "$1" .c).o
  echo "  CC $(basename "$1")"
  $CC $CFLAGS -c "$1" -o "$o"' _ {}

OBJS=""
for s in $SRCS; do OBJS="$OBJS $OBJ/$(basename "$s" .c).o"; done

# Only public NDK libraries: android_get_exported_namespace (libdl_android) is
# looked up at run time by standalone_main.c.
# shellcheck disable=SC2086
$CC -fPIE -pie -Wl,-z,max-page-size=16384 -Wl,-z,now -Wl,--as-needed -Wl,--hash-style=both \
  $OBJS -ldl -lm -llog -lnativewindow -o "$OUTFILE"
"$TC/llvm-strip" --strip-unneeded "$OUTFILE"
echo "built $OUTFILE"
"$TC/llvm-readelf" -h -d "$OUTFILE" | grep -E 'Type:|NEEDED'
