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
- **A fresh fixture/relay is used per cell** (see md-6ns below).
- `metadesk-host account add/bind-role host nip5f` verifies the live signer
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

## Cell 2 — GUI: display authorization results (separate)

Display is a separate private Sway container (`--network none`, pixman,
**no devices**, `MD_VIDEO_CONSENT=deny`). Granting display means bind-mounting
**only** its mode-0700 `wayland-1` socket into the client's private runtime
dir, with `WAYLAND_DISPLAY` set. Each sub-cell uses a fresh host and relay.

| Display grant | Result |
|---|---|
| **Granted socket** (`SDL_VIDEODRIVER=wayland`) | PASS: client logs `render: SDL video driver 'wayland'`, `render: SDL renderer 'opengl' (accelerated)`, `client: decoded frame size 1920x1080`; `decoded=96 displayed=96`; Sway tree shows a visible `xdg_shell` view `app_id=metadesk-client`, `name=metadesk`, 1920×1055 on `HEADLESS-1`; SIGTERM exit 0, `displayed 100` |
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

**Packaging fix (in `Dockerfile.client`):** with EGL/GLES present but no
desktop-GL `libOpenGL.so.0`, SDL 2.30's default `opengl` renderer **segfaults**
(exit 139) on Wayland instead of falling back. Forcing `opengles2` worked, and
adding `libopengl0=1.7.0-1build1` fixed the default path, which then picks
`opengl`. Diagnosis used a probe mirroring `render.c`.

## Findings and beads

- **md-ebm (P2, bug):** nostrc's NIP-5F client strips the quotes from string
  results but never unescapes them. A NIP-5F-backed host rejects every NIP-17
  gift wrap: `event parse failed … (offset 2)` → `DM rejected: invalid-seal`.
  That's why runtime signing uses NIP-46 bunkers here; `access` (sign-only)
  works with NIP-5F.
- **md-6ns (P2, bug):** the client accepts a stale `session_accept` replayed
  from relay history. On a retained-history relay, the second negotiation
  accepted the previous session's ID, and the host correctly failed it
  closed (`rejected unproven client #1`). Fresh relays per cell avoid it.
- **md-33e (P3, bug, fixed):** GUI display authorization failed open and was
  unobservable. GUI mode now fails closed (above) and logs the SDL video
  driver, the renderer and the decoded frame size. The SDL `opengl` crash
  without `libOpenGL.so.0` is still handled by packaging (`libopengl0`); a
  client-side GLES2 fallback is follow-up **md-rnc**.
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
  Each cell finishes well inside it.

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

The driver generates three throwaway keys and creates `cont-ah911-*`
containers and network. It removes all of them on exit, writes
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
- **Medium F:** fresh relays per cell keep this suite from catching md-6ns.
  After md-6ns, add a same-relay repeat-negotiation cell.
- **Low F:** the bunker accepts a reused `connect` secret (test-only).
- **Low J:** no `OK false`/`CLOSED`/`AUTH` negatives in these cells; md-pss/md-ikr
  cover them.
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
