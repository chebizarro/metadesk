# UIA harness

The full Windows product build needs additional dependencies. This harness
builds the real accessibility core and UIA backend independently.

In an MSYS2 CLANGARM64 shell with libcjson installed:

```sh
bash tools/win/build-uia-harness.sh
./build-win-uia/test_atspi.exe
./build-win-uia/test_uia_liveness.exe
./build-win-uia/test_uia_walk_equiv.exe
```

The last command is the **session-0** cell: run it through Windows OpenSSH.
It requires an allocated backend with `is_connected == false` and tree access
rejected. A COM pointer or an empty desktop root is not proof of readiness.
The generic tests skip live cells unless `MD_A11Y_LIVE_TESTS=1` is set.

## Interactive desktop

The user must be logged in at the VM console. In that console (not SSH), with
`C:\msys64\clangarm64\bin` on PATH:

```powershell
.\build-win-uia\test_uia_liveness.exe --interactive
.\build-win-uia\test_uia_walk_equiv.exe --interactive
$env:MD_A11Y_LIVE_TESTS = '1'
.\build-win-uia\test_atspi.exe
.\build-win-uia\atspi_dump.exe --compact
```

From SSH, scp a PowerShell script rather than nesting shell quotations.
`run-interactive.ps1` does the scheduling. It registers a uniquely named
one-shot task using
`New-ScheduledTaskPrincipal -UserId (whoami) -LogonType Interactive`, waits
with a deadline for your `.cmd` to create its done file, and stops and
unregisters the task in a `finally` block. It exits 3 when no console session
is active, so record that interactive cell as pending.

```powershell
powershell -ExecutionPolicy Bypass -File run-interactive.ps1 `
    -Command C:\path\job.cmd -DoneFile C:\path\job.done -TimeoutSeconds 300
```

The `.cmd` runs the commands above, captures each exit code to its outputs,
and creates the done file last. Delete temporary scripts and outputs when
finished. SSH itself runs in session 0 even when `quser` reports an active
console.

## Walk equivalence and timing (md-cw8, md-0by)

The backend walks each top-level window with one `TreeScope_Subtree`
cache-request round trip. UIA cannot bound that fetch, so items past the
256-child cap are fetched and then dropped. For the in-process Win32 proxies
(list box, list view, tree view, combo box) each dropped item costs window
messages. So before each walk the backend checks the visible Win32
descendants of every top-level window for those classes and asks for their
item counts. This takes about 0.3 ms on the desktop and makes no UIA call.
A window holding a container with more than 1024 items is walked by bounded
`BuildCache` navigation instead, with the uncached walk's exact semantics.
While such a window is on screen, the other windows cost one extra round trip
each.

`test_uia_walk_equiv.exe` keeps a verbatim copy of the old uncached
TreeWalker walk and requires identical output: the same node structure plus
byte-identical JSON and compact serialisation at a fixed timestamp. It runs
these cells:

- A Win32 fixture window: buttons (one disabled), a checkbox, an edit, a
  label, a 300-item list (the 256-child cap) and nested group boxes deeper than
  the 32-level depth cap. The subtree cache, the per-level fallback and the
  bounded navigation walk are all compared.
- The production top-level enumeration over fixture elements. This covers the
  subtree tier and the two-step tier, which uses a test-only lowered
  wide-list threshold (`uia_test_wide_items`) so the 300-item list takes the
  bounded path. It also covers a forced fallback to element navigation plus a
  per-level walk, a forced leaf fallback (enumeration must continue) and the
  top-level cap. The failures are forced through the test-only
  `uia_test_rejected` seam.
- The fixture timed as production walks a top-level window, next to the
  md-cw8 subtree-only fetch.
- With `--interactive`, the real desktop: the production `uia_get_tree` versus
  the uncached reference, retried up to 5 times if the desktop changes between
  walks. It prints uncached and cached medians and the wide-list scan cost.
  When the fixture is wide, the same cell also runs with the fixture open.

In session 0 both cells SKIP. Windows on that non-interactive window station
are never visible, so UIA exposes no fixture children, and the backend rejects
desktop walks there by design.

To measure wide lists, compile with `-DFIXTURE_LIST_ITEMS=5000`. Or add a
virtual list view with `-DFIXTURE_VLIST_ITEMS=100000 -DFIXTURE_TIMING_RUNS=1`
and link with `-lcomctl32`.

Reference numbers on the Win11 ARM64 VM (console session, medians):

| Cell | Uncached | Subtree only (md-cw8) | Production |
|---|---|---|---|
| Fixture, 300-item list | 645-880 ms | 200-240 ms | 210-240 ms (subtree) |
| Fixture, 5000-item list | 760 ms | 1370 ms | 400 ms (bounded) |
| Fixture, 100k virtual list view | 1310 ms | 20.6 s | 650 ms (bounded) |
| Desktop, 281 nodes | 570-790 ms | 2.8-3.0x | 2.7-2.9x |
| Desktop plus the 5000-item fixture | 1420 ms | | 660 ms (2.1x) |

## DXGI capture and SendInput (md-7yc, md-346)

```sh
bash tools/win/build-media-harness.sh
./build-win-media/test_sendinput_win.exe
./build-win-media/test_win_dpi_win.exe
./build-win-media/test_capture_dxgi_win.exe                 # session 0 cell
./build-win-media/test_capture_dxgi_win.exe --interactive   # console only
```

The script compiles `capture_dxgi.cpp` and `input_sendinput.cpp` with
`-Wall -Wextra -Werror`.

`test_sendinput_win.exe` replaces `SendInput` with a recorder in its own
translation unit. It checks the INPUT records and the propagation of
rejected injections. It never injects input, so it is safe in any session.
Never aim real SendInput at the console: it moves the user's pointer and types
into the user's applications.

The test also replaces `GetSystemMetrics` with a synthetic virtual desktop
(md-w65), so mouse moves are checked against layouts the VM does not have:
a non-primary output, negative origins, mixed DPI, a portrait output, and a
layout that changes while the backend runs. Each recorded move is decoded
the way Windows decodes `MOUSEEVENTF_VIRTUALDESK` (pixel = left +
n * width / 65536) and must land on the exact intended pixel. The mapping
works in two steps:

1. `md_input_map_to_output` (input.c) maps the frame point into the captured
   output's rectangle (`MdInputConfig.output_*`, copied from
   `md_capture_get_output_rect`).
2. The backend aims at the centre of that desktop pixel, across the virtual
   desktop.

The DXGI backend publishes that rectangle in physical pixels:
`GetMonitorInfo` on the output's monitor, read per-monitor DPI aware
(`win_dpi.h`). The virtual desktop is read the same way, because
`DXGI_OUTPUT_DESC.DesktopCoordinates` and `SM_*VIRTUALSCREEN` are
DPI-virtualized for a process that is not DPI aware.

### Per-monitor DPI awareness (md-m5x4)

Agent and MCP clicks use UI Automation `BoundingRectangle` coordinates,
which are physical only for a DPI-aware client. The host therefore calls
`md_win_dpi_process_init()` (`win_dpi.h`) as the first statement of
`main()`, before any window, UIA, DXGI or input call. That makes the process
per-monitor v2 aware (`SetProcessDpiAwarenessContext`, Windows 10 1703+;
per-monitor v1 through shcore on 8.1 to 1607), and every thread inherits
it. If a manifest or Windows already set the awareness, the call is
refused and the awareness in effect is reported. Over SSH, Windows starts
processes per-monitor v1 aware. DXGI `init` logs when the process is not
per-monitor aware.

The API call is used instead of an application manifest because the meson
Windows build is still blocked. A manifest needs `windows.compile_resources`,
and MinGW and MSVC embed it differently (MSVC also embeds its own default
manifest). None of that can be built or verified yet, while the call is
exercised by the standalone harnesses today. If a manifest is added later,
the call becomes a no-op.

With the process per-monitor aware, every coordinate source is physical
virtual-desktop pixels:

- UIA node rectangles. Walks also run inside an `MdPhysicalPixels` scope.
- The DXGI output rectangle and the "all" frame bounds. Regions are
  mode-sized, so physical rectangles make them abut exactly.
- `SendInput`'s virtual desktop.
- `md_input_execute_action`'s global-to-frame conversion, which inverts
  `md_input_map_to_output`.

The backend scopes also keep tools and tests that never called the init
correct.

The VM has one 100% output, so mixed DPI is covered synthetically:

- `test_sendinput_win` fakes `GetSystemMetrics` so it DPI-virtualizes
  (logical units at 150%) for threads that are not per-monitor aware, and
  runs every move on a forced-unaware thread. A mixed 100/125/150% layout
  checks that agent clicks land on the exact physical pixel, both 1:1 and
  in "all". A scaled stream lands within one frame pixel's footprint. MCP
  global points are exact. The cell runs on an unaware thread, then in a
  per-monitor process.
- `test_capture_dxgi_win` fakes `GetMonitorInfoW` for synthetic monitors
  the same way. It checks physical output rectangles from an unaware
  thread, scope restoration and the composed "all" layout.
- `test_win_dpi_win.c` compiles `win_dpi.h` as C with `src/host/main.c`'s
  include block. In the console it checks unaware to per-monitor v2, a
  repeated call and thread inheritance. In session 0 it checks that the
  preset v1 awareness is reported.

A real scaled or mixed-DPI display is not covered.

`test_capture_dxgi_win.exe` checks the capture HAL contract. Both modes start
with synthetic cells that need no desktop:

- monochrome, color and masked-color pointer blending
- clipping at every frame edge
- rejection of malformed shapes
- save-under restore after a move or hide
- which pointer updates count as damage
- output selector parsing

In session 0 no output is attached to the desktop, so `init` fails cleanly,
both by default and with an explicit selection. With `--interactive` it also
covers:

- output selection: the default is the primary, every output can be picked
  by index, by device name and by bare name, and unknown selections fail
  `init` and log the attached outputs
- ERROR before start and after stop, returned promptly
- IDLE versus frames within the timeout
- frame fields and QPC-nanosecond timestamps
- an unreleased frame reclaimed by the next call
- access loss, then recovery through stop + start
- real pointer shapes (monochrome, color, masked color) checked in the
  delivered pixels, hotspot placement, hiding, pointer-only updates as frames
  with `show_cursor`, and IDLE without it

Desktop Duplication delivers no frames while the console display is powered
off, which is the VM console's usual state. It also delivers none on a static
desktop. The test therefore shows a 64x64 non-activating window that repaints
on a timer. Each test window turns the display on (`SC_MONITORPOWER` sent to
itself, plus `ES_DISPLAY_REQUIRED` while it lives) and waits for
`GUID_CONSOLE_DISPLAY_STATE` to report the display on. The VM console turns
the display off again about a second later anyway. Duplication then stalls
and reports the pointer hidden, so the windows wake the display again each
time it reports off.

The pointer cells never move the user's pointer. They show a static window
around wherever the pointer already is and change only that window's cursor,
using `SetCursor` on the window's own thread. Each change is therefore a
pointer-only update. What was observed on the VM (Basic Display adapter):

- Color shapes are hardware pointers. DXGI reports them, and the backend
  composites them. `PointerPosition` is the shape's top-left, so position
  plus hotspot is the pointer location. Color shape data is premultiplied: a
  50% red pixel arrives as BGRA `0,0,128,128`.
- Monochrome and masked-color shapes are software cursors. DWM draws them into
  the desktop image itself and DXGI reports no pointer, so the backend draws
  nothing. Such cursors are captured even with `show_cursor` off.
- The desktop is never quiet for long. There is a full redraw after each
  display wake, the taskbar updates, and the scheduled task's console caret
  blinks about every 530 ms. The `show_cursor`-off cell therefore waits
  150 ms after each shape change. It counts an attempt only when the call
  returns IDLE and the backend consumed the pointer update in the meantime.

### Rotation (md-dbg)

Desktop Duplication delivers a rotated output's image in its native,
unrotated orientation. The backend rotates it into a CPU frame in desktop
orientation. The convention follows Microsoft's DXGIDesktopDuplication
sample and libwebrtc: `ROTATE90` turns the image 90 degrees clockwise.
Frames, `ctx->width/height` and the output rectangle are therefore all in
desktop orientation, and input maps onto them linearly. The pointer is
composited without a transform, because DXGI reports its position in
desktop coordinates and its shape upright. Unrotated outputs keep the
zero-copy path.

The synthetic cells check every pixel of each rotation against the
sample's forward mapping. The VM's single output cannot be rotated, so the
console cell forces each rotation onto it. A static window must then
appear exactly where the rotation puts it in real frames. A real rotated
monitor, and the pointer convention on one, are not covered on the VM.

### All outputs as one frame (md-246)

`--capture-output all` (`MdCaptureConfig.output = "all"`) duplicates every
attached output, each on a D3D11 device for its own adapter. The outputs
are composed into one CPU frame that covers the bounding rectangle of their
physical desktop rectangles:

- Negative origins work, and gaps between outputs are opaque black.
- Each output keeps its own rotation and first-frame tracking.
- `md_capture_get_output_rect` reports that bounding rectangle, so
  frame-relative input maps onto it linearly. Agent clicks in UIA global
  coordinates convert back exactly.
- Pointer ownership follows Microsoft's sample. A "not visible" report only
  counts from the output that placed the pointer last, and of two visible
  reports the newer wins.

Limits:

- DXGI has no waitable handle per duplication, so `get_frame` polls the
  other outputs and blocks on one of them in turn for at most a frame
  interval. An update on another output can wait that long, and an idle
  desktop costs one wake-up per interval. A single selected output still
  blocks for the whole timeout.
- Every update copies that output's whole image into the frame.
- Outputs attached or detached later are picked up on the next restart, and
  the host does not yet resize its encoder or input bounds after a restart
  (md-pvlj).

The synthetic cells cover a three-output layout (negative origin, gap,
portrait), pixel-exact composition, the pointer save-under across outputs,
and pointer ownership. The console cells run `all` on the VM's single
output through the rotation and pointer cells, which exercises the composed
path on real frames. Real multi-monitor and multi-adapter hardware is not
covered.

## Real lock/disconnect qualification

The normal liveness test injects provider errors at the root-call boundary and
sends session messages through the real listener window. These do **not** prove
that Windows delivered an actual lock/disconnect notification.

For that separate, disruptive cell, coordinate console access first. Launch
`test_uia_liveness.exe --wait-for-loss` in the interactive console (or an
Interactive scheduled task), then lock the VM with Win+L or disconnect that
session within 30 seconds. The test waits on an event, asserts terminal loss,
then destroys the backend. Save its output/exit code, unlock the console, and
remove any scheduled task. Repeat separately for disconnect if required.

## Lifetime and COM coverage

Both session modes exercise root errors (`RPC_E_DISCONNECTED`,
`UIA_E_ELEMENTNOTAVAILABLE`, disconnected objects and unavailable RPC servers),
non-terminal resource failure, foreign-session filtering, terminal lock/logoff/
disconnect messages, a late callback after backend destruction, incompatible
STA rejection, and destruction while one root call is blocked and another tree
caller is admitted but queued. Synchronization uses events/semaphores, not
sleep-based ordering. Test seams exist only in the standalone test translation
unit, never in the production backend object.

The liveness predicate performs only an Interlocked read. WTS monitoring runs
from initialization until destruction, independent of change subscriptions.
Root-provider failures are observed when a root is accessed; no periodic COM
probe detects a silent provider failure while idle. Loss is terminal: unlock or
reconnect requires a new context. Owners must stop starting new API calls before
destroy; admitted tree calls and late callback dispatch are drained internally.

Also compile `src/core/a11y_uia.cpp`, `capture_dxgi.cpp`, `input_sendinput.cpp`
and the three Windows test translation units with MSVC ARM64
`/std:c++17 /W4 /WX /EHsc /permissive-`. Use real dependency headers or the
VM's type-only pthread/cJSON stubs for this compile-only check. Do not define
`WIN32_LEAN_AND_MEAN` for the UIA files (it breaks UIAutomationCore headers).
Compile `tests/test_win_dpi_win.c` as C (`/std:c11 /W4 /WX /Tc`) and link
it with `user32.lib`: it stands in for `src/host/main.c`'s use of
`win_dpi.h` until the product builds on Windows.
