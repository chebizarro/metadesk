#!/bin/bash
# Native MSYS2 CLANGARM64. Inputs are pinned, pristine source directories from
# fetch-sources.py; output must be NEW and outside all source trees.
set -euo pipefail
[ "$#" = 2 ] || { echo "usage: $0 INPUTS NEW_OUTPUT" >&2; exit 2; }
repo=$(cd "$(dirname "$0")/../.." && pwd -P)
inputs=$(cd "$1" && pwd -P)
[ ! -e "$2" ] || { echo 'output exists' >&2; exit 2; }
out=$(realpath -m "$2")
case "$out/" in "$repo/"*|"$inputs/"*) echo 'output must be outside sources' >&2; exit 2;; esac
[ "${MSYSTEM:-}" = CLANGARM64 ] || { echo 'Use CLANGARM64 shell' >&2; exit 2; }
mkdir -p "$out"
# Copy inputs so CMake never writes generated headers into the pinned trees.
cp -R "$inputs/nostrc" "$out/nostrc"
cp -R "$inputs/secp256k1" "$out/secp256k1"
cmake -S "$out/secp256k1" -B "$out/secp-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$out/secp-install")" \
  -DBUILD_SHARED_LIBS=OFF -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF \
  -DSECP256K1_BUILD_BENCHMARK=OFF -DSECP256K1_BUILD_EXAMPLES=OFF
cmake --build "$out/secp-build" -j2
cmake --install "$out/secp-build"
mkdir -p "$out/headers"
cp -R "$inputs/amf/amf/public/include" "$out/headers/AMF"
cp -R "$inputs/nv-codec-headers" "$out/nv-codec-headers"
make -C "$out/nv-codec-headers" PREFIX="$out/headers" install
CC=cc AR=llvm-ar MD_FFMPEG_JOBS=2 MD_FFMPEG_ARCH=aarch64 MD_FFMPEG_TARGET_OS=mingw32 \
  PKG_CONFIG_PATH="$out/headers/lib/pkgconfig" MD_FFMPEG_CFLAGS="-I$out/headers" \
  sh "$repo/packaging/ffmpeg/build-minimal-lgpl.sh" windows "$inputs/ffmpeg" "$out/ffmpeg"
export PKG_CONFIG_PATH="$out/secp-install/lib/pkgconfig:$out/ffmpeg/install/lib/pkgconfig"
cmake -S "$out/nostrc" -B "$out/nostrc/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_FLAGS=-DSECP256K1_STATIC -DCMAKE_CXX_FLAGS=-DSECP256K1_STATIC \
  -DCMAKE_PREFIX_PATH="$(cygpath -m "$out/secp-install")" \
  -DBUILD_TESTING=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
  -DBUILD_SIGNER_TESTS=OFF -DLIBNOSTR_BUILD_TESTS=OFF \
  -DBUILD_APPS=OFF -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_LIBBLOSSOM=OFF -DBUILD_LIBPORTHOME=OFF \
  -DBUILD_LIBVAULT=OFF -DBUILD_LIBVAULT_MARMOT=OFF \
  -DBUILD_NATIVE_HOST=OFF -DBUILD_WINDOWS_INSTALLER=OFF \
  -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DBUILD_LIBHANAMI=OFF \
  -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF -DBUILD_RELAYD=OFF \
  -DBUILD_FIPS=OFF -DSIGNET_ENABLE=OFF \
  -DENABLE_NIP55L=OFF -DENABLE_NIP5F=OFF -DENABLE_NIP57=OFF -DENABLE_NIP78=ON \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
cmake --build "$out/nostrc/build" --target nostr nostr_json go_fiber \
  nostr_nip44_core nip19 nostr_nip51_core nostr_nip78_core nostr_nip98 nip11 nostr_nip17_core nip05 nip65 -j2
meson setup "$out/product" "$repo" --buildtype=release \
  -Dnostrc_discovery=build-tree -Dnostrc_root="$(cygpath -m "$out/nostrc")" \
  -Dnostrc_build_dir="$(cygpath -m "$out/nostrc/build")" -Dtools=true \
  "-Dc_args=['-DSECP256K1_STATIC','-I$(cygpath -m "$out/secp-install/include")']" \
  "-Dcpp_args=['-DSECP256K1_STATIC','-I$(cygpath -m "$out/secp-install/include")']" \
  "-Dc_link_args=['-L$(cygpath -m "$out/secp-install/lib")']" \
  "-Dcpp_link_args=['-L$(cygpath -m "$out/secp-install/lib")']"
meson compile -C "$out/product" -j2
python "$repo/packaging/windows/stage.py" --repo "$(cygpath -m "$repo")" \
  --build "$(cygpath -m "$out/product")" --nostrc "$(cygpath -m "$out/nostrc")" \
  --secp "$(cygpath -m "$out/secp256k1")" --ffmpeg "$(cygpath -m "$out/ffmpeg")" \
  --inputs "$(cygpath -m "$inputs")" --prefix "$(cygpath -m /clangarm64)" --output "$(cygpath -m "$out/stage")"
# shellcheck disable=SC2046 # pkg-config emits individual compiler/linker arguments.
cc "$repo/packaging/ffmpeg/verify-codecs.c" $(pkg-config --cflags --libs libavcodec libavutil) -o "$out/stage/verify-codecs.exe"
IFS=, read -r -a encoders <<< "$(sh "$repo/packaging/ffmpeg/profile-encoders.sh" windows aarch64)"
"$out/stage/verify-codecs.exe" "${encoders[@]}" > "$out/codec-inventory.txt"
rm "$out/stage/verify-codecs.exe"
pacman -Q > "$out/msys2-packages.txt"
clang --version > "$out/toolchain.txt"
cmake --version >> "$out/toolchain.txt"
meson --version >> "$out/toolchain.txt"
cp "$inputs/sources.lock.json" "$out/stage/source-inputs.json"
echo "PASS: ARM64 product, private runtime closure and exact LGPL FFmpeg inventory: $out/stage"
