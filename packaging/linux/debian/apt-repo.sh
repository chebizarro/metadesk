#!/bin/sh
# Produce an UNSIGNED local apt layout. Owner signs Release -> InRelease later.
set -eu
[ "$#" = 4 ] || { echo 'usage: apt-repo.sh DEB SUITE ARCH NEW_REPO' >&2; exit 2; }
deb=$(realpath "$1") suite=$2 arch=$3 out=$4
case "$suite:$arch" in *[!a-zA-Z0-9_.:-]*) exit 2;; esac
[ ! -e "$out" ] || exit 2
mkdir -p "$out/pool/main/m/metadesk" "$out/dists/$suite/main/binary-$arch"
cp "$deb" "$out/pool/main/m/metadesk/"
cd "$out"
apt-ftparchive packages pool > "dists/$suite/main/binary-$arch/Packages"
gzip -n -c "dists/$suite/main/binary-$arch/Packages" > "dists/$suite/main/binary-$arch/Packages.gz"
apt-ftparchive -o APT::FTPArchive::Release::Origin=Metadesk \
  -o APT::FTPArchive::Release::Suite="$suite" -o APT::FTPArchive::Release::Codename="$suite" \
  -o APT::FTPArchive::Release::Architectures="$arch" -o APT::FTPArchive::Release::Components=main \
  release "dists/$suite" > "dists/$suite/Release"
echo 'UNSIGNED: owner must sign Release and distribute the apt key via a trusted channel.'
