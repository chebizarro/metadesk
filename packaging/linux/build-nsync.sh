#!/bin/sh
# Fedora does not currently provide nsync-devel: build the nostrc-pinned submodule.
set -eu
[ "$#" = 3 ] || { echo 'usage: build-nsync.sh NOSTRC_SOURCE NEW_BUILD NEW_PREFIX' >&2; exit 2; }
[ ! -e "$2" ] && [ ! -e "$3" ] || exit 2
cmake -S "$1/third_party/nsync" -B "$2" -DBUILD_SHARED_LIBS=OFF \
  -DNSYNC_ENABLE_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$3" -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$2" -j2
cmake --install "$2"
