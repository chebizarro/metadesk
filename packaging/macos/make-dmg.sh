#!/bin/sh
# Wrap a signed Metadesk.app in a compressed .dmg with an /Applications link.
# Release builds then sign the .dmg, notarize it with `xcrun notarytool submit
# --wait`, and staple with `xcrun stapler staple` (docs/plans/packaging-*.md).
set -eu
[ "$#" -eq 2 ] || { echo "usage: $0 Metadesk.app OUTPUT.dmg" >&2; exit 2; }
[ ! -e "$2" ] || { echo "output must not exist: $2" >&2; exit 2; }
root=$(mktemp -d "${TMPDIR:-/tmp}/md-dmg.XXXXXX")
trap 'rm -rf "$root"' EXIT INT TERM
ditto "$1" "$root/Metadesk.app"
ln -s /Applications "$root/Applications"
hdiutil create -quiet -volname Metadesk -srcfolder "$root" -fs APFS -format UDZO "$2"
if [ "${MD_CODESIGN_IDENTITY:--}" != - ]; then
    codesign --force --sign "$MD_CODESIGN_IDENTITY" --timestamp "$2"
fi
echo "Created $2"
