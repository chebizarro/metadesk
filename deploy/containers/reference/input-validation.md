# md-ah9.9 — private wlroots input validation

Validated 2026-11-17 (PT), Ubuntu 24.04 aarch64 VM, rootless Podman 4.9.3,
Sway **1.9-1build2**, wlroots **0.17.1-2.1build1**, pixman/headless,
Wayland 1.22.0, xkbcommon 1.6.0. No render node was needed for input.
The existing video Dockerfile/session launcher were not changed.

## Selection and contract

Build with `-Dwayland_input=true` (default **false**). Only this option adds
Wayland client/server, xkbcommon and scanner dependencies. XML provenance,
licenses and hashes are in `protocols/README.md`; virtual-keyboard is carried
by wlroots, not the released wayland-protocols package.

```sh
metadesk-host --input-backend wayland \
  --input-wayland-display /absolute/private/runtime/wayland-1
```

The API equivalents are `MdInputConfig.backend = "wayland"` and
`.wayland_display = "/absolute/socket"`. Missing/relative sockets, unavailable
protocols/seats, policy denial, or an unbuilt backend fail without uinput
fallback. `auto`/NULL keeps the existing platform default. The backend connects
to the explicit socket itself, ignoring `WAYLAND_DISPLAY` and `WAYLAND_SOCKET`.

Scope: first advertised seat, compositor-wide absolute coordinates; the
reference has one 1920x1080 scale-1 output. Coordinates clamp to the configured
pixel range, with width/height passed as protocol extents. Buttons 0/1/2 map
to BTN_LEFT/RIGHT/MIDDLE. Scroll units are detents: positive X right, positive Y
up; Wayland Y is inverted, with ten logical pixels per detent.

Keyboard commands retain the HAL's evdev KEY_* convention. Text uses a generated
US pc105 keymap: printable ASCII, tab and newline, up to 4096 bytes. Unsupported
UTF-8/control characters, held keys or latched/locked modifiers reject the whole
string before injection. This is not an IME or arbitrary Unicode entry API.
Shift is held through character release. Protocol I/O and readiness reads are
serialized. Barriers have a two-second I/O deadline; detected failure disables
the context and shuts its connection down. Idle disconnect is detected on the
next operation, not by a background reconnect loop. Destruction releases held
keys/buttons best-effort; compositor connection teardown removes virtual devices.
Callers must stop using a context before destroying it.

## Real compositor matrix

`validate-input.py` creates **two separate Sway processes**, each with its own
mode-0700 runtime directory and mapped `wev` client. The standalone probe compiles
the production HAL/backend sources; it does not substitute a mock injection path.
The test injects into each session in turn while checking the other receiver's
input counts remain unchanged. Ambient display and socket-FD variables point at
the wrong target/invalid FD deliberately.

| Input | Receiver evidence | Result in both directions |
|---|---|---|
| Absolute motion | `100,150`, then `400,300` logical client coordinates | PASS |
| Buttons | 272 left, 273 right, 274 middle; each pressed and released | PASS |
| Scroll | horizontal -2 / -20 pixels; vertical -3 / -30 pixels for HAL Y +3 | PASS |
| Text | exact key-symbol sequence for `ah9-9 Wayland!` + Return | PASS |
| Modifiers | uppercase W, exclamation, Ctrl+A press/release | PASS |
| Unsupported UTF-8 | `rejecté` rejected; no prefix/key events | PASS |
| Other private session | no added key/motion/button/axis events | PASS |
| VM desktop isolation | no host display/runtime/bus mounts; no uinput or DRI devices | PASS for Wayland proof |

The container had `--network=none --cap-drop=all`, no new privileges, 512 MiB
memory, 128 PIDs and a 64 MiB private `/tmp`. It had no `/run/user/1000`,
`/dev/uinput`, or `/dev/dri`. The final recheck mounted only the rebuilt
probe executable and validator read-only, not a host runtime directory. GNOME was neither
connected to nor instrumented by this proof: isolation is established by the
inaccessible endpoints/device boundary plus the live two-session negative
control, not by a receiver attached to the real desktop.

## Reproduce on the matching Linux producer

```sh
meson setup build -Dwayland_input=true \
  -Dnostrc_root="$HOME/nostrc" -Dnostrc_build_dir="$HOME/nostrc/_build"
meson compile -C build -j2
cc -D_GNU_SOURCE -DMD_HAVE_WAYLAND_INPUT -Isrc/core -Ibuild \
  deploy/containers/reference/wayland-input-probe.c \
  src/core/input.c src/core/input_wayland.c src/core/input_uinput.c src/core/log.c \
  build/wlr-virtual-pointer-unstable-v1-protocol.c \
  build/virtual-keyboard-unstable-v1-protocol.c \
  $(pkg-config --cflags --libs wayland-client xkbcommon libcjson) -lpthread \
  -o ah9-9-input-probe
cp deploy/containers/reference/validate-input.py .
podman build --layers=false -t ah9-9-input \
  -f deploy/containers/reference/Dockerfile.input .
podman run --rm --init --name ah9-9-input-proof --network=none --cap-drop=all \
  --security-opt=no-new-privileges --pids-limit=128 --memory=512m \
  --tmpfs /tmp:rw,mode=1777,size=64m ah9-9-input
```

## Quality gates and deviations

- macOS default options (client enabled, Wayland/signers disabled): **25/25**.
- Linux full suite, client enabled, Wayland **enabled: 26/26**.
- Linux full suite, client enabled, Wayland **disabled: 26/26**. No Wayland or
  xkbcommon DT_NEEDED entry in the resulting core library.
- Linux new input test with Meson `-Db_sanitize=address,undefined`, leak checking
  enabled: **258 checks PASS**, no ASan/UBSan/LSan report.
- The deterministic wire peer covers coordinate bounds, all buttons, scroll
  direction, generated keymap, shifted press/release, Ctrl combos, held-input
  cleanup, concurrent operations/readiness, fallback dimensions, invalid text,
  missing seat/protocols, authorization failure, disconnect and stalled-peer
  timeout. These checks need no desktop devices.
- Two optional native-platform creation checks print an explicit opt-in skip in
  ordinary suites. They require `MD_INPUT_LIVE_TESTS=1` on a disposable desktop;
  they were not opted into for final gates. All default action checks use mocks.
- An earlier container run had a private-AT-SPI fixture mode-0 readiness timeout;
  the final enabled and disabled full runs passed it. No AT-SPI code was changed.
- **Safety deviation:** initial Linux/ASan runs of the inherited input tests
  created uinput devices because the VM user is in the input group. Source
  inspection confirmed real contexts were used only for creation and invalid
  argument checks; actual action dispatch already used mocks, so no test
  pointer/key event was sent through those devices. This still violated the
  no-uinput boundary. `md-2ml` records the regression fix: native device creation
  is now explicit-opt-in and edge checks are mock-only. All final Linux gates
  ran in device-free containers with no host session/runtime mounts.

All task containers, task images (including superseded builds), the accidentally
pulled test base tag, task build caches/directories and `~/wt-ah9-9` were removed
after validation. Shared base images and sibling resources were left alone.
No push or merge was performed.

Exploration used seven `jev.py find-lines` calls (presence 0.85–0.96) to narrow
HAL, plan, Meson, host and test reads: 21 requests, 161323 input tokens,
$0.006775. No relevance-threshold misfire was observed.
