#!/bin/sh
# Exact requested inventory for PROFILE [TARGET_ARCH]. Output is comma-separated.
# MD_FFMPEG_ARCH wins over compiler inference; cross-prefix and CC wrappers use
# the same trusted compiler command convention as FFmpeg configure itself.
set -eu
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || { echo 'usage: profile-encoders.sh PROFILE [TARGET_ARCH]' >&2; exit 2; }
arch=${2:-${MD_FFMPEG_ARCH:-}}
if [ -z "$arch" ]; then
    compiler=${CC:-cc}
    if [ -n "${MD_FFMPEG_CROSS_PREFIX:-}" ]; then
        compiler=${CC:-${MD_FFMPEG_CROSS_PREFIX}gcc}
    fi
    arch=$(sh -c "$compiler -dumpmachine") || { echo 'cannot infer target; set MD_FFMPEG_ARCH' >&2; exit 1; }
fi
case "$1" in
    macos) encoders=h264_videotoolbox ;;
    windows) encoders=h264_nvenc,h264_amf,h264_mf ;;
    container|flatpak) encoders=h264_nvenc,h264_amf,h264_vaapi ;;
    *) echo "unknown profile: $1" >&2; exit 2 ;;
esac
if [ "$1" != macos ]; then
    case "$arch" in
        x86_64|x86_64-*|amd64|amd64-*|x86|x86-*|i386|i386-*|i486|i486-*|i586|i586-*|i686|i686-*)
            encoders=$encoders,h264_qsv ;;
    esac
fi
printf '%s\n' "$encoders"
