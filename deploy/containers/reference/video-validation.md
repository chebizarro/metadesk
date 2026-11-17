<!-- validated on rootless Podman 4.9.3 / Ubuntu 24.04 aarch64, isolated Sway capture; 2026-11-17. -->
# md-ah9.8 — Tier B isolated-video experiment

## Verdict and boundaries

**Conditional capture compatibility, not DRI-free compatibility.** The pinned
Sway headless/pixman session starts without devices, but xdpw rejects ScreenCast
`Start`; metadesk receives **zero frames**. With only `/dev/dri/renderD128`
assigned, headless GLES2/GBM/virgl delivers changing **1920×1080 RGBX CPU frames**
through the unmodified metadesk PipeWire/portal capture API.

This is local Linux VM evidence, not Docker qualification, release packaging,
full host/client streaming, encoding, FIPS, input, accessibility-tree validation,
or production/unattended-host acceptance. The consumer is `capture-probe.c`,
linked to the **same installed libmetadesk.so** used by `metadesk-host`, not a
mock or independent PipeWire implementation. No `src/` or Meson files changed.
The full host watchdog was **not executed**; its implications below are an
inference from observed capture behavior and the documented watchdog policy.

## Provenance and pinned capabilities

- Producer source: `f19f6ab528ea991ec9c4c739a6a6247a42d82dd3`, `-Dclient=false`,
  `-j2`, Ubuntu 24.04 aarch64; existing `/home/bizarro/nostrc/_build` reused.
- Base: `docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8`.
- Runtime: rootless **Podman 4.9.3**, keep-ID, default seccomp. Podman reports
  AppArmor disabled; no AppArmor/SELinux confinement qualification. No Docker
  installed for this task. `Dockerfile.video` was built natively by Podman.
- Validated image: `4406917906ef57cbfc53c229d7ec8e9faf094563f63c1456c35de9520596d02e`,
  620,824,831 bytes. Local only; subsequently deleted. Mutable apt repositories
  and the throwaway producer are **not** a reproducible-build claim.

| Component | Installed version / runtime capability |
|---|---|
| Sway | `1.9-1build2` |
| wlroots | `0.17.1-2.1build1` (`libwlroots12t64`) |
| xdg-desktop-portal-wlr | `0.7.1-1build2` |
| xdg-desktop-portal | `1.18.4-1ubuntu2.24.04.3` |
| PipeWire + libspa-0.2-modules | `1.0.5-1ubuntu3.3` |
| WirePlumber | `0.4.17-1ubuntu4.1` |
| Mesa, render-node cell | `25.2.8-0ubuntu0.24.04.5`, EGL 1.5, OpenGL ES 3.0 |
| Actual GL renderer | `virgl (ANGLE (Apple, Apple M4 Max, OpenGL 4.1 Metal - 91.7))`, driver `virtio_gpu` |
| Output, both cells | `HEADLESS-1`, 1920×1080 @ 60000 mHz, scale 1, active |
| Portal frontend ScreenCast | version **5**, AvailableSourceTypes **1** (monitor), AvailableCursorModes **3** (hidden + embedded, no metadata cursor) |
| Wayland globals, both cells | `wl_shm` v1, `zwlr_screencopy_manager_v1` v3, `zxdg_output_manager_v1` v3 |
| Render-node-only additional global | `zwp_linux_dmabuf_v1` v4; **absent in pixman cell** |
| Input globals, advertised only | virtual keyboard v1, virtual pointer v2; **not exercised or accepted as metadesk input** |

`LIBGL_ALWAYS_SOFTWARE=1` and `WLR_RENDERER_ALLOW_SOFTWARE=1` were present in the
render-node run. They did **not** force a software renderer: the actual renderer
was virgl. Do not describe this cell as llvmpipe, GPU-free, or software-only.
The initial pixman cell is the software-rendering measurement; it failed capture.

## Frame-delivery and policy matrix

The driver sequence uses actual output redraws and validates center pixels
against two alternating backgrounds, five matches each. Geometry, CPU mapping,
bounds and pixel order are checked while the frame is held, then released.
A three-second per-change deadline is a **test failure bound**, not a portal
consent policy; metadesk's own 180-second Start timeout is unchanged. Times below
are from the first qualification run of the delivered image; subsequent repeat
and review-hardening results are recorded separately below.

| Cell | Portal result | Frame result / probe status |
|---|---|---|
| Headless + pixman, no devices, explicit allow | CreateSession/SelectSources succeed; Start returns rc=2 | **0 frames**, exit 6; `unable to receive a valid format from wlr_screencopy` |
| Headless + GLES2, only renderD128, fresh state | Start succeeds, node 37 | **10 frames**, 5+5 correct pixels, exit 0, 3.837 s including static wait |
| Same private services, new consumer, saved token | xdpw logs restoration; Start succeeds | **10 frames**, 5+5 matches, exit 0, 5.014 s |
| Unknown but syntactically valid UUID token | Backend reacquires through configured chooser policy | **10 frames**, 5+5 matches, exit 0, 4.942 s |
| Malformed non-UUID token | SelectSources InvalidArgument | **0 frames**, exit 6; start nevertheless returns 0 through direct-connect fallback |
| Token removed after malformed-token fallback, same private services | Start succeeds | **Intermittent FAIL**: CPU data NULL, exit 5; later repeat 10 matching frames, exit 0; md-atg |
| Render-node session, default deny chooser | SelectSources response rc=1 | **0 frames**, exit 6; same successful direct-connect fallback |
| Static output after each successful changing-pixel run | Stream remains connected, no new damage | **0 new frames**, 3 failed get_frame waits over 3.13–3.26 s |

Representative delivered frame: `1920x1080`, stride **7680**, **8,294,400 bytes**,
`MD_BUF_CPU`, `MD_PIX_CAPTURE_RGBX` (2). Pixel bytes alternate `204060ff` and
`a08020ff`, with increasing sequence numbers. All observed `timestamp_ns` values
were **0**; filed as **md-l0s**, not silently treated as valid timestamps.

### Repeats, corrections, and open gap

- An initial launcher attempt started xdpw before PipeWire was ready. The final
  launcher waits for a successful `pw-cli info 0` before starting the backend;
  final cells do not depend on D-Bus auto-activation rescuing a failed xdpw.
- An early probe expected repeated frames from an undamaged static background,
  and returned immediately many times before STREAMING. That was not a valid
  changing-frame test. The final probe paces immediate failures and continues
  redraws while waiting; it never counts elapsed time as success. Two fresh
  render-node runs before the final matrix also delivered all ten correct pixels.
- The post-malformed-token cleared-state run rejected a frame in an intermediate
  matrix, passed in the next repeat, then **failed again in the final image**
  with enhanced diagnostics: width 1920, height 1080, stride 7680, CPU type,
  RGBX format, 8,294,400 bytes, seq 0, **data pointer NULL**. The probe exits **5**
  before dereference. That aggregate gate correctly returned **1**, not green.
  After review hardening, the same delivered-image sequence passed once (fresh,
  reuse, stale-valid and cleared: ten matching frames each; aggregate **0**).
  This alternating result does not erase the confirmed NULL frame.
  **md-atg** tracks this confirmed invalid frame; buffer mapping/lifetime or
  fallback cleanup cause is not diagnosed. Do not claim reliable reconnect.
- In the preceding image (`72af02d7c1b4`, same pinned stack, before the additional
  backend-name readiness wait), a complete container/private-service restart with
  its own fresh runtime directory and the retained private home restored the
  token and delivered **11 frames**, 5+5 matches, exit 0, 3.828 s. This is one
  cross-service-restart success, not a remedy for the intermittent NULL frame.

## Consent and restore-token findings

`MD_VIDEO_CONSENT` defaults to **deny**, implemented by xdpw `chooser_type=simple`
and `chooser_cmd=/bin/false`. The operator must explicitly select
`allow-isolated-output` to install:

```ini
[screencast]
chooser_type=none
output_name=HEADLESS-1
max_fps=15
```

This is blanket operator authorization for clients of **this private session**
to capture its selected output, **not an interactive consent dialog** or a
per-client approval mechanism. Never apply it to the VM's GNOME session.
The pinned [xdpw configuration manual](https://raw.githubusercontent.com/emersion/xdg-desktop-portal-wlr/v0.7.1/xdg-desktop-portal-wlr.5.scd)
defines this no-interaction behavior. A stale valid UUID is therefore not a
fail-closed test: reacquisition is explicitly allowed by the chosen operator
policy. No generic unattended GNOME claim follows.

Metadesk sends uint32 `persist_mode=2`, requests monitor capture and embedded
cursor, and saves the returned token. In this cell it was **36 bytes**, mode
**0600**, under `$XDG_STATE_HOME/metadesk/screencast.token`; containing state
and metadesk directories were **0700**. The launcher sets umask 077 and creates
the parents. This does **not** establish atomic writes or hardening of existing
wrong-mode files: source still uses ordinary `fopen("w")`. No token values are
printed or committed. State belongs to one session/backend/identity; replicas
must not share it. Default run directories are disposable; any deliberately
retained state must also retain the portal backend's matching identity/state.

Malformed tokens are rejected promptly by the portal, but capture falls back to
a direct PipeWire connection and reports successful start without frames.
Denied consent behaves similarly. These observations and the legitimate static
idle case were attached to **md-ah9.10** (existing fail-closed/token/watchdog
owner); no duplicate policy bead or `src/` workaround was introduced. The
watchdog might mistake lack of damage for failure and delete authorization;
that is a follow-on risk, **not a watchdog runtime result from this experiment**.

## Why the software-only cell fails

The runtime failure is consistent with the pinned
[xdpw `start_screencast` implementation](https://raw.githubusercontent.com/emersion/xdg-desktop-portal-wlr/v0.7.1/src/screencast/screencast.c):
with screencopy v3 it rejects an invalid DMA-BUF format even when SHM is available.
Pixman advertises screencopy v3 but no linux-dmabuf global; adding only the render
node plus GLES2 supplies a compatible DMA-BUF path to xdpw. Metadesk still
negotiates **CPU RGBX**, not a metadesk DMA-BUF consumer. This is a pinned-stack
compatibility verdict, not proof that newer backends cannot capture SHM-only.
No backend patch, render-device permission change, card-node assignment, or
compositor upgrade was made to force the negative cell green.

## Bundle closure and least privilege

Follow `bundle-contract.md` and `throwaway-bundle.sh`, **not** a host-binary-only
copy. The matching Ubuntu build retained PipeWire/FFmpeg/x264 and their transitive
DT_NEEDED libraries despite `-Dclient=false`. The throwaway bundle was **138 MiB**;
`ldd metadesk-host` resolved **142** dependency entries with **no not found**.
Producer RPATHs were removed (all **140** staged ELFs checked with `readelf -d`,
no RPATH/RUNPATH) and bundle SHA256SUMS checked during image assembly.

The video image additionally installs PipeWire's **runtime modules and SPA
plugins**, WirePlumber's scripts/configuration, portal D-Bus services/backends,
Wayland tools, Sway and swaybg. These are not supplied by the Tier A ldd closure.
Module directories are the distro's `/usr/lib/aarch64-linux-gnu/pipewire-0.3`
and `/usr/lib/aarch64-linux-gnu/spa-0.2`; do not assume ELF closure proves dlopen
or GPU-driver closure. Actual renderer startup and capture are the runtime gate.
No encoder integration was run. 1920×1080 is already even; source dimension
rounding is not an odd-dimension encoder test. No x264/GPU encode throughput claim.

Final cells: `--init`, UID/GID 1000, keep-ID, read-only root, **CapEff=0**,
**NoNewPrivs=1**, private network with no published ports, no uinput/card/fuse
assignment. Pixman receives no devices. The optional GLES2 cell gets **only**
renderD128; the VM's existing user ACL supplies access. No new groups/ACLs,
capabilities, privileged mode, host networking, host IPC or host desktop mounts.

Each container creates its own mode-0700 runtime directory and dbus-run-session
bus, Sway socket, PipeWire socket, portal services and home/state. Tests bind only
task-owned empty runtime/home/evidence directories to preserve diagnostics; they
never mount `/run/user/1000`, the live session/AT-SPI buses, display or PipeWire.
The document portal emits a missing-FUSE warning; ScreenCast still functions.
RTKit/system-bus and optional libcamera warnings are not reasons to grant those
unrelated privileges. No accessible application/tree or input backend was started.

## Reproduce (throwaway, matching Ubuntu 24.04 aarch64)

Use an isolated source copy, never `~/metadesk`. Check `df -h /` first; budget
resources shared with other builds. The validated producer/consumer commands:

```sh
./deploy/containers/reference/throwaway-bundle.sh \
  "$PWD" /home/bizarro/nostrc "$PWD/deploy/containers/runtime-bundle"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Isrc \
  deploy/containers/reference/capture-probe.c -Lbuild-container -lmetadesk \
  -o capture-probe
podman build --jobs=2 -t localhost/metadesk-video:md-ah9-8 \
  -f deploy/containers/Dockerfile.video deploy/containers
python3 deploy/containers/reference/validate-video.py \
  localhost/metadesk-video:md-ah9-8 ./capture-probe ./video-evidence \
  --render-node /dev/dri/renderD128
```

The driver refuses existing evidence directories/container names and removes
only IDs recorded by its private `--cidfile`, even if another run wins a name
race. It records explicit probe statuses for expected negative cells; a successful
container process alone is not frame-delivery proof. The aggregate gate checks
all exercised container exits, zero-frame negative receipts, denial/malformed
failures and successful fresh/reuse/stale-valid/cleared capture;
without `--render-node` it records the software-only cell and returns nonzero.
Raw private evidence includes backend authorization state: **do not commit it**.
Delete evidence, bundle, executable, build directories and task images afterwards.

For a disposable capability-only run (no devices, capture denied by default):

```sh
podman run --rm --init --userns=keep-id --read-only --network=none --cap-drop=all \
  --security-opt=no-new-privileges --pids-limit=256 --memory=512m \
  --tmpfs /tmp:rw,nosuid,nodev,mode=1777 \
  --tmpfs /run:rw,nosuid,nodev,mode=1777 \
  --tmpfs /home:rw,nosuid,nodev,mode=1777 \
  localhost/metadesk-video:md-ah9-8
```

Podman 4.9.3 rejected tmpfs `uid=1000,gid=1000` mount options. Instead, private
`/run` and `/home` tmpfs parents permit the non-root launcher to create its own
0700 children. Do not relax the leaf runtime/state directory modes.

## Gates, cleanup, and handoff

- VM build with two jobs: PASS; only pre-existing compiler warnings.
- Meson tests with private nonexistent bus addresses, no live display:
  **23 passed / 0 failed / 0 skipped / 0 timeout**. Includes unit encode/decode,
  not portal-to-encoder integration or live-GNOME testing.
- Delivered-image matrix before review hardening: **exit 1**, NULL CPU frame on
  cleared-token reopen. Final driver rerun after review hardening: **exit 0**,
  fresh/reuse/stale-valid/cleared each ten matching frames; explicit software-only,
  deny and malformed-token negatives. **Intermittent reopen defect remains open.**
- Review fixes: cleanup uses a private CID file, not a container name; aggregate
  checks all exercised positive, denial/malformed zero-frame and lifecycle rows.
  A real Podman collision test forced a stale precheck against a task-owned
  sentinel container: driver exit 1, sentinel ID unchanged, then explicitly
  removed by the test owner. No other run's container was removed.
- Shell syntax, Python syntax, C `-Wall -Wextra -Werror`, whitespace: PASS.
- Input: **not tested**, md-ah9.9. Unattended policy: **not fixed**, md-ah9.10.
- New beads: **md-l0s** (zero capture timestamps), **md-atg** (NULL CPU frame on
  reopen). Both remain open; no production capture fixes are claimed.
- Substantive Oracle review used the seven actual files as attachments after
  stale selection transport omitted them. Both validator findings were fixed;
  no second substantive review or production-source change was made.

### Shutdown and live-session integrity

Without `--init`, `dbus-run-session` at PID 1 ignored default SIGTERM handling:
5.153 s stop, SIGKILL, exit **137**. All final examples/driver runs now require
`--init`. The delivered-image idle-consumer stop took **0.107 s**, exit **143**,
without SIGKILL; an earlier init-enabled check took 0.100 s. This checks process
termination, not graceful shutdown of the full metadesk host/watchdog.

Before/after cleanup, the live accessibility socket remained inode **2601**, mode
777, owned by bizarro; calculator PID **573511** retained its original start time
12:05:14. AT-SPI launcher/daemon/registry PIDs **573358/573371/573447** retained
original start times. Passive `ss -xnp` inspection confirmed Calculator fd19,
Unix inode **2457028**, still ESTABLISHED to the live AT-SPI daemon fd11, inode
**2462837**, at `/run/user/1000/at-spi/bus_0`. No live bus connection/query,
application action, GNOME restart, socket mount, or accessibility-tree dump was
performed by this task; registry contents were deliberately not re-queried.

### Final cleanup

Removed all eight diagnostic containers plus every driver-created container,
all image/cache IDs identified in this task's build receipts, the pulled base,
and `/home/bizarro/wt-ah9-8` (including bundle, executable, private state/tokens,
logs and build directories). No global image/system prune or other agent's files
were touched. Final rootless Podman accounting: **0 containers, 0 images,
0 local volumes, 0 B**. No Docker or host packages were installed by this task;
no extra local build directory was created.

Final VM root filesystem free: **9.2 GiB**, **9,841,889,280 bytes**, versus 7.9 GiB
at the initial check. This is a shared VM point-in-time reading, not a claim that
all free-space change was caused by this task.

Context triage used six Jev `find-lines` calls (14 model requests, 105,000 input
tokens, $0.004410 reported cost) to narrow plan/source/report reads; the Meson
judgment was partial and was not used as runtime proof. All capability and
capture conclusions above were checked against actual source or VM results.

Delivery is a **local-only branch commit**; no push, rebase, merge, publishing,
input implementation or sibling-source edit. md-ah9.8's reporting experiment is
complete; md-ah9.9/md-ah9.10 and the two filed capture bugs remain separate.
