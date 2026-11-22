#!/bin/sh
# A user-only, private build/repository. Never installs runtimes automatically.
set -eu
[ "$#" = 2 ] || { echo 'usage: build.sh Client|Manager NEW_WORK_DIR' >&2; exit 2; }
case "$1" in Client|Manager) product=$1 ;; *) exit 2 ;; esac
[ ! -e "$2" ] || { echo 'work directory must be new' >&2; exit 2; }
mkdir -p "$2"
work=$(realpath "$2")
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
cd "$work"
flatpak-builder --user --jobs=2 --disable-rofiles-fuse --state-dir="$work/state" \
  --repo="$work/repo" "$work/app" "$here/org.metadesk.$product.json"
# Do not let builder installation update pre-existing shared GL/codecs extensions.
flatpak install --user --noninteractive --or-update --no-related --no-deps "$work/repo" "org.metadesk.$product"
flatpak build-bundle "$work/repo" "$work/org.metadesk.$product.flatpak" "org.metadesk.$product"
flatpak run --user "org.metadesk.$product" --help
python3 - "$here" "$product" <<'PYCODE'
import subprocess, sys
arch = subprocess.check_output(['flatpak', '--default-arch'], text=True).strip()
encoders = subprocess.check_output([sys.argv[1] + '/../ffmpeg/profile-encoders.sh', 'flatpak', arch], text=True).strip().split(',')
subprocess.run(['flatpak', 'run', '--user', '--command=/app/libexec/metadesk/verify-codecs',
                'org.metadesk.' + sys.argv[2], *encoders], check=True)
PYCODE
flatpak run --user --command=/app/libexec/metadesk/decode-smoke "org.metadesk.$product" /app/share/metadesk/frame.h264
