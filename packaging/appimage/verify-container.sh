#!/bin/sh
# Clean different-distro smoke. Image must already exist; no package installs.
set -eu
[ "$#" = 3 ] || { echo 'usage: verify-container.sh ARTIFACT CONTAINER_IMAGE aarch64|x86_64' >&2; exit 2; }
artifact=$(realpath "$1") image=$2 arch=$3
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
encoders=$("$here/../ffmpeg/profile-encoders.sh" container "$arch" | tr ',' ' ')
podman run --pull=never --rm --name "${MD_SMOKE_NAME:-metadesk-appimage-smoke}" \
  --network=none --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --tmpfs /tmp:rw,size=256m -v "$artifact:/input/Metadesk.AppImage:ro" "$image" \
  sh -ec 'cd /tmp
    /input/Metadesk.AppImage --appimage-extract > /dev/null
    squashfs-root/AppRun --help
    export LD_LIBRARY_PATH=/tmp/squashfs-root/usr/lib
    squashfs-root/usr/libexec/metadesk/verify-codecs $1
    squashfs-root/usr/libexec/metadesk/decode-smoke squashfs-root/usr/share/metadesk/frame.h264
    cat /etc/os-release
    ldd --version | head -1' sh "$encoders"
