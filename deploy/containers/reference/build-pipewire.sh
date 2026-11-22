#!/bin/sh
# Throwaway Ubuntu 24.04 aarch64 source producer for Dockerfile.pipewire.
# Needs curl, gcc/g++, meson >=1.3, ninja, pkg-config and the distro development
# packages for dbus-1, libsystemd and libudev. No host installation or service use.
set -eu
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
# Upstream tag 1.4.10, immutable commit. Verify the downloaded source archive.
curl -fL https://codeload.github.com/PipeWire/pipewire/tar.gz/ced36a5315135044a22c4b373be44368d312f9c8 -o source.tar.gz
printf '%s  source.tar.gz\n' 3c3460f6fa93036f108e9576d1ba1c43de74f593a33b8ced0fad7747e316ac33 | sha256sum -c -
tar xf source.tar.gz
meson setup build pipewire-ced36a5315135044a22c4b373be44368d312f9c8 \
    --prefix=/usr --libdir=lib/aarch64-linux-gnu --buildtype=release \
    --auto-features=disabled -Dspa-plugins=enabled -Dpipewire-jack=disabled \
    -Dpipewire-v4l2=disabled -Dsession-managers=[] -Dexamples=disabled -Dtests=disabled
meson compile -C build -j2
DESTDIR="$out" meson install -C build
# Runtime binaries/configuration are public assets, not private test evidence.
# Preserve executability and make the install traversable by container UID1000.
chmod -R a+rX "$out"
# Retain source/build provenance beside the install, outside the image.
sha256sum source.tar.gz > source.sha256
