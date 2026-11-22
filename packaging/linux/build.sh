#!/bin/sh
# Trusted sources/build inputs only. All outputs must initially be absent.
set -eu
[ "$#" = 5 ] || { echo 'usage: build.sh native|oci SOURCE NOSTRC_SOURCE NOSTRC_BUILD NEW_BUILD' >&2; exit 2; }
mode=$1 src=$(realpath "$2") nostrc=$(realpath "$3") nb=$(realpath "$4") build=$5
[ ! -e "$build" ] || { echo 'build must be new' >&2; exit 2; }
case "$mode" in
  native) prefix=/usr; libdir=lib/metadesk; client=true; native_rpath=true ;;
  oci) prefix=/opt/metadesk; libdir=lib; client=false; native_rpath=false
    : "${MD_FFMPEG_PREFIX:?OCI requires private minimal LGPL FFmpeg}"
    export PKG_CONFIG_PATH="$MD_FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$MD_FFMPEG_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    python3 - "$src" "${build}-codec-probe" <<'PYCODE'
import shlex
import subprocess
import sys
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libavcodec', 'libavutil'], text=True))
subprocess.run(['cc', sys.argv[1] + '/packaging/ffmpeg/verify-codecs.c', *flags, '-o', sys.argv[2]], check=True)
PYCODE
    "${build}-codec-probe" h264_nvenc h264_amf h264_vaapi h264_qsv
    ;;
  *) exit 2 ;;
esac
meson setup "$build" "$src" --prefix="$prefix" --libdir="$libdir" \
  --buildtype=release -Dclient="$client" -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dlinux_package_rpath="$native_rpath" -Dnostrc_root="$nostrc" -Dnostrc_build_dir="$nb"
if [ "$client" = true ]; then
    meson compile -C "$build" -j2 metadesk-host metadesk-client
else
    meson compile -C "$build" -j2 metadesk-host
fi
