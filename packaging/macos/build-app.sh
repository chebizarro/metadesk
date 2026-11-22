#!/bin/sh
# md-kb7 macOS prototype: build metadesk, stage an install tree, bundle it into
# a self-contained Metadesk.app, verify it, and (optionally) wrap it in a .dmg.
#
#   packaging/macos/build-app.sh NOSTRC_ROOT NOSTRC_BUILD_DIR OUTPUT [MESON_OPTION...]
#
# OUTPUT must not exist and must be outside the source tree. NOSTRC_BUILD_DIR is
# a nostrc CMake build with the libraries metadesk links (docs/build-signers.md).
# Signing: MD_CODESIGN_IDENTITY (default '-' = ad-hoc). Setting a Developer ID
# identity enables the hardened runtime. MD_DMG=1 also produces a .dmg.
set -eu
[ "$#" -ge 3 ] || { echo "usage: $0 NOSTRC_ROOT NOSTRC_BUILD_DIR OUTPUT [MESON_OPTION...]" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
nostrc=$(cd "$1" && pwd)
nostrc_build=$(cd "$2" && pwd)
[ ! -e "$3" ] || { echo "output must not exist: $3" >&2; exit 2; }
mkdir -p "$3"
out=$(cd "$3" && pwd)
case "$out/" in "$src"/*) echo "output must be outside the source tree" >&2; rmdir "$out"; exit 2 ;; esac
shift 3

identity=${MD_CODESIGN_IDENTITY:--}
version=$(meson introspect --projectinfo "$src/meson.build" | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
rev=$(git -C "$src" rev-parse --short=12 HEAD)
git -C "$src" diff --quiet HEAD -- . ':!.beads' || rev="$rev-dirty"
nostrc_rev=$(git -C "$nostrc" rev-parse --short=12 HEAD)

meson setup "$out/build" "$src" --buildtype=release --prefix=/opt/metadesk \
    -Dclient=true -Dsigner_nip5f=true -Dsigner_nip46=true \
    -Dnostrc_root="$nostrc" -Dnostrc_build_dir="$nostrc_build" "$@"
meson compile -C "$out/build"
meson install -C "$out/build" --destdir "$out/stage" --quiet

# Homebrew's sdl2 is sdl2-compat, which dlopens SDL3 at SDL_Init time. The
# link closure cannot see it, so it is declared explicitly (contract: enumerate
# dlopen features rather than trusting the linker view).
dlopen_args=
sdl2_lib=$(pkg-config --variable=libdir sdl2)/libSDL2-2.0.0.dylib
if [ -f "$sdl2_lib" ] && strings "$sdl2_lib" | grep -q '^@loader_path/libSDL3.dylib$'; then
    sdl3_lib=$(pkg-config --variable=libdir sdl3)/libSDL3.dylib
    [ -f "$sdl3_lib" ] || { echo "sdl2-compat needs SDL3, not found" >&2; exit 1; }
    dlopen_args="--dlopen libSDL3.dylib=$sdl3_lib"
fi

# Build-tree rpaths are the producer's link view; installed images lost them.
search_args=
for rp in $(otool -l "$out/build/metadesk-client" "$out/build/metadesk-host" "$out/build/libmetadesk.0.dylib" |
            awk '/cmd LC_RPATH/{getline; getline; print $2}' | sort -u); do
    case "$rp" in @*) ;; *) search_args="$search_args --search $rp" ;; esac
done

hardened=
[ "$identity" = - ] || hardened=--hardened-runtime
# shellcheck disable=SC2086 # intentional word splitting of optional args
python3 "$here/bundle_app.py" --stage "$out/stage/opt/metadesk" --out "$out/Metadesk.app" \
    --version "$version" --source-rev "$rev" --nostrc-rev "$nostrc_rev" \
    --sign-identity "$identity" $hardened $dlopen_args $search_args
"$here/verify-app.sh" "$out/Metadesk.app"

if [ "${MD_DMG:-0}" = 1 ]; then
    "$here/make-dmg.sh" "$out/Metadesk.app" "$out/Metadesk-$version.dmg"
fi
(cd "$out" && find Metadesk.app -type f -print0 | sort -z | xargs -0 shasum -a 256 >SHA256SUMS
 [ ! -f "Metadesk-$version.dmg" ] || shasum -a 256 "Metadesk-$version.dmg" >>SHA256SUMS)
echo "Built $out/Metadesk.app ($rev, nostrc $nostrc_rev, signed with '$identity')"
