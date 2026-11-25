#!/bin/sh
# Stage is produce.py native OUTPUT/tree. Requires dpkg-dev, binutils, gzip.
set -eu
[ "$#" = 3 ] || { echo 'usage: build-deb.sh TREE VERSION NEW_OUTPUT' >&2; exit 2; }
tree=$(realpath "$1") version=$2 out=$3
[ ! -e "$out" ] || { echo 'output must be new' >&2; exit 2; }
case "$version" in *[!0-9A-Za-z.+~-]*|'') exit 2;; esac
mkdir -p "$out"
out=$(realpath "$out")
root="$out/root"
cp -a "$tree" "$root"
mkdir -p "$root/DEBIAN" "$out/debian"
printf 'Source: metadesk\n' > "$out/debian/control"
# Use dpkg's distro shlibs/symbols database; never hard-code SONAME packages.
cd "$out"
set -- -e"$root/usr/bin/metadesk-host" -e"$root/usr/bin/metadesk-client" \
  -e"$root/usr/lib/metadesk/libmetadesk.so.0.1.0"
if [ -x "$root/usr/bin/metadesk-manager" ]; then set -- "$@" -e"$root/usr/bin/metadesk-manager"; fi
deps=$(dpkg-shlibdeps -O -l"$root/usr/lib/metadesk" "$@" | sed -n 's/^shlibs:Depends=//p')
[ -n "$deps" ] || { echo 'empty shlibs dependencies' >&2; exit 1; }
arch=$(dpkg --print-architecture)
cat > "$root/DEBIAN/control" <<EOF
Package: metadesk
Version: $version
Architecture: $arch
Maintainer: Chris Daley <chebizarro@gmail.com>
Section: net
Priority: optional
Homepage: https://github.com/chebizarro/metadesk
Depends: $deps
Recommends: xdg-desktop-portal
Suggests: fipsd
Installed-Size: $(du -sk "$root/usr" | cut -f1)
Description: Nostr-native desktop host and video client
 Desktop accessibility, authenticated remote control and low-latency video.
 Uses system FFmpeg; Cisco OpenH264 is optional and never bundled.
EOF
# Copyright includes the complete project license and dependency boundary.
cp "$root/usr/share/doc/metadesk/LICENSE" "$root/usr/share/doc/metadesk/copyright"
rm "$root/usr/share/doc/metadesk/LICENSE"
printf 'metadesk (%s) unstable; urgency=medium\n\n  * Local native packaging build.\n\n -- Chris Daley <chebizarro@gmail.com>  Sun, 22 Nov 2026 00:00:00 +0000\n' "$version" | gzip -n -9 > "$root/usr/share/doc/metadesk/changelog.Debian.gz"
for page in "$root"/usr/share/man/man1/*.1; do gzip -n -9 "$page"; done
# Regenerate hashes after package documentation normalization.
rm -f "$root/SHA256SUMS"
(cd "$root" && find usr etc -type f -print0 | sort -z | xargs -0 md5sum) > "$root/DEBIAN/md5sums"
chmod -R go-w "$root"
dpkg-deb --root-owner-group --build "$root" "$out/metadesk_${version}_${arch}.deb"
