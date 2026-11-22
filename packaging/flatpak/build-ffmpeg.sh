#!/bin/sh
# Invoked in an isolated flatpak-builder module; consume the canonical policy.
set -eu
out=$(mktemp -d /tmp/md-ffmpeg-parent.XXXXXX)
trap 'rm -rf "$out"' EXIT HUP INT TERM
sh packaging/ffmpeg/build-minimal-lgpl.sh flatpak "$PWD/ffmpeg-source" "$out/output"
cp -a "$out/output/install/"* /app/
# Metadata relocation only. Keep the exact original configure arguments for source compliance.
sed -i -e 's|^prefix=.*|prefix=/app|' -e "s|^libdir=.*|libdir=\${prefix}/lib|" -e "s|^includedir=.*|includedir=\${prefix}/include|" /app/lib/pkgconfig/libav*.pc
install -d /app/share/licenses/ffmpeg
cp ffmpeg-source/COPYING.LGPLv2.1 "$out/output/configure-args.txt" /app/share/licenses/ffmpeg/
python3 - "$out" <<'PYCODE'
import shlex, subprocess, sys
probe = sys.argv[1] + '/verify-codecs'
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libavcodec', 'libavutil'], text=True))
subprocess.run(['cc', 'packaging/ffmpeg/verify-codecs.c', *flags, '-o', probe], check=True)
encoders = subprocess.check_output(['packaging/ffmpeg/profile-encoders.sh', 'flatpak'], text=True).strip().split(',')
subprocess.run([probe, *encoders], check=True)
PYCODE
