#!/bin/sh
# Build the native arm64 13.0-floor dependency prefix and verified app.
set -eu
[ "$#" = 2 ] || { echo "usage: $0 NOSTRC_GIT_REPO NEW_ABSOLUTE_OUTPUT" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd -P)
case "$2" in /*) ;; *) echo 'OUTPUT must be absolute' >&2; exit 2 ;; esac
[ ! -e "$2" ] || { echo 'OUTPUT must not exist' >&2; exit 2; }
# The dependency builder enforces platform/source guards before creating output.
python3 "$here/build-deps.py" "$2/deps" --nostrc-repo "$1"
out=$(cd "$2" && pwd -P)
export MD_DEPS_PREFIX="$out/deps/prefix" MD_MACOS_MIN=13.0
"$here/build-app.sh" "$out/deps/sources/nostrc" "$out/deps/build/nostrc" "$out/app"
NOSTRC_TEST_ISOLATION=1 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    meson test -C "$out/app/build" --print-errorlogs
