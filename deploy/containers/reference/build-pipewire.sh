#!/bin/sh
# Throwaway Ubuntu 24.04 aarch64 source producer for Dockerfile.pipewire.
# Needs Git, Python >=3.12, gcc/g++, meson >=1.3, ninja, pkg-config and the distro development
# packages for dbus-1, libsystemd and libudev. No host installation or service use.
set -eu
here=$(cd "$(dirname "$0")" && pwd -P)
[ "$#" = 2 ] || { echo "usage: $0 NEW_WORK_DIR NEW_RUNTIME_DIR" >&2; exit 2; }
[ "$(uname -m)" = aarch64 ] || { echo 'requires aarch64' >&2; exit 2; }
. /etc/os-release
[ "$ID:$VERSION_ID" = ubuntu:24.04 ] || { echo 'requires Ubuntu 24.04' >&2; exit 2; }
[ ! -e "$1" ] && [ ! -e "$2" ] || { echo 'directories must be new' >&2; exit 2; }
umask 077
mkdir -p "$1" "$2"
work=$(realpath "$1")
out=$(realpath "$2")
cd "$work"
# Upstream 1.4.10. Verify the exact commit and tree before exporting sources.
commit=ced36a5315135044a22c4b373be44368d312f9c8
tree=bf76e6d580ede8a1e9f268182874f1f22974cb03
python3 "$here/../../../packaging/git_source.py" source \
    https://github.com/PipeWire/pipewire.git "$commit" "$tree"
meson setup build source \
    --prefix=/usr --libdir=lib/aarch64-linux-gnu --buildtype=release \
    --auto-features=disabled -Dspa-plugins=enabled -Dpipewire-jack=disabled \
    -Dpipewire-v4l2=disabled -Dsession-managers=[] -Dexamples=disabled -Dtests=disabled
meson compile -C build -j2
DESTDIR="$out" meson install -C build
# Runtime binaries/configuration are public assets, not private test evidence.
# Preserve executability and make the install traversable by container UID1000.
chmod -R a+rX "$out"
# Retain source/build provenance beside the install, outside the image.
printf 'commit=%s\ntree=%s\n' "$commit" "$tree" > source.git
