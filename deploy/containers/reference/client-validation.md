# Container client qualification — md-ah9.11

Validated 2026-11-18 on the Ubuntu 24.04 aarch64 VM, **rootless Podman 4.9.3**.
A containerized `metadesk-client` received, decoded and (in the GUI cell)
displayed real H.264 frames from a containerized host. Each run used real Nostr
session negotiation over a private relay with throwaway keys. The decode-only
(headless) result and the display authorization results are reported
**separately** below. Driver result: **73/73 checks**, 79 s, aggregate exit 0
(final run after review hardening; the prior run was 69/69).

**Re-run for md-rk4 + md-33e** (2026-11-18, same VM and pins, master `66c1886`
plus the fix): **44/44 checks**, 53 s, exit 0, with images built from this tree
(`rk4-*` tags). The driver no longer uses `stdbuf`, and the two no-display GUI
cells are now clean refusals. They stop before any host, relay or signer work,
so they need no host or fixture of their own; that is why there are fewer
checks.

**Re-run for md-cdz + md-rnc** (2026-11-18, same VM and pins, master `85af248`
plus the change, `cdz-*` tags): **73/73 checks**, 81 s, exit 0. Every cell now
shares **one private relay that retains history** (no fresh relay per cell).
A new repeat cell runs two more back-to-back negotiations on it, and every
later session must succeed while ignoring all earlier ones (md-cdz, below).
The GUI image no longer installs `libopengl0`; the client skips SDL's `opengl`
renderer and presents through `opengles2` (md-rnc, below).

**Re-run for md-817x** (2026-11-19, same VM and pins, master `7cb7ab5` plus
the driver change only, `t817-*` tags): **90/90 checks** (86 PASS + 4 INFO), exit
0, ~140 s wall on the shared VM. The repeat cell's assertions are hardened (Cell
2, below): each earlier request/acceptance must be ignored **exactly once** per
later session, matched by `request_id`, and the "never answered" negatives are
judged only after both logs have drained to EOF. A mutation check shows the old
assertions miss, and the new ones catch, a duplicate or late answer.

Scope boundary: the FIPS control endpoint is a **fixture** and TCP crosses a
TUN-less IPv6 container bridge to the host's npub-derived address. This is not
a FIPS daemon, mesh or encrypted-transport receipt; that is **md-ah9.6**.
Nothing touched the VM's GNOME session, its buses or its runtime directory.

## Host choice: Tier B Sway/xdpw container

The host is the Tier B image (`Dockerfile.video`, unchanged) with
`/dev/dri/renderD128`, `WLR_RENDERER=gles2` and explicit
`MD_VIDEO_CONSENT=allow-isolated-output`. It captures **only its own** headless
`HEADLESS-1` output (1920×1080). A small in-container loop changes that output's
background colour, so the capture has damage to encode.

A loopback test host was rejected. The host has no synthetic frame source, and
PipeWire portal capture is the only Linux path. `--no-capture` sends no video at
all, so it can't show decode, and a loopback host capturing the VM desktop would
touch the live GNOME session.

## Pins

| Component | Measured pin |
|---|---|
| metadesk | master `8eebb6c` (this branch adds no `src/` change) |
| Bundle | `throwaway-bundle.sh … -Dclient=true -Dsigner_nip5f=true -Dsigner_nip46=true`, `-j2`, 151 MB |
| nostrc build | VM `~/nostrc/_build`, `da2e4d62bb41` (only `.github/` workflow files dirty) |
| Runtime | Podman `4.9.3`, crun, kernel `7.0.0-42-generic`, base `ubuntu@sha256:73b99939…c4d6a8` |
| Host image | `localhost/cont-video:8eebb6c` = `e3e45603dfe4…`; Sway 1.9, xdpw 0.7.1, PipeWire 1.0.5 (Tier B pins) |
| Decode image | `Dockerfile.client --target decode` = `4ae6a0dacc3a…` |
| GUI image | `Dockerfile.client --target gui` = `e8057023bdd8…`; Mesa `25.2.8-0ubuntu0.24.04.5`, glvnd `1.7.0-1build1`, Wayland `1.22.0-2.1build1` |
| Codec | host x264 `0.164.3108` (NVENC probed, `libcuda.so.1` absent), client FFmpeg `libavcodec60 6.1.1-3ubuntu5` |
| SDL | bundled `libSDL2 2.30.0+dfsg-1ubuntu3.1`, ImGui `1.90.4` |

md-cdz/md-rnc re-run pins: metadesk master `85af248` plus the change; nostrc
unchanged (`da2e4d62b`); bundle 152 MB. Images: `localhost/cdz-video:wip` =
`040b51bf31c9…`, `cdz-client:wip-decode` = `050dcd3eeb4d…`, `cdz-client:wip-gui` =
`70add021745b…`. The GUI image has `libegl1`/`libgles2` `1.7.0-1build1` and
**no** `libopengl0`/`libgl1`. Other package pins are unchanged.

## Topology and session path

- One task network, `cont-ah911-net`, dual-stack: `10.89.97.0/24` plus the `/64`
  containing the host's derived ULA. The host gets `--ip6 <ULA>` and runs
  `--bind <ULA> --port 7400 --no-input --capture-unattended`.
- A fixture container (`client-fixture.py`, Tier B image for Python and
  `libmetadesk`) at `10.89.97.10` provides:
  - the `tests/` private NIP-01 relay, with real REQ/EOSE/OK/CLOSED;
  - a throwaway NIP-46 bunker per role (`nip46-bunker.c`), holding a private
    0600 key file and a random transport key and `connect` secret;
  - a NIP-5F signer for the host account (`nip5f-crypto.c`);
  - a FIPS control socket that reports the host peer as connected.
- **One fixture/relay serves the whole run and keeps every event** (md-cdz).
  Each negotiation still gets a new host and client process, so each later
  host replays every earlier request and each later client replays every
  earlier acceptance from relay history.
- `metadesk-host account add/bind-role host nip5f` (once) verifies the live signer
  owner. `access add CLIENT_NPUB video` publishes a v1 `video` grant, and
  `access list` must equal exactly `<client>\tvideo`.
- Host and client run with an explicit `--bunker URI` (NIP-46): NIP-17
  request/accept DMs, then the one-use token `session_bind` and ACK over TCP.
  The URI includes the throwaway connect secret in argv, because `--bunker` has
  no other input; use a managed signer in deployments.
- Every role container: keep-ID UID 1000, read-only root, `--cap-drop all`,
  no-new-privileges, `--init`, PID/memory limits, tmpfs runtime. Only the host
  gets the render node, and no container gets a host socket.

## Cell 1 — decode-only (headless)

`metadesk-client --npub HOST --fips-control … --relay … --no-display --bunker …`
in the **decode** image. All checks PASS:

| Check | Result |
|---|---|
| Non-allowlisted client (stranger key, own bunker) | host `REJECTED session request from non-allowlisted pubkey`; client `timed out waiting for session accept`, exit 1; no TCP client |
| Host media | `capture started (1920x1080)`; `encoder ready (x264, 1920x1080)` |
| Negotiation → binding | `session accepted`; host `accepted connection from fda4:…::8` (bridge IPv6), `streaming to client #1` |
| Decode | `decoded=94 displayed=0 decode_avg=1.3ms`; at SIGTERM `decoded 102 frames, displayed 0`; host `sent 102 frames` |
| Sandbox | CapEff `0000000000000000`; no `/dev/dri`; `WAYLAND_DISPLAY`/`DISPLAY` empty; empty runtime dir; read-only, 0 devices |
| Image | no EGL/GLES runtime. `libwayland-client/cursor/egl` are present only as Ubuntu libSDL2's DT_NEEDED closure, with no socket or env |
| Legacy direct-IP client (`metadesk-client ULA --port 7400`) | host `rejected unproven client #2`; drained client summary `decoded 0 frames`; no stream (fails closed) |
| Control readiness | every `--npub` client queried fixture `show_peers` (2 in this cell). A non-ready-peer cell is **not** covered; that is md-ah9.6 with a real daemon |
| Client SIGTERM | exit 0 in 0.17 s, summary printed |

`--no-display` is a real decode-only mode: the executable still contains SDL
and still requests capabilities (granted `video` here), so it isn't a
semantic-only agent client. It needed no new bead.

## Cell 2 — repeat negotiations on one retaining relay (md-cdz)

After the decode cell, two more headless host/client pairs (`repeat-1`,
`repeat-2`) negotiate back to back on the same relay, then the GUI cell is the
fourth negotiation. The driver records each client's logged `request_id`
(8-hex prefix) and session id. Every later negotiation must stream (≥ 60
decoded frames, SIGTERM exit 0), and:

- the new host logs `ignoring session request from relay history` for
  **each** earlier client request_id **exactly once**, never for the one it
  answers, nothing for any other client request_id, and never `REJECTED` or
  ignores a request as stale (the stranger's old request is history too);
- the new client logs `ignoring session_accept not answering the current
  request (request_id=…, history)` for **each** earlier acceptance **exactly
  once**, never for its own, never with the tag `live` (a live ignore is a
  duplicate or late answer), and accepts a session id that no earlier session
  used;
- the host logs `received session request` from the client exactly once and
  the client logs `session accepted` exactly once.

These are judged in `Run.correlated_final`, which runs only after
`finish_client` and `finish_host` stopped both containers and drained both
`podman logs -f` streams to EOF (`Logs.drain`; the driver fails if either
stream did not reach EOF). Earlier versions checked once, right after the
stream reached 60 frames, so a line arriving later could not fail them.
Comparison is by sorted request_id lists: the ids are unique, so equality means
"each earlier id exactly once, nothing else". Waits stay event barriers (log
lines, process exit, EOF); there are no sleeps.

Final run, all PASS:

| Negotiation | Earlier sessions ignored (host requests / client acceptances) | Answered | Decoded |
|---|---|---|---|
| decode | none (empty relay) | `dc9da4be` | 103 at SIGTERM |
| repeat-1 | `dc9da4be` / `dc9da4be` | `c4dc0be0` | 98 |
| repeat-2 | `dc9da4be c4dc0be0` (+ stranger `be13d388`) / both | `52d57515` | 98 |
| gui | all three (+ stranger) / all three | `fe8386f6` | 106, displayed 106 |

Fixture totals for the run: 23 REQ, 23 EOSE, 229 events and 229 OK on one
relay; `show_peers` 5 for 5 `--npub` clients. Before md-6ns this cell failed
at the second negotiation (stale acceptance, host `rejected unproven
client #1`).

### md-817x: the hardened assertions catch regressions (mutation check)

The final run is all PASS: every later session ignored each earlier request_id
exactly once on the host and on the client, answered once, no live ignore:

| Negotiation | Host ignored once each | Client ignored once each | Answered |
|---|---|---|---|
| decode | none | none | `a6203cf5` |
| repeat-1 | `a6203cf5` | `a6203cf5` | `71e2fcd4` |
| repeat-2 | `a6203cf5 71e2fcd4` | both | `b9838643` |
| gui | all three | all three | `3e9d2eda` |

To prove the new checks can fail, scratch copies of the old (`7cb7ab5`) and new
driver were mutated and run on the VM with the same images (the mutation was
**never committed**). The mutated driver runs the host/client under a `sh`
wrapper and, once the negotiation's `request_id` is known, makes the container
print one extra line to its real stdout, so it goes through `podman logs -f`
and the EOF drain like any product line:

- **duplicate**: a second `ignoring … from relay history` /
  `ignoring session_accept … history` line for the first earlier request_id,
  printed by the host while it runs and by the client after it exits;
- **late answer**: after the client process exited, `client: ignoring
  session_accept not answering the current request (request_id=<own>…, live)`.

| Mutation | Old driver (`7cb7ab5`) | New driver |
|---|---|---|
| duplicate history ignore | **PASS**, 73/73, exit 0 | **FAIL**, 11 failing (repeat-1/2/gui: host and client "exactly once", live/late), exit 1 |
| late own-request answer after exit | **PASS**, 73/73, exit 0 | **FAIL**, 8 of 90 failing (every cell: client "exactly once" and "no live or late duplicate answer", `live ignores [<own request_id>]`), exit 1 |

The dup run's other failures (a 5.2 s client stop, `show_peers=0`) came from
the `sh` wrapper in the mutation harness, not from the new checks. The late
answer run had only the intended failures. In an earlier late-answer run, the
new checks also caught an unrecorded request: a client whose stream dropped
(the wrapper delayed its stop) sent a second request, and later hosts ignored
it as history. The sorted-equality check reports it.

The relay itself was also mutated once (scratch copy of `tests/`
`account_startup_fixture.py`): every kind-1059 event re-broadcast verbatim 9 s
after storing (a late duplicate of the same event id). Neither side logged a
second ignore, so the stack suppresses repeats of one event id; the new
checks passed on those cells. That run's GUI cell failed its frame threshold
for an unrelated reason (12 decoded frames, 12 s SIGTERM on the loaded shared
VM; the unmutated final run decoded 107 frames in that cell). A relay replaying a *new*
event id for an old answer cannot be forged without signing keys, so the
product-level duplicate case is covered only by the log-level mutation above.

## Cell 3 — GUI: display authorization results (separate)

Display is a separate private Sway container (`--network none`, pixman,
**no devices**, `MD_VIDEO_CONSENT=deny`). Granting display means bind-mounting
**only** its mode-0700 `wayland-1` socket into the client's private runtime
dir, with `WAYLAND_DISPLAY` set. The granted sub-cell uses a fresh host on the
shared relay.

| Display grant | Result |
|---|---|
| **Granted socket** (`SDL_VIDEODRIVER=wayland`) | PASS: client logs `render: SDL video driver 'wayland'`, `render: skipping SDL renderer 'opengl': cannot load libGL.so.1 or libOpenGL.so.0`, `render: SDL renderer 'opengles2' (accelerated)`, `client: decoded frame size 1920x1080`; `decoded=98 displayed=98`; Sway tree shows a visible `xdg_shell` view `app_id=metadesk-client`, `name=metadesk`, 1920×1055 on `HEADLESS-1`; SIGTERM exit 0, `displayed 106` |
| **Nothing granted**, default env | PASS (refused, md-33e fixed): `render: SDL video driver 'offscreen' has no visible output` → `ERROR: no usable display for the GUI client; grant a display or use --no-display for decode-only`, exit 1. No bunker connect, relay, FIPS control query or session request (`show_peers` counted only the granted client). *Before:* SDL silently used `offscreen` (Mesa EGL), reported `displayed=96`, exit 0, nothing visible. |
| **Nothing granted**, `SDL_VIDEODRIVER=wayland` | PASS (refused, md-33e fixed): `render: SDL_Init failed: wayland not available` → the same `ERROR`, exit 1, before any signer or session work. *Before:* `WARNING: SDL2 renderer unavailable, decode-only mode`, decoding continued, exit 0. |

The client refuses SDL's headless video drivers (`offscreen`, `dummy`,
`evdev`), and it checks the display at startup, before signer or session work.
A window or renderer failure after connecting is also fatal (exit 1). Only
explicit `--no-display` decodes without a window. The `displayed` counter
still counts frames handed to SDL, so the driver keeps checking the
compositor tree for a visible view.
Wayland authorization is socket access alone; the private socket here is
mode 0700 and owned by the keep-ID user.

### Renderer selection (md-rnc)

SDL 2.30's GL renderers on Wayland **segfault** (exit 139), rather than fail,
when a library they need is absent: `opengl` without `libGL.so.1` or
`libOpenGL.so.0`, and `opengl`/`opengles2` without `libEGL.so.1` or
`libGLESv2.so.2`. md-ah9.11 worked around the first case by installing
`libopengl0`. A crash cannot be retried, so `src/client/render.c` now plans
the order instead of calling `SDL_CreateRenderer(-1)`:

1. The `SDL_RENDER_DRIVER` hint first, if it names a driver; then the other
   accelerated drivers in SDL's order (`opengl`, `opengles2` here); then
   `software`.
2. Before a GL driver is tried, its libraries are probed with
   `SDL_LoadObject`. On non-X11 video drivers that means desktop GL + EGL for
   `opengl` and GLES + EGL for `opengles2`; X11 (GLX) requires only desktop GL
   of `opengl`. An SDL library override (`SDL_VIDEO_GL_DRIVER`,
   `SDL_VIDEO_EGL_DRIVER`) is probed alone when set. A failed probe logs
   `render: skipping SDL renderer 'NAME': cannot load LIB` and the driver is
   not tried, not even when it is the hint.
3. Wayland has no native window framebuffer, so SDL's `software` renderer
   presents through a texture on the first non-software driver it can create,
   which would be `opengl` again. Once anything is skipped, the client pins
   `SDL_FRAMEBUFFER_ACCELERATION` to the first planned accelerated driver
   (or `0` if there is none). An operator's own env setting still wins.
4. Each failure logs `render: SDL renderer 'NAME' failed: …`, and the choice
   logs `render: SDL renderer 'NAME' (accelerated|software)`. If nothing can
   be created, md-33e's fatal path applies (exit 1).

Full driver runs on this VM. The first row is the gate. The others used a
derived image or an injected env var, so they are not part of the gate. Rows 1,
3 and 4 ran the final binary. Rows 2, 5 and 6 ran a build that differed only in
logging an empty SDL error as blank rather than `no error reported`.

| GUI image / environment | Renderer log | Result |
|---|---|---|
| shipped `gui` (no `libopengl0`) | `opengl` skipped → `opengles2` (accelerated) | PASS 73/73; frames displayed, window visible |
| `gui` + `libopengl0=1.7.0-1build1` | `opengl` (accelerated), nothing skipped | PASS 73/73 (the cell expects `opengl` when the image has desktop GL) |
| shipped, `SDL_RENDER_DRIVER=opengl` | `opengl` skipped despite the hint → `opengles2` | PASS 73/73; 105 displayed |
| shipped, `SDL_RENDER_DRIVER=software` | `opengl` skipped → `software` (framebuffer on `opengles2`) | 104 displayed, window visible, SIGTERM exit 0; only the "expects `opengles2`" check fails, by design. **Before step 3: exit 139** |
| `libEGL.so.1` removed | `opengl` and `opengles2` skipped; `software` failed | exit 1, `ERROR: cannot open the GUI window`, no crash. SDL alone: exit 139 |
| `libGLESv2.so.2` removed | `opengl` and `opengles2` skipped; `software` failed | exit 1, no crash. SDL alone: exit 139 |

`SDL_VIDEO_EGL_DRIVER` pointing at a nonexistent file also crashes SDL's
`opengles2` while `libEGL.so.1` exists; the probe treats the override as the
only candidate, so it is skipped too.

**`libopengl0` was dropped from `Dockerfile.client`.** The client no longer
needs it, and the shipped image now exercises the fallback path on every gate
run, so a regression in the probe shows up as a crash instead of being
masked by packaging. The desktop-GL path stays covered by the `libopengl0`
comparison run above and by desktop installs.

## Findings and beads

- **md-ebm (P2, bug):** nostrc's NIP-5F client strips the quotes from string
  results but never unescapes them. A NIP-5F-backed host rejects every NIP-17
  gift wrap: `event parse failed … (offset 2)` → `DM rejected: invalid-seal`.
  That's why runtime signing uses NIP-46 bunkers here; `access` (sign-only)
  works with NIP-5F.
- **md-6ns (P2, bug):** the client accepts a stale `session_accept` replayed
  from relay history. On a retained-history relay, the second negotiation
  accepted the previous session's ID, and the host correctly failed it
  closed (`rejected unproven client #1`). *Fixed by md-6ns* (request_id
  correlation and a host live/EOSE gate; see `docs/stream-binding.md`).
  **md-cdz** removed the fresh-relay-per-cell workaround and added the
  same-relay repeat cell (Cell 2).
- **md-33e (P3, bug, fixed):** GUI display authorization failed open and was
  unobservable. GUI mode now fails closed (above) and logs the SDL video
  driver, the renderer and the decoded frame size. The SDL `opengl` crash
  without `libOpenGL.so.0` is now handled in the client (**md-rnc**, fixed;
  renderer selection above), and `libopengl0` is gone from the image.
- **md-rk4 (P3, bug, fixed):** both binaries block-buffered stdout without a
  TTY. `podman logs` showed `waiting for session requests` only after the
  host's 60 s request timeout. Both now line-buffer stdout: the client at
  startup, the host only in video mode, because MCP stdio owns fd 1. The host
  cell checks that the readiness line arrives while the host is running. The
  driver and `tests/access_session_e2e.py` no longer use `stdbuf`.
- Saved NIP-46 accounts would need a Secret Service, and Tier B's
  `sway-session.sh` deliberately replaces the session bus with its private
  portal bus. So explicit `--bunker` was used rather than editing Tier B
  files. A host-role restore from NIP-46 inside Tier B stays unqualified.
- The fixture relay drops connections idle for 45 s (a `tests/` safety bound).
  The shared relay and its bunkers now live for the whole ~80 s run. nostrc
  sends a WebSocket PING after 25 s of silence and the relay answers it, so
  the long-lived bunker connections stay up. Each host/client process
  connects fresh and finishes well inside the bound.
- md-33e's no-display check read the refusal line after a 5 s drain. In one
  run `podman logs -f` delivered it later than that, after the container had
  exited. The check now waits for the line itself, then drains.

## Reproduce

On the VM, in a task-owned directory and on matching Ubuntu 24.04/arm64:

```sh
reference/throwaway-bundle.sh SRC ~/nostrc ctx/runtime-bundle \
  -Dclient=true -Dsigner_nip5f=true -Dsigner_nip46=true
cp Dockerfile.video Dockerfile.client ctx/; cp reference/sway-session.sh ctx/reference/
podman build --layers -t localhost/cont-video:REV -f ctx/Dockerfile.video ctx
podman build --layers --target decode -t localhost/cont-client:REV-decode -f ctx/Dockerfile.client ctx
podman build --layers --target gui -t localhost/cont-client:REV-gui -f ctx/Dockerfile.client ctx
```

Compile `nip5f-crypto.c` with the command in its header. Compile
`nip46-bunker.c` with the `access_bunker_fixture` target's compile and link
arguments from `SRC/build-container` (`compile_commands.json` and
`build.ninja`). Then populate a **new** work directory: `mesh-peer.py`,
`client-fixture.py`, `tests/account_startup_fixture.py`, `bin/nip5f-crypto`,
`bin/nip46-bunker`. Run:

```sh
python3 -B validate-client.py --work WORK --video-image localhost/cont-video:REV \
  --decode-image localhost/cont-client:REV-decode --gui-image localhost/cont-client:REV-gui
```

The driver generates three throwaway keys and creates `PREFIX-*` (default
`cont-ah911`, `--prefix`) containers and network. It removes all of them on exit, writes
`WORK/evidence.json` (results, fixture counters, logs; no keys or tokens). It
exits nonzero on any FAIL, including a setup failure that stops later cells.
It removes the network only if it created it. Its waits follow `podman logs -f` lines through a
condition variable, and deadlines only bound failure.

## Nostr protocol review (diff-scoped, A–J)

The new fixtures and driver add no product protocol path. The relay is the
existing `tests/` relay, bound to a container address; it replays history then
sends EOSE, and acknowledges every EVENT with OK. Clients and host gate on
EOSE/OK in the unchanged nostrc/metadesk layers.

- Live delivery is REQ-subscription push. Nothing polls relays. No subscription
  is closed on a timer.
- Each bunker answers only its bound transport. A refused `connect` returns an
  explicit error rather than silence, and replies go through the bunker's
  request/complete API.
- The only waits are on container log lines (event-driven condition variable).
  Bounded deadlines only declare failure, and the GUI tree check is a
  single query.
- The two protocol gaps exposed (md-6ns correlation, md-ebm escaping) are filed,
  not worked around in product code.

`nostr-protocol-smells` skill pass (report-only):
- **Medium F (resolved by md-cdz):** fresh relays per cell kept this suite
  from catching md-6ns. One retaining relay now serves every cell, and the
  repeat cell asserts history is ignored by request_id and liveness.
- **Low F:** the bunker accepts a reused `connect` secret (test-only). With one
  fixture per run this is now relied on: every host (or client) process
  re-presents its role's secret, and the bunker serves only the latest.
- **Low J:** no `OK false`/`CLOSED`/`AUTH` negatives in these cells; md-pss/md-ikr
  cover them.
- **md-cdz pass (diff-scoped):** the repeat cell waits only on log lines, and
  each client starts after its host's post-EOSE readiness line, so the
  live/history split is the EOSE boundary, not timing. Low J ("never answered
  history / never REJECTED" checked once, after the session streams) and Low F
  (each earlier request_id ignored at least once, not exactly once) were
  optional test hardening, **resolved by md-817x**: the negatives now run after
  both logs drained to EOF, and each earlier request_id must be ignored exactly
  once on each side.
- **Out of diff, needs review:** the reused `tests/` relay closes idle sockets
  after 45 s, a timer-driven close. Cells finish well inside it.

## Cleanup

All `cont-*` containers, networks and images (removed by exact tag or ID), the
task Podman API service, the bundle/build tree, throwaway keys and `~/wt-cont`
were removed. Only the shared Ubuntu base image remained. VM free disk:
9.0 GB at start, 6.0 GB lowest, **7.4 GB** at the end (`df -h /`). Other agents
shared the VM, and it never went below 3 GB. GNOME was never touched.

md-rk4/md-33e re-run: the `rk4-*` images (removed by exact tag), the bundle and
build trees, throwaway keys and `~/wt-rk4` were removed, and the driver removed
its containers and network. VM free disk was 7.4 GB at start, 6.3 GB lowest and
7.3 GB at the end. GNOME was never touched; host-side SDL probes forced
`dummy`/`offscreen` or pointed `XDG_RUNTIME_DIR` away from the session.

md-cdz/md-rnc re-run: the `cdz-*` images and the derived probe images
(`probe-noegl`, `probe-nogles`, `wip-gui-gl`) were removed by exact tag, and
dangling layers were pruned. The bundle and build trees, throwaway keys, probe
display container and `~/wt-cdz` were removed. The driver removed its
containers and network. Only the shared Ubuntu base image remained. VM free
disk was 9.0 GB at start, 5.5 GB lowest and 8.9 GB at the end; the VM is
shared with other agents. GNOME was never touched: every SDL probe ran inside
a container against the private Sway socket.

md-817x re-run: the `t817-*` images (removed by exact tag), dangling layers, the
bundle/build trees, scratch mutation drivers, throwaway keys and `~/wt-817x`
were removed; the driver removed its containers and network on every run (none
named `t817*` remained). Only the shared Ubuntu base image remained. VM free
disk was 8.9 GB at start, 6.3 GB lowest and 8.7 GB at the end. GNOME was never
touched.
