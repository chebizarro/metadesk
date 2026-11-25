#!/bin/sh
# Build the native arm64 13.0-floor dependency prefix and verified app.
set -eu
[ "$#" = 2 ] || { echo "usage: $0 NOSTRC_GIT_REPO NEW_ABSOLUTE_OUTPUT" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd -P)
case "$2" in /*) ;; *) echo 'OUTPUT must be absolute' >&2; exit 2 ;; esac
[ ! -e "$2" ] || { echo 'OUTPUT must not exist' >&2; exit 2; }
[ "$(uname -s):$(uname -m)" = Darwin:arm64 ] || { echo 'native macOS arm64 required' >&2; exit 2; }
# The dependency builder enforces platform/source guards before creating output.
nostrc=$1 output=$2
deps=${MD_RELEASE_DEPS_DIR:-$output/deps}
case "$deps" in /*) ;; *) echo 'MD_RELEASE_DEPS_DIR must be absolute' >&2; exit 2 ;; esac
if [ ! -d "$deps" ]; then
    set -- "$deps" --nostrc-repo "$nostrc"
    if [ -n "${MD_DEPS_DOWNLOADS:-}" ]; then
        set -- "$@" --downloads "$MD_DEPS_DOWNLOADS"
    fi
    python3 "$here/build-deps.py" "$@" || {
        tail -n 40 "$deps"/logs/*.log >&2
        exit 1
    }
else
    # Opt-in CI cache, with toolchain and producer hashes in the cache key.
    python3 - "$here/deps.lock.json" "$deps/deps.lock.json" <<'PY'
import json, sys
from pathlib import Path
if json.loads(Path(sys.argv[1]).read_text()) != json.loads(Path(sys.argv[2]).read_text()):
    sys.exit('cached native dependencies do not match the source lock')
PY
fi
mkdir -p "$output"
out=$(cd "$output" && pwd -P)
export MD_DEPS_PREFIX="$deps/prefix" MD_MACOS_MIN=13.0
"$here/build-app.sh" "$deps/sources/nostrc" "$deps/build/nostrc" "$out/app" -Dmacos_manager=true
NOSTRC_TEST_ISOLATION=1 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    meson test -C "$out/app/build" --print-errorlogs
