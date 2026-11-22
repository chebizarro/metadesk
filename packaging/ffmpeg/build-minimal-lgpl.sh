#!/bin/sh
# Build only shared avcodec/avutil, native H.264 decode/parser and platform encoders.
# Usage: build-minimal-lgpl.sh macos|windows|container|flatpak SOURCE OUTPUT
# SOURCE: clean, already extracted, pinned FFmpeg source (not downloaded here).
# OUTPUT: NEW absolute directory OUTSIDE SOURCE and the metadesk worktree.
# Installed libraries: OUTPUT/install; logs/config: OUTPUT/build, OUTPUT/*.txt.
# Native builds by default. Cross builds: MD_FFMPEG_TARGET_OS, MD_FFMPEG_ARCH,
# MD_FFMPEG_CROSS_PREFIX; CC/AR/PKG_CONFIG select the toolchain. Header/library
# search paths: MD_FFMPEG_CFLAGS / MD_FFMPEG_LDFLAGS. No arbitrary configure flags.
# Windows needs nv-codec-headers + AMF headers + oneVPL, and Media Foundation SDK.
# Linux needs nv-codec-headers + AMF headers + libva/libdrm + oneVPL.
set -eu
fail() { echo "$*" >&2; exit 1; }
[ "$#" = 3 ] || fail "usage: $0 macos|windows|container|flatpak SOURCE OUTPUT"
profile=$1
source=$(cd "$2" && pwd -P)
[ -f "$source/configure" ] || fail "not an FFmpeg source directory: $source"
[ ! -e "$source/ffbuild/config.mak" ] || fail "use a clean, unconfigured source tree"
case "$3" in /*) ;; *) fail "OUTPUT must be absolute" ;; esac
[ ! -e "$3" ] || fail "OUTPUT must not exist: $3"
case "$profile" in
    macos) encoders=h264_videotoolbox ;;
    windows) encoders=h264_nvenc,h264_amf,h264_mf,h264_qsv ;;
    container|flatpak) encoders=h264_nvenc,h264_amf,h264_vaapi,h264_qsv ;;
    *) fail "unknown profile (distro packages use system FFmpeg): $profile" ;;
esac
# Resolve the parent before creating anything (including symlink components).
parent=$(cd "$(dirname "$3")" && pwd -P)
out=$parent/$(basename "$3")
repo=$(cd "$(dirname "$0")/../.." && pwd -P)
case "$out/" in "$source/"*|"$repo/"*) fail "OUTPUT must be outside both source trees" ;; esac
mkdir "$out"
mkdir "$out/build"
prefix=$out/install
set -- --prefix="$prefix" --disable-everything --disable-autodetect \
    --disable-gpl --disable-nonfree --disable-version3 \
    --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-avdevice --disable-avfilter --disable-avformat \
    --disable-swresample --disable-swscale --enable-avcodec --enable-avutil \
    --enable-decoder=h264 --enable-parser=h264 --enable-encoder="$encoders"
case "$profile" in
    macos) set -- "$@" --enable-videotoolbox ;;
    windows) set -- "$@" --enable-ffnvcodec --enable-nvenc --enable-amf \
        --enable-mediafoundation --enable-libvpl ;;
    container|flatpak) set -- "$@" --enable-ffnvcodec --enable-nvenc --enable-amf \
        --enable-vaapi --enable-libdrm --enable-libvpl ;;
esac
[ -z "${MD_FFMPEG_TARGET_OS:-}" ] || set -- "$@" --target-os="$MD_FFMPEG_TARGET_OS"
[ -z "${MD_FFMPEG_ARCH:-}" ] || set -- "$@" --arch="$MD_FFMPEG_ARCH"
[ -z "${MD_FFMPEG_CROSS_PREFIX:-}" ] || set -- "$@" --enable-cross-compile --cross-prefix="$MD_FFMPEG_CROSS_PREFIX"
[ -z "${CC:-}" ] || set -- "$@" --cc="$CC"
[ -z "${AR:-}" ] || set -- "$@" --ar="$AR"
[ -z "${PKG_CONFIG:-}" ] || set -- "$@" --pkg-config="$PKG_CONFIG"
[ -z "${MD_FFMPEG_CFLAGS:-}" ] || set -- "$@" --extra-cflags="$MD_FFMPEG_CFLAGS"
[ -z "${MD_FFMPEG_LDFLAGS:-}" ] || set -- "$@" --extra-ldflags="$MD_FFMPEG_LDFLAGS"
# One argument per line, preserving paths with spaces; retain with source package.
printf '%s\n' "$source/configure" "$@" > "$out/configure-args.txt"
printf '%s\n' "$profile" > "$out/profile.txt"
cd "$out/build"
if ! "$source/configure" "$@" > "$out/configure.txt" 2>&1; then
    cat "$out/configure.txt" >&2
    fail "configure failed; see $out/build/ffbuild/config.log"
fi
cat "$out/configure.txt"
grep -qx 'License: LGPL version 2.1 or later' "$out/configure.txt" || fail "not LGPL-2.1-or-later"
# FFmpeg may warn and silently disable an explicitly requested codec. Fail closed.
for feature in H264_DECODER H264_PARSER $(printf '%s' "$encoders" | tr ',' ' ' | tr '[:lower:]' '[:upper:]' | sed 's/[^ ]*/&_ENCODER/g'); do
    grep -qx "#define CONFIG_${feature} 1" config_components.h || fail "missing $feature"
done
for feature in GPL NONFREE VERSION3 LIBX264 LIBOPENH264; do
    grep -qx "#define CONFIG_${feature} 0" config.h || fail "unexpected $feature"
done
make -j "${MD_FFMPEG_JOBS:-2}" > "$out/build.txt" 2>&1 || { tail -60 "$out/build.txt" >&2; fail "build failed"; }
make install > "$out/install.txt" 2>&1 || { tail -60 "$out/install.txt" >&2; fail "install failed"; }
cp "$source/COPYING.LGPLv2.1" "$prefix/"
printf '\nBuilt %s: LGPL-2.1-or-later; decoder/parser=h264; encoders=%s\n' "$prefix" "$encoders"
