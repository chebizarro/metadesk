#!/bin/sh
set -eu

# Build-tree bundle only. Meson supplies the built executable and generated plist.
bundle=$1
mkdir -p "$bundle/Contents/MacOS"
cp "$2" "$bundle/Contents/MacOS/metadesk-macos-manager"
cp "$3" "$bundle/Contents/Info.plist"
touch "$bundle"
