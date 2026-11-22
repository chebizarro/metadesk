#!/usr/bin/env bash
# metadesk — tools/win/build-media-harness.sh
#
# Standalone build of the Windows media backends (md-7yc). Runs inside an
# MSYS2 *clangarm64* (or clang64) shell, like build-uia-harness.sh, because
# the full meson product build still lacks Windows dependencies. Compiles:
#   - capture_dxgi.cpp and input_sendinput.cpp as production objects
#     (-Wall -Wextra -Werror)
#   - test_capture_dxgi_win.exe : DXGI backend vs the capture HAL contract
#                                 (session 0: init fails cleanly; --interactive:
#                                 frames, IDLE, timeouts, stop, loss recovery)
#   - test_sendinput_win.exe    : SendInput backend with SendInput replaced by
#                                 a recorder in the test TU (never injects input)
#   - test_win_dpi_win.exe      : win_dpi.h compiled as C, as src/host/main.c
#                                 includes it: process per-monitor v2 (md-m5x4)
#   - test_win_dpi_manifest.exe : same C test with the product manifest;
#                                 run in the console, including while locked
#   - test_win_dpi_live.exe [DPI] : opt-in UNLOCKED owned-console test;
#                                 UIA/DXGI physical rectangles + one real click
#                                 in its own window. E.g. 144 requires 150%.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build-win-media}"
mkdir -p "$OUT"
CC=${CC:-clang}; CXX=${CXX:-clang++}
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I$ROOT/src/core"
CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Werror -I$ROOT/src/core"

echo "== toolchain"; $CXX --version | head -1
echo "== compile backends"
$CXX $CXXFLAGS -c "$ROOT/src/core/capture_dxgi.cpp" -o "$OUT/capture_dxgi.o"
$CXX $CXXFLAGS -c "$ROOT/src/core/input_sendinput.cpp" -o "$OUT/input_sendinput.o"
$CC $CFLAGS -c "$ROOT/src/core/log.c" -o "$OUT/log.o"
$CC $CFLAGS -c "$ROOT/src/core/input.c" -o "$OUT/input.o"
$CC $CFLAGS -c "$ROOT/src/core/capture.c" -o "$OUT/capture.o"
$CC $CFLAGS -c "$ROOT/src/core/md_time.c" -o "$OUT/md_time.o"
echo "== link test_capture_dxgi_win.exe"
$CXX $CXXFLAGS -o "$OUT/test_capture_dxgi_win.exe" "$ROOT/tests/test_capture_dxgi_win.cpp" \
  "$OUT/capture.o" "$OUT/md_time.o" "$OUT/log.o" -ld3d11 -ldxgi -lgdi32 -luser32 -lpthread
echo "== link test_sendinput_win.exe"
$CXX $CXXFLAGS -o "$OUT/test_sendinput_win.exe" "$ROOT/tests/test_sendinput_win.cpp" \
  "$OUT/input.o" "$OUT/log.o" -luser32
echo "== link test_win_dpi_win.exe"
$CC $CFLAGS -o "$OUT/test_win_dpi_win.exe" "$ROOT/tests/test_win_dpi_win.c" -luser32
echo "== link test_win_dpi_live.exe (owned window, real input; opt-in console run)"
windres -I"$ROOT/tools/win" "$ROOT/tools/win/metadesk.rc" "$OUT/metadesk-res.o"
$CC $CFLAGS -DMD_TEST_MANIFEST -o "$OUT/test_win_dpi_manifest.exe" \
  "$ROOT/tests/test_win_dpi_win.c" "$OUT/metadesk-res.o" -luser32
$CXX $CXXFLAGS -o "$OUT/test_win_dpi_live.exe" "$ROOT/tests/test_win_dpi_live.cpp" \
  "$OUT/capture.o" "$OUT/capture_dxgi.o" "$OUT/input.o" "$OUT/input_sendinput.o" \
  "$OUT/md_time.o" "$OUT/log.o" "$OUT/metadesk-res.o" \
  -ld3d11 -ldxgi -lgdi32 -luser32 -lole32 -loleaut32 -luiautomationcore -luuid -lpthread

echo "== done: $OUT"
ls -la "$OUT"/*.exe
