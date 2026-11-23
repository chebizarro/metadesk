#!/bin/sh
# Build only selected frontends, never install a host into the sandbox.
set -eu
[ "$#" = 5 ] || { echo 'usage: build-product.sh SOURCE NOSTRC_PREFIX NEW_BUILD PREFIX client|manager' >&2; exit 2; }
src=$(realpath "$1") nostrc=$(realpath "$2") build=$3 prefix=$4 product=$5
case "$product" in client) client=true; manager=false ;; manager) client=false; manager=true ;; *) exit 2 ;; esac
[ ! -e "$build" ] || { echo 'build must be new' >&2; exit 2; }
export PKG_CONFIG_PATH="$nostrc/lib/pkgconfig:$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
meson setup "$build" "$src" --prefix="$prefix" --libdir=lib --buildtype=release \
  -Dclient="$client" -Dmanager="$manager" -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dportal_input=disabled -Dnostrc_discovery=installed
meson compile -C "$build" -j2 "metadesk-$product"
install -d "$prefix/bin" "$prefix/lib" "$prefix/libexec/metadesk" "$prefix/share/licenses/metadesk"
install -m755 "$build/$( [ "$product" = manager ] && printf 'src/manager/' )metadesk-$product" "$prefix/bin/"
# Meson install would also install the host and build unselected targets.
for file in "$build"/libmetadesk.so*; do
  [ -d "$file" ] || cp -P "$file" "$prefix/lib/"
done
cp -P "$nostrc"/lib/libnostr-json.so* "$prefix/lib/"
python3 - "$src" "$prefix" <<'PYCODE'
import os, shlex, subprocess, sys
# Meson supplies its own pkg-config search environment; direct probes must too.
env = dict(os.environ, PKG_CONFIG_PATH=sys.argv[2] + '/lib/pkgconfig:' + os.environ.get('PKG_CONFIG_PATH', ''))
flags = shlex.split(subprocess.check_output(['pkg-config', '--keep-system-cflags', '--keep-system-libs', '--cflags', '--libs', 'libavcodec', 'libavutil'], env=env, text=True))
probe = sys.argv[2] + '/libexec/metadesk/verify-codecs'
subprocess.run(['cc', sys.argv[1] + '/packaging/ffmpeg/verify-codecs.c', *flags, '-o', probe], check=True)
libdir = subprocess.check_output(['pkg-config', '--variable=libdir', 'libavcodec'], env=env, text=True).strip()
env['LD_LIBRARY_PATH'] = libdir + ':' + sys.argv[2] + '/lib'
encoders = subprocess.check_output([sys.argv[1] + '/packaging/ffmpeg/profile-encoders.sh', 'container'], text=True).strip().split(',')
subprocess.run([probe, *encoders], env=env, check=True)
PYCODE
cc "$src/packaging/appimage/tests/decode-smoke.c" -I"$src/src/core" -L"$prefix/lib" -lmetadesk \
  -Wl,-rpath-link,"$prefix/lib" -o "$prefix/libexec/metadesk/decode-smoke"
install -m644 "$src/LICENSE" "$src/THIRD_PARTY_NOTICES.md" "$prefix/share/licenses/metadesk/"
# Selective staging bypasses Meson's install-time RPATH removal.
for file in "$prefix/bin/metadesk-$product" "$prefix/libexec/metadesk/verify-codecs" "$prefix/libexec/metadesk/decode-smoke" "$prefix"/lib/libmetadesk.so.* "$prefix"/lib/libnostr-json.so.*; do
  [ -L "$file" ] || patchelf --remove-rpath "$file"
done
