# End-to-end host ↔ client acceptance matrix

Owner: **md-5zi** (full matrix execution and failing-cell bugs). Container
receipts incorporated by **md-ah9.12** against master `c571b5d`; no new runtime
tests were run for that documentation task. Container client cells were added by
**md-ah9.11** against master `8eebb6c`; md-rk4/md-33e re-ran those cells against
`66c1886` plus their fix. This is the owning matrix, not a new E2E harness.

## Status and counting

**PASS** means the cited evidence establishes the named check on that stack;
**FAIL** preserves observed failures, including intermittent ones. **not yet
run** means no qualifying receipt here; **pending** names a prerequisite owner;
**N/A** means a runtime-specific assertion does not apply. Expected-negative
checks pass only when rejection was observed.

A cell is one table body entry in a result column, excluding row labels.
General cells are complete pairing runs; container cells are narrower component
checks, not full E2E passes. Counts exclude the checklist and summary tables.

| Scope | PASS | FAIL | not yet run | pending | N/A | Total |
|---|---:|---:|---:|---:|---:|---:|
| General host/client pairings | 0 | 0 | 32 | 0 | 0 | 32 |
| Containers | 93 | 4 | 4 | 5 | 1 | 107 |
| Total | 93 | 4 | 36 | 5 | 1 | 139 |

## General roles × platforms × transports

Rows are hosts; columns are clients. Every platform appears in both roles,
including same-platform pairings. The historical macOS ↔ GNOME exercise
mentioned in md-5zi is not an all-checks acceptance receipt, so no pairing is
promoted to PASS. Container component checks below do not qualify these cells.

### Direct IP (LAN/IPv6, TUN-less)

| Host / Client | Windows | GNOME/Linux | macOS | Omarchy (Hyprland/Wayland Linux) |
|---|---|---|---|---|
| Windows | not yet run | not yet run | not yet run | not yet run |
| GNOME/Linux | not yet run | not yet run | not yet run | not yet run |
| macOS | not yet run | not yet run | not yet run | not yet run |
| Omarchy (Hyprland/Wayland Linux) | not yet run | not yet run | not yet run | not yet run |

### FIPS mesh (npub, NIP-44 session negotiation, TCP over FIPS TUN)

| Host / Client | Windows | GNOME/Linux | macOS | Omarchy (Hyprland/Wayland Linux) |
|---|---|---|---|---|
| Windows | not yet run | not yet run | not yet run | not yet run |
| GNOME/Linux | not yet run | not yet run | not yet run | not yet run |
| macOS | not yet run | not yet run | not yet run | not yet run |
| Omarchy (Hyprland/Wayland Linux) | not yet run | not yet run | not yet run | not yet run |

General platform runs remain md-5zi work. Container FIPS cells are pending
**md-ah9.5**; its results doc is **to be added by md-ah9.5**. Future container
qualification must not be extrapolated to all platform pairings.

### Required checks for every general cell

Record pass/fail and notes for each check, not just connection success:

| Check | Required observation |
|---|---|
| Connect/auth | Intended peer, authentication and session negotiation; explicit unauthorized-peer rejection. |
| Video | Frames flow, correct dimensions, fills window, no trails. |
| Decode/performance | Decode works; measured FPS and bitrate, codec and encoder/decoder selection. |
| Overlay | Renders and tracks intended content. |
| HUMAN input | Mouse move/button/scroll and keyboard; correct button mapping and 1:1 cursor mapping. |
| AGENT path | MCP tools, a11y tree and action-by-id reach intended target; input-disabled/unready behavior. |
| Reconnect/restore | Disconnect/reconnect, capture restoration, consent/token behavior and failures. |
| Teardown | Clean shutdown, exit status/duration, no leaked session resources. |
| Capture consent/permissions | Windows DXGI; GNOME PipeWire portal/restore_token; macOS ScreenCaptureKit TCC; Omarchy/Hyprland portal. Granted and denied behavior. |
| Transport | Direct IP: endpoint/IP family without TUN. FIPS: identities, NIP-44 negotiation, control readiness, TUN route and TCP delivery; socket existence is insufficient. |

### Repeatable execution and receipts

1. Select host/client OS versions, source commits, transport and isolated target
   desktop; record runtime, codec, permissions and devices. Use disposable
   fixtures for actions, not the operator's desktop.
2. Start the host with platform capture consent/input permissions. For mesh,
   establish control readiness and the route before connecting. Record actual
   launch commands and endpoint/identities without secrets. Platform-wide launch
   recipes are **not yet recorded** here; md-5zi must supply exercised commands
   or scripts with its receipts.
3. Connect and execute every check above, including authorization negatives,
   reconnect/restore and teardown. Observe events/state, not arbitrary sleeps.
4. Attach dated results, exact commands/versions, per-check observations,
   measured values, limitations and cleanup. Link a bug for each failing cell;
   partial coverage does not make a cell PASS.
5. For containers reuse the existing drivers/commands linked below; do not
   introduce a competing harness.

Known-flaky areas: **GNOME screencast restore (md-3nv)** must be exercised,
not inferred from a saved token. **md-atg remains OPEN**: cleared-state and
same-context reopen can reject invalid backend allocations and deliver zero
frames. Later passes do not erase these failures.

## Containers

Local Ubuntu VM/component evidence only: not hosted CI, release certification
or full host/client streaming acceptance. MCP HTTP/stdio are local MCP
transports, not direct-IP video or FIPS session receipts. Proposed axes follow
[container plan §3.9](../plans/container-runtime-2026-11-14.md#39-verification-and-coordination).

### Tier A: attached desktop fixture

Source: [validation.md — Historical fixture qualification](../../deploy/containers/reference/validation.md#historical-fixture-qualification--md-ah94).
Metadesk **8157ac0**, Ubuntu 24.04 aarch64, kernel `7.0.0-42-generic`, GNOME
Wayland seat0, AT-SPI `2.52.0-1build1`, GTK `3.24.41-4ubuntu1.3`, GLib
`2.80.0-6ubuntu3.10`. Docker is rootful, default seccomp, explicit
`metadesk-agent-ah9-4` AppArmor profile; rootless Podman uses keep-ID,
AppArmor disabled, SELinux unvalidated. Compose **2.40.3**.
Real GTK fixture checks, not live GNOME/video/input acceptance:

| Fixture check | Docker 29.1.3 | Rootless Podman 4.9.3 |
|---|---|---|
| MCP HTTP bearer initialize → initialized → tools/list → ui-tree | PASS | PASS |
| MCP stdio, expected button in real tree | PASS | PASS |
| Wrong numeric identity | PASS, exit 1 | PASS, exit 1 |
| Missing session socket | PASS, exit 1, diagnostic | PASS, exit 1, diagnostic |
| Missing secret file at HTTP startup | PASS, exit 1 | PASS, exit 1 |
| Missing bearer on configured HTTP /mcp | PASS, 401 | PASS, 401 |
| Direct host without desktop (bypass preparation) | PASS, nonzero diagnostic | PASS, nonzero diagnostic |
| Input/capture tools unlisted; no devices; no PipeWire socket; no 7700 listener | PASS | PASS |
| Read-only root, private runtime tmpfs, zero effective caps, no-new-privileges | PASS | PASS |
| Explicit 127.0.0.1:7710, private netns, IPv4 client dial | PASS | PASS |
| Docker IPv6-disabled namespace and bridge (`EnableIPv6=false`) | PASS, sysctl=1 | N/A |
| SIGTERM HTTP PID 1 / stdio host / preparation PID 1 (<5 s) | PASS: 1.020 / 0.114 / 0.105 s | PASS: 1.080 / 0.118 / 0.088 s |
| Restart policy/backoff after startup failure (not logout recovery) | PASS, native increasing backoff | PASS, user systemd fixed 2 s + rate limit |
| DT_NEEDED in runtime image | PASS, 144 resolved entries | PASS, 144 resolved entries |

Reproduce with the source's isolated fixture setup, then
`python3 deploy/containers/reference/validate.py docker` and
`python3 deploy/containers/reference/validate.py podman`. Set runtime environment
before starting the fixture bus. Never use its private-bus loss simulation on
a real desktop.

### Tier A: live GNOME

Source: [validation.md — Live GNOME requalification](../../deploy/containers/reference/validation.md#live-gnome-requalification--md-ah916-historical).
Producer **92ab2cf2e37030f0ddb32df2b9d933fdb410be94**, same Ubuntu/kernel/AT-SPI/
GTK/GLib stack above; gnome-shell `46.0-0ubuntu6~24.04.17`, gnome-calculator
`1:46.3-0ubuntu0.1`. Docker **29.1.3**, Compose **2.40.3**, rootless Podman
**4.9.3**. The registry contained one Calculator; gnome-shell had not
re-registered after the earlier bus restart. Checks require the Calculator
window, buttons **0 / 7 / + / =**, and two display text nodes; no input enabled.

| Live GNOME check | Docker 29.1.3 | Rootless Podman 4.9.3 |
|---|---|---|
| HTTP bearer, bind 127.0.0.1:7710, calculator widget tree | PASS | PASS |
| Missing bearer on /mcp | PASS, 401 | PASS, 401 |
| Stdio calculator widget tree | PASS | PASS |
| HTTP SIGTERM, exit 0, <5 s | PASS, 1.030 s | PASS, 1.098 s |
| Stdio child SIGTERM, exit 0, <5 s | PASS, 0.130 s | PASS, 0.131 s |
| No 7700; nine input/capture tools absent; no input/DRI/PipeWire access | PASS | PASS |
| Read-only root, zero effective caps, no-new-privileges, no assigned devices, private netns/no published ports | PASS | PASS |
| Compose healthy → authenticated widget tree → stop exit 0, <5 s | PASS, 1.00 s | PASS, 1.08 s, compatibility API + override |
| Wrong identity / missing session socket / missing secret / direct no-desktop host | PASS, nonzero diagnostics | PASS, nonzero diagnostics |
| DT_NEEDED closure | PASS, 144 resolved entries | PASS, 144 resolved entries |

Reproduce with `validate.py docker --live` and `validate.py podman --live` under
`deploy/containers/reference/`, after the source's bundle/socket/secret setup.
Rootless Compose uses Docker Compose 2.40.3 against a task-owned Podman
compatibility API with override, not native Podman Compose/build qualification.
The historical missing-socket blocked row is superseded by this live evidence.

### Tier A: desktop accessibility-loss restart recovery

Source: [validation.md — Desktop accessibility-loss restart recovery](../../deploy/containers/reference/validation.md#desktop-accessibility-loss-restart-recovery--md-ah94),
source master **323292d**, same VM/dependencies. This latest section supersedes
historical pending md-ah9.15 statements. Loss means controlled AT-SPI
launcher/daemon/registry termination, **not logout or session-bus replacement**.
A changed session-socket inode still requires recreation.

| Recovery check | Docker 29.1.3 / Compose 2.40.3 | Rootless Podman 4.9.3 / user systemd |
|---|---|---|
| Host exits nonzero after desktop loss | PASS: 0.680 s, exit 1 | PASS: 0.641 s, exit 1 |
| Automatic restart owner / backoff | PASS: Compose `on-failure`, one engine restart; die → start 0.259 s (engine initial 100 ms backoff plus runtime overhead) | PASS: Single systemd owner, `Restart=on-failure`, `RestartSec=5s`, burst 5/300 s; one restart; systemd failure → next start 5.093 s, container died → start 5.266 s |
| GetAddress returns new bus | PASS: 0.721 s after loss | PASS: 0.671 s after loss |
| New-address authenticated widget tree | PASS: 3.324 s after loss | PASS: 6.626 s after loss |
| New full address in recovered PID 1 environment; dynamic HTTP health | PASS | PASS, even with deliberately stale `/stale/old-bus` container env |

Socket basename stayed `bus_0`; live basename-change recovery is not claimed.
Timings belong to the first image pair, not the later syntax-hardened pair.
Rootless systemd recovery does not qualify Compose backoff. Follow the source's
controlled-loss procedure only with explicit desktop-owner authorization;
the ordinary live driver is not itself a disruptive recovery run.

### Tier B: isolated Sway capture and policy

Source: [video-validation.md — Rebase and renewed qualification](../../deploy/containers/reference/video-validation.md#rebase-and-renewed-qualification),
with [pinned capabilities](../../deploy/containers/reference/video-validation.md#provenance-and-pinned-capabilities).
Rootless **Podman 4.9.3**, Ubuntu 24.04 aarch64; Sway `1.9-1build2`, wlroots
`0.17.1-2.1build1`, xdg-desktop-portal-wlr `0.7.1-1build2`,
xdg-desktop-portal `1.18.4-1ubuntu2.24.04.3`, PipeWire + libspa-0.2-modules
`1.0.5-1ubuntu3.3`, WirePlumber `0.4.17-1ubuntu4.1`, Mesa
`25.2.8-0ubuntu0.24.04.5`. Actual renderer: **virgl (ANGLE / Apple M4 Max)**,
only `/dev/dri/renderD128`; not software-only or DRI-free. Output: 1920×1080.
Rebased source **142bb28**, image
`5478a00fec7d749fca98d67047d307115485783bff31076feb5967b330c87ac9`.

| Rebased check | Native probe/library | ASan probe/library |
|---|---|---|
| Fresh state | PASS: 10 matching timestamped frames | PASS: 10 matching timestamped frames |
| Saved-token reuse | PASS: 10 matching timestamped frames | PASS: 10 matching timestamped frames |
| Unknown valid UUID | PASS: 10 matching timestamped frames | PASS: 10 matching timestamped frames |
| Malformed token | PASS: Start -1, 0 frames, file retained | PASS: Start -1, 0 frames, file retained |
| Cleared token | PASS: 10 matching frames | FAIL: 0 frames; probe 6, 8 invalid descriptors rejected; md-atg |
| Six-cycle test | FAIL: first start; probe 6, 0 frames, 13 invalid descriptors rejected; md-atg | PASS: 60 frames; all six start/stop cycles pass |
| Static undamaged output in successful cells | PASS: IDLE, no ERROR, token retained | PASS: IDLE, no ERROR, token retained |
| Concurrent stop with borrowed mapping, successful cells | PASS: Mapping intact until release; post-stop ERROR | PASS: Mapping intact until release; post-stop ERROR |
| Kill only private PipeWire after positive capture | PASS: ERROR, token retained | PASS: ERROR, token retained |
| Pixman / deny chooser | PASS: Start -1; zero frames, expected negatives | PASS: Start -1; zero frames, expected negatives |
| Stalled chooser, 500 ms budget | PASS: Start -1 in 0.504 s | PASS: Start -1 in 0.504 s |

Both renewed matrices returned **aggregate 1**, not green; **150 validated
changing-pixel frames** (50 native, 100 ASan). No NULL CPU frame escaped, no
timestamp regression was observed, and no ASan/LSan diagnostic occurred.
Expected pixman rejection is not successful software capture. Historical
md-ah9.8 fallback/zero-timestamp behavior is superseded by md-ah9.10 hardening;
**md-atg remains OPEN**, despite two earlier pre-rebase aggregate-0 matrices.
No full host/client transport, encoder integration or connected-host watchdog
qualification follows. Unknown valid UUID capture uses explicit operator
chooser policy, not proof of retained authorization.

Reproduce using the source's matching producer build, capture-probe compile,
Podman video build and `deploy/containers/reference/validate-video.py` command
with `--render-node /dev/dri/renderD128`. Use private services, explicit
isolated-output consent and `--init`; never target the VM GNOME session.

### Wayland virtual input

Source: [input-validation.md](../../deploy/containers/reference/input-validation.md).
Validated **2026-11-17 (PT)**, Ubuntu 24.04 aarch64 VM, rootless Podman **4.9.3**,
Sway **1.9-1build2**, wlroots **0.17.1-2.1build1**, pixman/headless,
Wayland **1.22.0**, xkbcommon **1.6.0**. No render node needed. Build option
`-Dwayland_input=true` (default false); explicit private socket, no uinput
fallback. Two separate Sway processes with mapped `wev` receivers establish
both-direction and other-session negative controls, not a live GNOME receiver
or an a11y-observed action result.

| Input check | Both directions, rootless Podman 4.9.3 |
|---|---|
| Absolute motion | PASS: `100,150`, then `400,300` logical client coordinates |
| Buttons | PASS: 272 left, 273 right, 274 middle; each pressed and released |
| Scroll | PASS: horizontal -2 / -20 pixels; vertical -3 / -30 pixels for HAL Y +3 |
| Text | PASS: exact key-symbol sequence for `ah9-9 Wayland!` + Return |
| Modifiers | PASS: uppercase W, exclamation, Ctrl+A press/release |
| Unsupported UTF-8 | PASS: `rejecté` rejected; no prefix/key events |
| Other private session | PASS: no added key/motion/button/axis events |
| VM desktop isolation | PASS for Wayland proof: no host display/runtime/bus mounts; no uinput or DRI devices |

Reproduce with the source's enabled build, probe compile, `Dockerfile.input`
and device-free `podman run`; driver:
`deploy/containers/reference/validate-input.py`. Text acceptance is US pc105
printable ASCII/tab/newline, not arbitrary Unicode/IME input. The inherited
native-device-creation safety deviation and md-2ml fix remain documented in
the source; final Linux gates ran without devices or host session/runtime mounts.

### Container client (decode-only and GUI)

Source: [client-validation.md](../../deploy/containers/reference/client-validation.md).
Validated **2026-11-18**: rootless **Podman 4.9.3**, Ubuntu 24.04 aarch64,
metadesk **8eebb6c**; re-run by md-rk4/md-33e on `66c1886` plus the fix (44/44
checks, no `stdbuf`). The host is the Tier B Sway/xdpw container (render node,
GLES2, isolated 1920×1080 output, x264). A loopback host has no frame source
without capturing the live desktop. Negotiation is real NIP-17 over a private
relay, with a v1 `video` grant, throwaway NIP-46 bunkers and one-use stream
binding. The FIPS control endpoint is a fixture and TCP uses a TUN-less IPv6
container bridge to the npub-derived address. This is **not** a FIPS/mesh
receipt. Headless and display-authorization results are separate rows.

| Client check | Rootless Podman 4.9.3 |
|---|---|
| Readiness/progress lines without a TTY (no `stdbuf`) | PASS: host `nostr bridge ready` logged while running; client stats/summary live; **md-rk4** |
| Non-allowlisted client refused before any stream | PASS: host `REJECTED … non-allowlisted`; client exit 1; no TCP client |
| Decode-only (`--no-display`): negotiate, bind, decode | PASS: 1920×1080 x264; 102 decoded, 0 displayed; SIGTERM exit 0 in 0.17 s |
| Decode-only sandbox: no display server/devices/caps | PASS: CapEff 0; no `/dev/dri`; no display env/socket; no EGL/GLES in image |
| Legacy direct-IP client (no session proof) | PASS (fails closed): host `rejected unproven client #2`; no stream |
| GUI, granted private Wayland socket | PASS: logs `SDL video driver 'wayland'`, renderer `opengl`, frame 1920×1080; 100 displayed; visible `xdg_shell` view `metadesk-client` 1920×1055 on the private output |
| GUI, no display granted (default env) | PASS (refused): `SDL video driver 'offscreen' has no visible output`, exit 1 before signer/relay/session work; **md-33e** fixed |
| GUI, no display granted, `SDL_VIDEODRIVER=wayland` | PASS (refused): `SDL_Init failed: wayland not available`, exit 1 before signer/relay/session work; no decode-only fallback (**md-33e**) |
| Repeat negotiation on a history-retaining relay | FAIL: stale `session_accept` replayed; host rejects the stale proof; **md-6ns** |
| NIP-5F signer for a live host session | FAIL: decrypted seal JSON not unescaped, `DM rejected: invalid-seal`; **md-ebm** |

Reproduce with the source's bundle/image builds and
`deploy/containers/reference/validate-client.py` (fresh relay per cell).
The GUI image needs `libopengl0`: without it SDL 2.30 segfaults on Wayland.

### Remaining container acceptance cells

| Cell | Status / owner and required evidence |
|---|---|
| Docker Compose / FIPS shared netns | pending **md-ah9.5** (W1): app has no TUN/NET_ADMIN; mesh route and socket restart recovery. Results doc: **to be added by md-ah9.5**. |
| FIPS socket available but route absent | pending **md-ah9.5**: reject unavailable routing; socket existence is not readiness. Results doc: **to be added by md-ah9.5**. |
| Kubernetes / pinned desktop node | pending **md-ah9.7** (W4): readiness, identity/socket mounts, RBAC port forwarding and clean termination. |
| Binary-session FIPS | pending **md-ah9.6**: binary host/client session over mesh, not MCP-only reachability. |
| Client in container / FIPS | pending **md-ah9.6** (binary session over the mesh): real `fipsd` control readiness before connection and actual decode. Client image and decode were qualified by md-ah9.11 (above). Mesh reference: [mesh-validation.md](../../deploy/containers/reference/mesh-validation.md). |
| MCP HTTP / authenticated proxy | not yet run: unauthorized POST and SSE denied; authorized tree access. Local bearer receipts do not qualify an external proxy. |
| Sway video / software full stream | not yet run: portal capture, changing frames, x264 selection and dimensions together; pinned pixman capture is a known negative, not a software-stream pass. |
| Sway virtual input / a11y-observed result | not yet run: target-only input plus accessible state change; `wev` input receipts alone are insufficient. |
| GPU variant | not yet run: actual hardware encoder selection and software fallback independently verified; virgl capture is not hardware-encode evidence. |

Other unqualified negative axes from §3.9 remain required when running those
cells: missing a11y bus despite available session bus, empty/wrong desktop,
input disabled/unready, portal revocation, and SIGTERM during idle request, SSE
delivery and a11y notification. Recorded negatives qualify only their stated
scenarios. No duplicate bug is needed: reopen failures retain **md-atg**;
GNOME restore retains **md-3nv**. Leave **md-5zi open** for full execution.
