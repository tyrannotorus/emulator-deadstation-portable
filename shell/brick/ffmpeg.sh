#!/bin/sh
# FFmpeg for the Brick, from the source in ffmpeg/ (PPSSPP's FFmpeg): the PSP's movies and some of its sounds, static, into build-brick/ffmpeg.
# Only what a PSP game plays. Needs the toolchain (shell/brick/build.sh says how).
set -e
cd "$(dirname "$0")/../.."
ROOT=$PWD
TC=${TG5040_ROOT:-$ROOT/build-brick/toolchain}
SYSROOT="$TC/aarch64-nextui-linux-gnu/libc"
[ -d "$SYSROOT" ] || { echo "ffmpeg: no toolchain at $TC"; exit 2; }
OUT=$ROOT/build-brick/ffmpeg
B=$ROOT/build-brick/ffmpeg-build
rm -rf "$B" "$OUT"
mkdir -p "$B"
cd "$B"
FLAGS="--target=aarch64-nextui-linux-gnu --sysroot=$SYSROOT --gcc-toolchain=$TC -mcpu=cortex-a53"
"$ROOT/ffmpeg/configure" --prefix="$OUT" --target-os=linux --arch=aarch64 --cpu=cortex-a53 --enable-cross-compile \
	--cc=clang --cxx=clang++ --as=clang --ld=clang --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip \
	--extra-cflags="$FLAGS -O3 -fno-strict-aliasing -ffile-prefix-map=$ROOT=. -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-everything" \
	--extra-ldflags="$FLAGS -fuse-ld=lld" \
	--disable-shared --enable-static --enable-pic --disable-debug --disable-doc --disable-programs --disable-everything --disable-iconv \
	--disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-hwaccels --disable-sdl --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --disable-vdpau \
	--enable-decoder=h264 --enable-decoder=mpeg4 --enable-decoder=mpeg2video --enable-decoder=mjpeg --enable-decoder=mjpegb \
	--enable-decoder=aac --enable-decoder=aac_latm --enable-decoder=atrac3 --enable-decoder=atrac3p --enable-decoder=mp3 --enable-decoder=pcm_s16le --enable-decoder=pcm_s8 \
	--enable-demuxer=h264 --enable-demuxer=m4v --enable-demuxer=mpegvideo --enable-demuxer=mpegps --enable-demuxer=mp3 --enable-demuxer=avi --enable-demuxer=aac --enable-demuxer=pmp --enable-demuxer=oma --enable-demuxer=pcm_s16le --enable-demuxer=pcm_s8 --enable-demuxer=wav \
	--enable-parser=h264 --enable-parser=mpeg4video --enable-parser=mpegaudio --enable-parser=mpegvideo --enable-parser=aac --enable-parser=aac_latm \
	> "$B.configure.log" 2>&1 || { tail -30 "$B.configure.log"; tail -30 config.log 2>/dev/null; exit 1; }
# no path of this machine in the library: its build settings are not kept
sed -i 's/^#define FFMPEG_CONFIGURATION .*/#define FFMPEG_CONFIGURATION ""/' config.h
make -j"$(nproc)" > "$B.make.log" 2>&1 || { tail -30 "$B.make.log"; exit 1; }
make install > /dev/null 2>&1
echo "ffmpeg: $OUT"
ls -la "$OUT/lib"
