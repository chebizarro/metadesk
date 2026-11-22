#!/bin/sh
# Build a complete, pinned nostrc source export with system dependencies.
set -eu
[ "$#" = 2 ] || { echo 'usage: build-nostrc.sh NOSTRC_SOURCE NEW_BUILD' >&2; exit 2; }
src=$(realpath "$1") out=$2
[ ! -e "$out" ] || exit 2
cmake -S "$src" -B "$out" -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_APPS=OFF -DBUILD_GNOSTR_APP=OFF -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_TESTING=OFF -DBUILD_TESTING_FRAMEWORK=OFF -DBUILD_SIGNER_TESTS=OFF \
  -DBUILD_FIPS=OFF -DBUILD_RELAYD=OFF -DBUILD_LIBHANAMI=OFF -DBUILD_LIBBLOSSOM=OFF \
  -DBUILD_LIBMARMOT=OFF -DBUILD_LIBPORTHOME=OFF -DBUILD_LIBVAULT=OFF \
  -DBUILD_LIBVAULT_MARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DSIGNET_ENABLE=OFF
cmake --build "$out" -j2 --target nostr nostr_json go_fiber nostr_nip44_core \
  nostr_nip46_core nostr_nip5f_core nip04 nip19 nostr_nip51_core nostr_nip98 nip11 nostr_nip17_core
