#!/usr/bin/env bash
# metadesk — tools/win/build-uia-harness.sh
#
# First-light build of the Windows UI Automation accessibility backend.
# Runs inside an MSYS2 *clangarm64* (or clang64) shell. Builds the
# platform-neutral a11y core + a11y_uia.cpp into a static lib, then links:
#   - atspi_dump.exe  : walks the real Windows desktop via UIA (--json/--compact)
#   - test_uia_liveness.exe : session/provider loss and teardown races
#   - test_uia_walk_equiv.exe : cached walk == uncached reference walk,
#                       with before/after timings (md-cw8)
#   - test_atspi.exe  : the generic a11y serialisation/vocab tests
#                       (live UIA tests when MD_A11Y_LIVE_TESTS=1)
# This is deliberately NOT the full meson product build (ffmpeg/libb2/curl/
# nostrc are not yet available on Windows); it exists to compile and run the
# UIA backend, which had only ever been edited blind before this script.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build-win-uia}"
mkdir -p "$OUT"
CC=${CC:-clang}; CXX=${CXX:-clang++}
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -D_GNU_SOURCE -I$ROOT/src/core $(pkg-config --cflags libcjson)"
CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -I$ROOT/src/core $(pkg-config --cflags libcjson)"
LIBS="$(pkg-config --libs libcjson) -lole32 -loleaut32 -luiautomationcore -luuid -lpthread -luser32"

echo "== toolchain"; $CC --version | head -1; $CXX --version | head -1
echo "== compile core"
for f in a11y a11y_query a11y_snapshot log md_time; do
  $CC $CFLAGS -c "$ROOT/src/core/$f.c" -o "$OUT/$f.o"
done
echo "== compile UIA backend (first ever)"
$CXX $CXXFLAGS -c "$ROOT/src/core/a11y_uia.cpp" -o "$OUT/a11y_uia.o"
ar rcs "$OUT/libmd_a11y.a" "$OUT"/a11y.o "$OUT"/a11y_query.o "$OUT"/a11y_snapshot.o "$OUT"/log.o "$OUT"/md_time.o "$OUT"/a11y_uia.o
echo "== link atspi_dump.exe"
$CXX -o "$OUT/atspi_dump.exe" $CFLAGS -x c "$ROOT/tools/atspi_dump.c" -x none "$OUT/libmd_a11y.a" $LIBS
echo "== link test_atspi.exe"
$CXX -o "$OUT/test_atspi.exe" $CFLAGS -I"$ROOT/tests" -x c "$ROOT/tests/test_atspi.c" -x none "$OUT/libmd_a11y.a" $LIBS
echo "== link test_uia_liveness.exe (private test seams, no production backend object)"
$CXX $CXXFLAGS -o "$OUT/test_uia_liveness.exe" "$ROOT/tests/test_uia_liveness.cpp" \
  "$OUT/a11y.o" "$OUT/a11y_query.o" "$OUT/a11y_snapshot.o" "$OUT/log.o" $LIBS
echo "== link test_uia_walk_equiv.exe (uncached reference walker lives only in the test)"
$CXX $CXXFLAGS -o "$OUT/test_uia_walk_equiv.exe" "$ROOT/tests/test_uia_walk_equiv.cpp" \
  "$OUT/a11y.o" "$OUT/a11y_query.o" "$OUT/a11y_snapshot.o" "$OUT/log.o" $LIBS
echo "== done: $OUT"
ls -la "$OUT"/*.exe
