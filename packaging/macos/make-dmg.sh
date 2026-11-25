#!/bin/sh
# Wrap the signed apps in a compressed .dmg with an /Applications link.
# Release builds then sign the .dmg, notarize it with `xcrun notarytool submit
# --wait`, and staple with `xcrun stapler staple` (docs/plans/packaging-*.md).
set -eu
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || { echo "usage: $0 Metadesk.app OUTPUT.dmg [MANAGER.app]" >&2; exit 2; }
[ ! -e "$2" ] || { echo "output must not exist: $2" >&2; exit 2; }
root=$(mktemp -d "${TMPDIR:-/tmp}/md-dmg.XXXXXX")
trap 'rm -rf "$root"' EXIT INT TERM
ditto "$1" "$root/Metadesk.app"
if [ -n "${3:-}" ]; then
    ditto "$3" "$root/$(basename "$3")"
fi
ln -s /Applications "$root/Applications"
hdiutil create -quiet -volname Metadesk -srcfolder "$root" -fs APFS -format UDZO "$2"
if [ "${MD_CODESIGN_IDENTITY:--}" != - ]; then
    codesign --force --sign "$MD_CODESIGN_IDENTITY" --timestamp "$2"
fi
echo "Created $2"
