#!/bin/sh
# Explicit, pre-fetched tools only; no publishing or real-key signing.
set -eu
[ "$#" = 4 ] || { echo 'usage: make-image.sh APPDIR OUTPUT.AppImage APPIMAGETOOL TYPE2_RUNTIME' >&2; exit 2; }
appdir=$(realpath "$1") output=$2 tool=$(realpath "$3") runtime=$(realpath "$4")
[ ! -e "$output" ] || exit 2
# appimagetool may itself need FUSE; extract-and-run also works on container builders.
APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --runtime-file "$runtime" "$appdir" "$output"
sha256sum "$output" > "$output.sha256"
