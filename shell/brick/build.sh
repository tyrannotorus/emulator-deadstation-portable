#!/bin/sh
# Cross-build Deadstation Portable for NextUI's tg5040 handhelds (TrimUI Brick, aarch64) into build-brick/deadstation.
# Needs:
#  - the toolchain (ui/toolchain/toolchain.sh), in TG5040_ROOT or build-brick/toolchain
#  - FFmpeg for the Brick (shell/brick/ffmpeg.sh), in build-brick/ffmpeg
#  - the host's Vulkan headers (vulkan-headers; the sysroot has none) and glslangValidator (glslang)
#  - the device's libcurl.so.4 (ui/toolchain/devlibs.sh SERIAL), in TG5040_DEVLIBS or build-brick/devlibs, and the host's curl
#    headers (curl); the sysroot has neither
#  - clang, lld, cmake and ninja on the host
# Arguments go to ninja.
set -e
cd "$(dirname "$0")/../.."
B=build-brick/deadstation
TC=${TG5040_ROOT:-$PWD/build-brick/toolchain}
[ -d "$TC/aarch64-nextui-linux-gnu/libc" ] || { echo "brick: no toolchain (ui/toolchain/toolchain.sh)"; exit 2; }
[ -f build-brick/ffmpeg/lib/libavcodec.a ] || { echo "brick: no FFmpeg (shell/brick/ffmpeg.sh)"; exit 2; }
mkdir -p build-brick/include
CURLLIB=${TG5040_DEVLIBS:-$PWD/build-brick/devlibs}/libcurl.so.4
[ -f "$CURLLIB" ] || { echo "brick: no $CURLLIB (ui/toolchain/devlibs.sh SERIAL)"; exit 2; }
rm -rf build-brick/include/vulkan build-brick/include/vk_video build-brick/include/curl
cp -r /usr/include/vulkan /usr/include/vk_video /usr/include/curl build-brick/include/
# pkg-config sees the sysroot's packages only, never the host's
SYSROOT="$TC/aarch64-nextui-linux-gnu/libc"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH
mkdir -p "$B"
cmake -S . -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/ui/toolchain/tg5040.cmake" \
    -DTG5040_ROOT="$TC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SKIP_RPATH=ON \
    -DSDL2_DIR="$PWD/shell/brick/cmake" \
    -DDEADSTATION_FFMPEG="$PWD/build-brick/ffmpeg" \
    -DDEADSTATION_CURL_INCLUDE="$PWD/build-brick/include" \
    -DDEADSTATION_CURL_LIBRARY="$CURLLIB" > "$B.configure.log"
ninja -C "$B" "$@" deadstation-portable
echo "brick: built $B/core/deadstation/deadstation-portable"
