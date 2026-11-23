<!-- validated on rootless Podman 4.9.3 / Ubuntu 24.04 aarch64, isolated Sway capture; 2026-11-17. -->
# Tier B isolated-video evidence — md-ah9.8 and md-ah9.10

The original md-ah9.8 sections below are **historical evidence for f19f6ab**.
The final md-ah9.10 section records the new implementation and its qualification;
old statements about fallback, timestamps and unchanged production code are not
claims about the current branch.

For the current PipeWire pin, version matrix and actual-host recovery evidence,
see [L10 qualification](#l10-newer-pipewire-and-real-host-recovery-2026-11-21).
The sections before that qualification are historical.

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
  -pthread -o capture-probe
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

## md-ah9.10 — portal policy and restore-token lifecycle

2026-11-17 local / 2026-11-18 UTC receipts. Base **323292d9d5b1717371a2061cb12c29c2d69e7207**;
all production changes are confined to capture.c/.h, capture_pipewire.c and the
contiguous host watchdog/CLI/config blocks. The reference probe/driver/launcher
are qualification tooling, not new supported capture executables.

### Operator policy and API

- Linux capture now **requires portal authorization**, including interactive
  mode. Denial, method error, malformed response, missing bus and timeout return
  `md_capture_start=-1`; none trigger a direct PipeWire fallback.
- `--capture-unattended` caps the **whole portal negotiation** at 10 seconds.
  `--capture-consent-timeout-ms MS` selects a shorter budget (1..180000 ms;
  unattended still caps at 10000). Interactive default is 180000 ms total.
  Authentication, Hello, AddMatch and method/Response waits share the deadline;
  there is no bus auto-launch/discovery or blocking flush. A single explicit
  local `unix:` session-bus address is required.
- `--capture-forget-on-stall` explicitly permits local token removal after
  exhausted **broken-stream** retries. It defaults off and is ignored in
  unattended mode. Explicit operator revocation remains available separately.
- `MD_CAPTURE_IDLE` (1) means connected PAUSED/STREAMING without a new damaged
  frame. `MD_CAPTURE_ERROR` (-1) means broken/stopped capture or a failed startup
  transition. Idle disarms the watchdog fault timer but does not erase retry
  history. A valid frame or client disconnect resets recovery. Core errors and
  an unexpected event-loop exit wake consumers as errors.
- Unattended host startup exits nonzero if capture authorization/start fails.
  This does **not** authorize a private output by itself: the operator must still
  choose the isolated xdpw `allow-isolated-output` policy. Unknown valid UUIDs
  still reacquire through that policy, not through retained authorization.
- Failure/expiry closes the request/session and the private D-Bus connection.
  Successful teardown waits up to 1 second for Session.Close acknowledgment;
  failed-start cleanup cannot extend its consent deadline. The pinned portal
  [completes Close after backend close](https://github.com/flatpak/xdg-desktop-portal/blob/1.18.4/src/session.c#L158-L212).
  This is a protocol boundary, not a sleep before reopening.

Example options for a host already launched **inside the private video session**:

```sh
metadesk-host --capture-unattended --capture-consent-timeout-ms 1500
```

### Token and frame contracts

The opaque file remains `$XDG_STATE_HOME/metadesk/screencast.token`, with HOME
fallback to `.local/state`, but never a shared `/tmp` fallback. The store creates
missing parents, enforces an owned non-symlink 0700 leaf and 0600 regular token,
rejects symlinks/oversized/truncated/NUL-containing reads, and tightens readable
legacy permissions. Invalid UTF-8 is rejected before building a D-Bus string;
its local file is retained. Error logs do not echo portal error detail/token bytes.

Replacement uses exclusive same-directory temp creation, complete checked writes,
file fsync, close, renameat and directory fsync. Pre-rename failure removes only
the temporary file and leaves old authorization intact. Tests exercise a forced
partial write with RLIMIT_FSIZE and concurrent readers during 100 replacements;
readers must see a complete old or new token. An already-open old inode with
`nlink=0` after concurrent rename remains readable. No power-loss/crash-recovery
qualification is claimed.

Delivered PipeWire frames now carry strictly progressing CLOCK_MONOTONIC
**receipt timestamps**, not a claim of compositor acquisition PTS. The probe
checks nonzero, increasing, nonfuture timestamps, including same-context reopen.
CPU descriptors are checked for supported type, non-NULL mapping, offset,
accessible extent, geometry and stride before publication. A second HAL guard
releases/rejects any NULL CPU frame returned by a backend. Pending borrowed
pointers are cleared on stop; stop blocks new acquisitions and waits for an
outstanding consumer release before destroying storage.

The pinned xdpw 0.7.1 intentionally
[sets data flags to zero](https://github.com/emersion/xdg-desktop-portal-wlr/blob/v0.7.1/src/screencast/pipewire_screencast.c#L312-L358),
including valid readable MemFd mappings. A review suggestion to require READABLE
unconditionally was tested and **rejected by real evidence**: it rejected all
otherwise valid frames. Zero flags remain compatible; explicit non-readable
flags and negative stride are rejected. Buffer dataType negotiation now uses a
flags choice, matching the producer's mask representation.

### Findings preserved from implementation runs

- The original backend, built from 323292d capture_pipewire.c with only metadata
  logging and preloaded into the private reference, reproduced **zero timestamps**
  on otherwise correct RGBX frames. Its fresh/reuse/stale/cleared matrix passed
  once; that repeat did not reproduce the old intermittent NULL frame.
- Instrumented hardened runs **did** reproduce invalid backend allocations:
  `type=4294967295`, flags=0, mapped=0, maxsize=0, chunk size=8294400,
  offset=0, stride=7680. PipeWire simultaneously logged `invalid memory type 4`.
  The guard returned no NULL CPU frame; the changing-frame gate correctly failed.
  A single passing run after the flags-choice correction did not establish a fix:
  another delivered-image run still failed its cleared-token cell (probe 6).
- The pinned [PipeWire invalid-type branch](https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-client-node/client-node.c#L799-L835)
  replaces an unsupported descriptor with SPA_ID_INVALID/NULL. This identifies
  the invalid descriptor before HAL publication; it is not evidence that the
  probe freed a valid mapping. Metadesk's confirmed violation was treating every
  non-DMA descriptor as a valid CPU frame. The deeper allocation/reuse trigger
  is not proven merely by adding a guard or observing subsequent passing runs.
- Source review found that dropping the D-Bus connection/issuing no-reply Close
  allowed teardown to remain asynchronous with the next consumer. Bounded
  Session.Close acknowledgment was added and is exercised below. It must not be
  described as a proven cure for every upstream allocation race.
- ASan caught a 50-byte libdbus container-builder leak on the new invalid-UTF-8
  path. Abandoning the open container before message unref fixed it; final gates
  use leak detection without suppressions.

### Pre-rebase delivered-image matrix and gates

Pre-rebase implementation commit: **f808e52**, replayed as **142bb28** on local
master **636904f** (including the md-ah9.9, md-66n and md-ah9.4 merges). The
pre-rebase private reference image was
**59945722e8b38c385332ff882ffd490b1ffcfdba032b47d44061f6332f3fc8d6**,
1,139,735,545 bytes; local only and removed after qualification. Sway, wlroots,
xdpw, portal, PipeWire and WirePlumber versions match the pinned table above.
The actual renderer was again **virgl (ANGLE / Apple M4 Max)** with only
`/dev/dri/renderD128`; this is not a software-only or DRI-free result.

Two complete fresh-container/private-service matrices on that final image each
returned **aggregate 0**, with no invalid-descriptor log or NULL CPU delivery:

| Cell, per matrix | Result |
|---|---|
| Pixman, no render device | Start -1; zero frames, probe 3; expected negative |
| Render-node fresh state | 10 correct changing-pixel frames; 5+5 matches; probe 0 |
| Saved-token reuse | 10 matching frames; probe 0 |
| Unknown valid UUID | 10 matching frames under explicit operator chooser policy; probe 0 |
| Malformed UUID | Start -1 in 0.006 / 0.005 s; zero frames, probe 3; malformed file retained |
| Cleared token after malformed attempt | 10 matching frames; probe 0 |
| One context, six start/stop cycles | 60 matching frames; probe 0; timestamps progress across restarts |
| Undamaged static output, after each successful cycle | Five IDLE results over approximately 5.1 s, zero ERRORs, token retained |
| Outstanding consumer mapping during concurrent stop | Mapping intact until release; get_frame after stop returns ERROR; ten checks per matrix |
| Kill only the private container's PipeWire after valid capture | 10 prior matching frames, then ERROR (-1); token retained; probe 0 |
| Default deny chooser | Start -1 in 0.008 s in both runs; zero frames, probe 3 |
| Stalled chooser, explicit 500 ms budget | Start -1 in **0.503 / 0.504 s**; zero frames, probe 3 |

That is **220 validated changing-pixel frames**, 12 same-context start/stop cycles
(10 true reopens after the initial starts), 20 outstanding-buffer teardown checks
and 110 static-idle results across the two
matrices. This remains local pinned-backend qualification, not proof that all
upstream allocation races or compositor combinations are eliminated. Earlier
failed matrices above remain failures; subsequent passes do not erase them.

**Additional real-backend ASan matrix: aggregate 1 (not a pass).** Using the same
private runtime image with the ASan-built library and ASan-built probe mounted
privately, fresh/reuse/stale each passed, six same-context cycles passed, and
private PipeWire loss returned ERROR with authorization retained. There were
100 correct timestamped changing frames and no ASan/LSan diagnostic. However,
cleared-token reopen returned start=0 in 0.017 s, then rejected eight descriptors
with the same SPA_ID_INVALID/NULL/maxsize=0 metadata and delivered **zero frames**
(probe 6). Denial, malformed-token retention, pixman and bounded chooser negatives
passed; the stalled chooser returned -1 in 0.501 s. This failure occurred **after**
bounded Session.Close was added: neither flags negotiation nor Close ordering is
claimed to have cured the deeper backend allocation failure.

**md-atg remains OPEN.** Its NULL-frame escape and stale-pointer hazards are
hardened in f808e52, but reliable cleared-state reopen/the deeper allocation
trigger is still unresolved. No duplicate follow-up bead was created; md-atg
retains that work and the detailed metadata. md-ah9.10 and md-l0s meet their
policy/token and timestamp acceptance respectively.

Quality gates for `-Dclient=false`, all compiled with at most two jobs per build:

- macOS full configured `meson test`: **25 passed**, 0 failed.
- Linux full configured suite, nonexistent desktop bus/runtime/display:
  **26 passed**, 0 failed, 0 skipped, 0 timeout.
- Linux capture tests with Meson `-Db_sanitize=address -Db_lundef=false`,
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`: **1/1 passed**, no ASan/LSan report.
  New tests cover actual Linux backend denial, malformed response and stream
  variants, a hung Start, silent bus authentication, corrupt UTF-8 retention,
  retry-cap/idle/timer-wrap logic, NULL CPU rejection/release and token lifecycle.
  Portal fixtures launch their own daemon and service; they assert which actual
  methods were reached, rather than treating any failure as test success.
- A Linux strace run of the capture tests also returned 0 and observed:
  exclusive 0600 temp open → fchmod0600 → complete write → file fsync → close →
  renameat to screencast.token → directory fsync. This is syscall execution
  evidence in addition to concurrent-reader and partial-write tests, not a claim
  of power-loss testing.
- Probe C `-Wall -Wextra -Werror -pthread`, shell syntax, Python AST and
  `git diff --check`: passed.

The full metadesk host/client transport and encoder were **not** run in this
matrix. The actual host watchdog state machine is shared with and tested by
`tests/test_capture.c`; the reference exercises its backend IDLE/ERROR inputs,
not a connected full-host watchdog session. No generic GNOME unattended claim,
input implementation, FIPS or accessibility qualification is added here.

One substantive Oracle review used actual worktree attachments after stale
selection transport returned the old baseline. The ownership, bounded setup,
parser, UTF-8, retry-history and loop-exit findings were addressed; no second
substantive review was run. Jev used three find-lines judgments (9 requests,
79,430 input tokens, $0.003336) to narrow the plan, host and historical-report
reads; those judgments were not treated as runtime evidence.

### Rebase and renewed qualification

The branch was rebased onto local master 636904f, whose code integration tip is
1d23cf7. Git applied the host changes without textual conflict; direct source
checks verified both input flags in usage/argument parsing, the new input_options
parameter and caller in host_run_mcp, and the separate capture-policy block.
No input or Meson implementation was edited by this task. A combined-options
`metadesk-host --help` check passed after rebuilding.

The interrupted run's uncommitted evidence was preserved in a task-only stash,
restored after rebase, and that stash was dropped. No unrelated stash was touched.
A fresh macOS Meson configuration was required to discover the new option.
The VM clock lagged copied source timestamps by roughly 25 minutes; only this
private checkout's future file mtimes were normalized to the guest clock. The
VM clock and live services were not changed.

Renewed gates on the rebased source:

- macOS, client=false, wayland_input=false: **25/25 passed**.
- Linux, client=false, **wayland_input=true**: **26/26 passed**, zero skips,
  failures or timeouts. The input tests include the merged private Wayland fixture;
  live-input/live-a11y environment flags were unset, and desktop bus/runtime/display
  addresses were unavailable.
- Linux ASan capture gate with wayland_input=true: **1/1 passed** (1.56 s),
  detect_leaks=1, halt_on_error=1, no suppressions or sanitizer diagnostics.

Rebuilt rebased image: **5478a00fec7d749fca98d67047d307115485783bff31076feb5967b330c87ac9**,
1,140,807,672 bytes, from 142bb28 with wayland_input=true. Both renewed real-backend
matrices correctly returned **aggregate 1**, not green:

| Rebased cell | Native probe/library | ASan probe/library |
|---|---|---|
| Fresh / reuse / unknown valid UUID | 10 matching timestamped frames each | 10 each |
| Malformed token | Start -1, 0 frames, file retained | Same |
| Cleared token | 10 matching frames | **0 frames; probe 6**, 8 invalid descriptors rejected |
| Six-cycle test | **Fails first start; probe 6**, 0 frames, 13 invalid descriptors rejected | 60 frames; all six start/stop cycles pass |
| Static undamaged output in successful cells | IDLE, no ERROR, token retained | Same |
| Concurrent stop with borrowed mapping, successful cells | Mapping intact until release; post-stop ERROR | Same |
| Kill only private PipeWire after positive capture | ERROR, token retained | Same |
| Pixman / deny chooser | Start -1; zero frames, expected negatives | Same |
| Stalled chooser, 500 ms budget | Start -1 in **0.504 s** | Start -1 in **0.504 s** |

There were **150 validated changing-pixel frames** in these renewed matrices
(50 native, 100 ASan). No NULL CPU frame escaped, no timestamp regression was
observed, and no ASan/LSan diagnostic occurred. The rejected mapping metadata and
PipeWire `invalid memory type 4` error match the pre-rebase failure. This is
continuing evidence for md-atg, **not** a successful reliable-reopen qualification
and not evidence that the unrelated master merges caused the defect. No retries
were used to hide the failed aggregate gates.

Final bead decisions: **md-ah9.10 CLOSED**, **md-l0s CLOSED**, **md-atg OPEN** with
partial safety/lifetime hardening and unresolved backend allocation/reuse work.
No new bead was filed because md-atg already owns the remaining investigation.

### Final cleanup and handoff

Committed the implementation and evidence locally, then removed the task's rebased
image and its pulled base by exact image IDs (all earlier task images had already
been removed). Driver CID cleanup left no containers, including external build
containers. Removed `/home/bizarro/wt-ah9-10` in full: both Linux builds, staged
bundle, native/ASan probes, baselines, private runtime/home/token state and all raw
evidence. Also removed the exact stale socket-test directory left by the earlier
timeout, the macOS build-ah9-10 directory, and task-owned temporary macOS logs.
No global prune, unrelated image/container deletion, shared nostrc build edit,
or live GNOME display/portal/PipeWire/bus access or restart was performed.

Final rootless Podman accounting: **0 containers, 0 images, 0 volumes; 0 B**.
Final VM free space: approximately **8.2 GiB**, **8,760,590,336 bytes**. All checked
free-space values stayed above the 3 GiB stop threshold; shared-VM space changes
are not attributed solely to this task. Task-only stashes were dropped, and the
feature worktree/branch are retained for review. No push or merge was performed.

## md-atg: allocation-publication investigation (2026-11-18)

Baseline: master **c571b5df580fc68b83637c103cfc9eb19837c564**, private checkout
`/home/bizarro/wt-atg`. Same pinned Sway 1.9 / wlroots 0.17.1 / xdpw 0.7.1 /
portal 1.18.4 / PipeWire 1.0.5 backend as above, with only renderD128. The runtime
image built from the unmodified Dockerfile.video was
**9ac1d1e646d1e20e2322462370de9786408c7be262f6b92208d292018532e299**,
621,661,951 bytes. All names/images/evidence use an `atg` prefix. No live GNOME
runtime directory, display, bus, portal or PipeWire connection was accessed.

**This is not a reliability fix or a passing after-fix matrix.** Changes in the
assigned capture code are opt-in diagnostics, not allocation workarounds. The
confirmed violation is in the PipeWire backend's allocation-publication path:
the daemon exports the producer's still-unallocated skeleton to the consumer
before applying the producer's allocation reply. Fixing that ordering is outside
the assigned files; md-atg must remain open until corrected and requalified.

### Instrumentation and corrected library override

`MD_CAPTURE_TRACE_BUFFERS=1` records a per-context start generation, stream state,
negotiated format/size/dataType mask and update result, and add/dequeue/acquire/
release/remove metadata. Each plane includes type, flags, FD, mapping pointer,
map offset, capacity, chunk offset/size/stride; add also identifies the FD's
backing file and size. Tracing is disabled by default and never owns, maps or
closes a producer FD. The probe additionally reports private `/proc/self/fd` and
SHM/memfd mapping counts before create, after each stop, and after destroy.

The validator now supports task-prefixed names, 1..100 cycles, explicit library
and ASan runtime mounts, separate upstream log-topic selection, input hashes,
and per-cycle changing-pixel receipt checks. Any rejected descriptor makes the
positive matrix fail even if later frames recover. `--skip-idle` is explicitly a
**burst diagnostic**, not static-idle qualification; pixel, timestamp, held-frame
and stop/release checks remain enabled.

The throwaway bundle **dereferences** its library symlinks. Therefore an override
must mount **libmetadesk.so.0**, the actual SONAME loaded by the probe/host, not
libmetadesk.so.0.1.0. The first run mistakenly overlaid only the latter and thus
exercised the image's pristine baseline library; it is identified separately
below. All subsequent overrides target the loaded SONAME. The baseline tracing
library SHA-256 was
`fb92ca3914dd175d84490f22b20ea004fd9f52df54220ffce4cdc63018f23c0a`.

### Before/diagnostic matrix — no behavior fix

| Run | Logging / idle mode | Reopen result | Aggregate |
|---|---|---|---|
| Pristine native image | Global upstream DEBUG; normal static checks | 20/20 starts pass; 250 matched changing-pixel frames across all positive cells | 0 |
| Native tracing library, correctly overlaid | Default upstream logging; normal static checks | 3 pass, fourth start fails; zero frames and 22 rejected descriptors | **1** |
| Native tracing library | Link/client-node topic DEBUG; burst | 100/100 starts pass; 1,050 matched frames across positive cells | 0 |
| Native + in-memory SPA server logger | No debug-output I/O before fault; burst | 5 pass, sixth start fails; 16 rejected descriptors | **1** |
| Native + in-memory SPA/core logger | All core DEBUG enabled into ring; burst | 100/100 starts pass; 1,050 matched frames | 0 |
| Native + selective in-memory SPA/core logger | Link/port topics only; burst | First start passes, second fails; causal ledger below | **1** |
| Native + selective ring with sync/resource topics added | Burst; added tracing masks failure again | 100/100 starts pass; 1,050 matched frames | 0 |
| ASan probe + ASan library | Default upstream logging; normal static checks | 6 pass, seventh start fails; zero frames, 13 rejected descriptors; no ASan/LSan report | **1** |

Fresh, saved-token reuse, unknown valid UUID, cleared-token, malformed-token
retention, private-PipeWire-loss and all expected-negative cells passed in each
of these matrices; the allocation failures above were in the repeated-start cell.
The pixman/no-device and deny cells failed closed with zero frames as expected;
the 500 ms stalled-chooser cell also failed closed. Higher-verbosity passes do
not erase default-logging failures and are not evidence of a cure. There is no
established token-specific trigger.

### Causal ledger and owner

Consumer-side add callbacks in the fourth-start failure already show both data
planes as `SPA_ID_INVALID` (4294967295), FD -1, NULL mapping and maxsize 0. The
first shared chunk already contains size 8,294,400 and stride 7,680; the second
chunk fills later. STREAMING and continuing nonempty chunks do not repair the
consumer's private descriptor copy. All bad frames remain contained by the
existing guard; none was published as a CPU frame.

A VM-only `LD_PRELOAD` diagnostic substituted an in-memory SPA logger for the
**private daemon only**. It also routed selected core link/port log topics into
the ring, dumping on the first invalid-type error and recording subsequent
allocation replies. It changed no buffer values, flags, calls or waits. Other
container processes passed through unchanged. The high-volume all-core variant
masked the failure; selective logging reproduced it. This experimental tracer
is not part of the runtime product. Its selective source is retained privately at
`~/Library/Caches/metadesk-md-atg-20261118/atg-pw-ring-selective.c` on the macOS
workstation, SHA-256
`daf8bacf08548f7665eb111271ead3ca11ac70f27a03252e3be3dceb14d105d6`.
It is available for source audit without retaining authorization state.

In the selective failure, one link (output node 37 port 0 to input node 39 port
0) and one server-local array were correlated throughout. Relative to its output
allocation dispatch:

| Event | Relative time | Same-link observation |
|---|---:|---|
| Producer allocation request | 0 us | Two skeletons; `SPA_NODE_BUFFERS_FLAG_ALLOC=1` |
| Output marked complete/paused | +142 us | Link resumes allocation before allocation reply is applied |
| Consumer export | +146 us | Same array, non-ALLOC input `use_buffers` |
| Conversion error | +148 us | Type **4** rejected and converted to SPA_ID_INVALID |
| Producer allocation reply applied | +816 us | Two concrete **SPA_DATA_MemFd=2** descriptors, valid FDs, 8,294,400-byte capacities |
| Link activation | Later | Consumer still has the earlier invalid copies |

These are traced-run timestamps, not a latency benchmark: dumping the ring after
an already-detected invalid export can delay processing of the later reply.
The premature export itself occurs before any fault-dump I/O.

There is no second overlapping link in this failed allocation. The earlier
successful allocation has the expected request → reply application → consumer
export ordering. The first independent ring run reproduced premature export on
another start; the selective trace additionally captures the late reply. This
establishes the allocation-publication violation, not merely temporal proximity
between logs from different processes. O, I and C were all in the **same TLS
ring**, hence the same daemon thread; the array was newly allocated for that
link, with no intervening clear/replacement. C's entry at client-node.c:1054
precedes per-plane logs at :1095, which occur **after** copying descriptor fields
into the output buffer at :1089. Both entry and post-copy logs are later than I.
Protocol sequence IDs were not recorded, so these receipts do not identify
which synchronization completion was wrong.

Pinned source explains the signature:

- [xdpw v0.7.1 producer allocation](https://github.com/emersion/xdg-desktop-portal-wlr/blob/v0.7.1/src/screencast/pipewire_screencast.c)
  receives a type **mask**, allocates SHM synchronously in add_buffer, then sets
  concrete type MemFd, FD and capacity. No allocation failure was logged.
- [PipeWire 1.0.5 client-node export/import](https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-client-node/client-node.c)
  rejects unsupported non-ALLOC types and makes the consumer copy invalid.
  Here **4 is the unconverted mask `1 << SPA_DATA_MemFd`**, not DMA-BUF type 3.
  A later allocation reply updates the server/producer descriptors, not the
  already-exported consumer copy.
- [PipeWire 1.0.5 link allocation](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/impl-link.c)
  allocates producer skeletons, waits via asynchronous synchronization, then
  reuses that array on the input. The observed completion precedes the reply.
  The exact synchronization defect still needs a corrective upstream patch.

Successful buffers are type 2 with mapped CPU memory; negotiated consumer mask
6 intersects xdpw mask 4 correctly. No DMA-BUF/modifier path was negotiated. The
100-cycle resource-enabled native burst returns to **10 FDs and zero SHM/memfd
mappings after every stop**, then to **3 FDs and zero mappings after destroy**,
matching the pre-create baseline. The ASan matrix used library SHA-256
`6a7d7a15d2785229a99ed615940dcc4e25ac16cf05e3676913a3359aee85ab2d`
and probe `d9190934a6810e7fff2ecc602adbaaae6b7560bc8d70bff183239f8769bbdefb`.
Add/remove and borrowed release precede teardown
in successful cycles. Those early probe versions record concurrent-stop attempts,
not proof that stop had reached its blocking boundary. Review strengthened the
probe to require ERROR while a mapping is held **and** atomic confirmation that
stop has not completed before release; the final validator requires that receipt
for every positive non-broken cycle. It also pins every cell to the originally
resolved image ID, retaining the supplied tag only as descriptive metadata. These observations argue against a consumer mapping/FD
lifetime leak; they do not claim general concurrency correctness for all backends.

### Reproduction and gates

After the unchanged throwaway bundle/image build described above, compile the
probe against the matching build. For a normal, correctly overlaid matrix:

```sh
python3 deploy/containers/reference/validate-video.py \
  localhost/atg-video:baseline ./capture-probe ./atg-evidence \
  --prefix atg-check --render-node /dev/dri/renderD128 --reopens 20 \
  --library "$PWD/build-container/libmetadesk.so.0.1.0" --trace-buffers
```

For burst diagnostics, add `--skip-idle --reopens 100`. Upstream verbosity can be
selected independently with `--pipewire-debug '2,pw.link:4,mod.client-node:4'`;
logging can mask this race. For ASan, compile the probe with `-fsanitize=address
-fno-omit-frame-pointer`, link the ASan build, and supply both
`--library "$PWD/build-asan/libmetadesk.so.0.1.0"` and
`--asan-runtime /usr/lib/aarch64-linux-gnu/libasan.so.8`. No sanitizer suppression
or automatic matrix retry is used. Private input hashes, summaries and receipts
are recorded in the evidence directory; raw tokens/state/logs must not be committed.

Final configured gates on the instrumentation changes: macOS **25/25**; Linux
with `client=false,wayland_input=true` **26/26**; ASan capture convenience API
**1/1**, `detect_leaks=1:halt_on_error=1`, no sanitizer report. Probe compilation
uses `-Wall -Wextra -Werror`. The real-backend reliability gate remains **failed**,
not skipped, despite the unit gates passing.

Jev was used for seven find-lines judgments (13 requests, 90,522 input tokens,
$0.003803): it narrowed capture negotiation/teardown and historical-evidence reads.
Its absent/partial validator/probe judgments were insufficient for extending the
harness, so those short files were read directly. No judgment was treated as
runtime proof.

### Reviewed harness requalification

After strengthening the held-frame check and pinning image execution, two new
normal-idle matrices used the final library and probe builds, default upstream
logging, opt-in buffer tracing, and **20 start/stop cycles each**:

| Final scoped-delivery cell | Native | ASan |
|---|---|---|
| Fresh / saved-token reuse | Pass | Pass |
| Unknown valid UUID | Pass | **Zero frames, probe 6; eight invalid descriptors** |
| Malformed UUID / retention / cleared token | Expected reject / retained / pass | Same |
| Repeated-start cell | **20/20 pass** | **20/20 pass** |
| Private PipeWire loss | ERROR, authorization retained | Same |
| Pixman / denied / stalled chooser | All expected negatives pass | Same |
| Aggregate | **0** | **1 — allocation reliability still fails** |

These final matrices validate **250 / 240 matched changing-pixel frames**
(native / ASan), respectively, from **257 / 247 observed frames**. There are
**24 / 23** strengthened outstanding-borrow stop receipts: ERROR observed while
held, stop not completed, pixels intact, then release and post-stop ERROR.
No ASan/LSan diagnostic or NULL CPU frame escape occurred. All 20-cycle resource
receipts return to 10 FDs/zero SHM mappings after stop and 3/zero after destroy.
Valid plane FDs resolve to unlinked `/dev/shm/xdpw-shm-*` files of 8,294,400 bytes;
`SPA_DATA_MemFd` is the transport type, not a claim that xdpw used memfd_create.

Final native library/probe SHA-256:
`a52c50ae362930ea2950b34a5ec6038d725249d0766656ce2912cf61e7f2376c` /
`24e2b913cf9b89fdf06b550ebc2da414ea9b0506ac18571a9e813486d2c20a72`.
Final ASan library/probe:
`6a7d7a15d2785229a99ed615940dcc4e25ac16cf05e3676913a3359aee85ab2d` /
`fb41b1c4ee8c6cc8097de7f6b8a6984dc4fe5d884d2ad41d1f044ce1276d053a`.
Image ID, inputs, summaries, causal server log, tracer source and unit-gate
summaries are retained in the mode-0700 private macOS cache directory above;
files are mode 0600. No restore-token/home/runtime directory is retained there.

One substantive Oracle review was completed using actual-file attachments after
stale selected-context transport omitted the diagnostic patch. Its held-frame
and mutable-tag findings were fixed and the matrices rerun; no second substantive
review was performed. macOS was rerun and remains 25/25; Linux remains 26/26 and
ASan capture 1/1. C probe strict compilation, Python AST and diff checks pass.

**Remaining work:** correct and qualify the backend synchronization/publication
path. No production behavior fix is included and no after-fix success is claimed.
A scope-expansion decision was requested before changing PipeWire/runtime
packaging. Existing **md-atg remains unresolved**; no duplicate bead was created.

### md-atg cleanup and handoff

Removed the task image by its exact recorded ID (including its unreferenced build
layers), and removed `/home/bizarro/wt-atg` with all builds, probes, tracer binaries,
bundle and private runtime/home/token/evidence directories. CID cleanup left zero
containers; no task volumes remain. The shared `ubuntu:24.04` base/tag was left
untouched. Final checked VM free space is **7.4 GiB**; all checked values stayed
above the 3 GiB stop threshold. No global prune, sibling code edit, live service
restart, push or merge was performed. The repository's pre-commit hook exported
shared Beads metadata to the main checkout's `.beads/issues.jsonl`; that
project-managed export was not staged in the task commit or reverted. The local
branch/worktree and small private macOS evidence cache are retained for review.

## md-awgm: PipeWire capture size cap (2026-11-19)

`MdCaptureConfig.max_width`/`max_height` (host `--capture-max-size WxH`) are now
supported by the PipeWire backend. These results come from the same pinned image
recipe (`localhost/awgm-video:base`, ID `2c09370f3a4f…`) with the task library
overlaid via `--library`. The final library, rebased onto `ad1fcf5`, is `65d1bb4f…` and the probe is `4c3be139…`.

### Negotiation vs consumer-side scale

- **Negotiation is not available on the reference.** A throwaway library offered
  `SPA_FORMAT_VIDEO_size` only as `1x1..1280x720`. xdpw 0.7.1 advertises its
  EnumFormat at the fixed output size (1920×1080), so the link failed:
  `paused -> error error=no more input formats`, zero frames, probe 6. This
  matches mutter, which also offers monitor streams at one fixed size. That
  experiment was never committed.
- **Shipped: a CPU box filter on the consumer side.**
  - `pw_get_frame` scales the acquired SHM frame into a backend-owned buffer
    (`md_capture_scale_frame`), outside the frame lock.
  - The PipeWire buffer stays borrowed until `release_frame`, so stop still waits
    for it (md-ah9.10). md-atg's validation and NULL-frame rejection run on the
    native buffer before any scale.
  - Only frames the consumer takes are scaled; dropped frames cost nothing.
  - `wait_size` publishes `md_capture_fit_size` of the negotiated size.
- **Why a box filter and not bilinear.** Each output pixel is the exact rounded
  mean of a box of whole source pixels, so uniform areas keep their value. The
  probe's background-pixel check stays exact. It is also cheaper. Micro-benchmark
  on the VM, `-O2`, single thread, 1920×1080 source:

| Target | Box (shipped) | Bilinear (baseline) | Full-frame memcpy |
|---|---|---|---|
| 1600×900 | 2.6–3.2 ms | 6.2–8.3 ms | 0.15 ms |
| 1280×720 | 1.9–3.0 ms | 4.0–5.9 ms | 0.15 ms |
| 1000×563 | 1.5–1.8 ms | 2.5–2.9 ms | 0.15 ms |
| 960×540 | 1.4–1.7 ms | 2.3–3.0 ms | 0.15 ms |
| 640×360 | 1.0–1.5 ms | 1.0–1.2 ms | 0.15 ms |

The ranges span four runs on the shared VM. Other agents' compiles were running
during the later runs, so absolute times are noisy. The box/bilinear ratio holds
in every run.

### Real-backend matrices (`--reopens 10`, normal idle checks)

| Cell | Uncapped (regression) | `--max-size 1280x0` |
|---|---|---|
| Published / delivered size | 1920×1080 / 1920×1080 | **1280×720 / 1280×720** (aspect 1.7778 kept) |
| Fresh, reuse, stale, cleared, 10 reopens, broken | all pass | all pass |
| Changing-pixel receipts (5,5 each) / frames observed | 15 / 152 | 15 / 152 (exact `204060`/`a08020`) |
| Outstanding-borrow stop receipts | 14 | 14 (the scaled pixels stay intact while held) |
| Pixman / denied / stalled chooser / malformed | expected negatives | expected negatives |
| Delivery latency p50 / p95 / max, 102 reopen frames | 0.05 / 0.12 / 1.2 ms | 2.2 / 4.2 / 11.7 ms |
| Delivery latency p50 / p95, fresh | 0.06 / 0.13 ms | 2.0 / 3.1 ms |
| Gate failures | **0** | **0** |

These runs use the final rebased library. The VM is shared, so the latency
tails vary between runs. Before the rebase, two earlier runs measured capped
reopen p50 2.0 ms and 2.5 ms, with p95 3.0 ms and 7.2 ms; the second was taken
during a concurrent compile. The Linux suite passed **36/36** both with
`--num-processes 2` and serially.

Single render cells with other caps also passed. Each matched pixels 5,5 and
passed the held-buffer receipt:
- `MD_CAPTURE_MAX_SIZE=1000x1000` published and delivered 1000×563 (aspect
  1.7762 against 1.7778 native, rounded per `md_capture_fit_size`). Latency
  p50 was 1.82 ms.
- `64x0` (the host's smallest cap) delivered 64×36. Latency p50 was 1.57 ms.

Latency is the time from `get_frame` returning back to the frame's receipt
timestamp, so for capped frames it includes the scale. The validator now
records it in `summary.json`. With `--max-size`, it requires one
`published_size` receipt per cycle equal to the probe's expected size.

Reproduce: run the existing commands, adding `--max-size 1280x0` to
`validate-video.py`. The probe reads `MD_CAPTURE_MAX_SIZE` and expects
`md_capture_fit_size(1920, 1080, cap)`.

## L10: newer PipeWire and real-host recovery (2026-11-21)

This section supersedes the historical reference-version and watchdog statements
above. Scope: isolated Ubuntu 24.04 aarch64, rootless Podman 4.9.3, the same pinned
Sway 1.9 / wlroots 0.17.1 / xdpw 0.7.1 / portal 1.18.4 / WirePlumber 0.4.17
stack, explicit `/dev/dri/renderD128` and virgl. No live GNOME, AT-SPI, shared
checkout, production relay or actual FIPS daemon was used.

### Runtime pin and support policy

**Current reference pin: PipeWire 1.4.10. Minimum supported PipeWire for this
reliable Tier B Linux capture reference: 1.2.7.** Both versions passed 20/20
full sequences after the flag-compatibility repair below. Ubuntu 24.04's
1.0.5 is a legacy, best-effort comparison: watchdog recovery works, but it is
not supported as an allocation-reliable reference. This is a support-policy
floor based on the tested stack, not a claim that 1.2.7 was the first fixed
upstream release or a qualification of every Linux compositor.

`reference/build-pipewire.sh` pins upstream **1.4.10**, commit
`ced36a5315135044a22c4b373be44368d312f9c8`. It now verifies that commit and
tree `bf76e6d580ede8a1e9f268182874f1f22974cb03` via Git (recorded in
`source.git`), rather than hashing the GitHub-generated archive used during the
qualification below.
`reference/Dockerfile.pipewire` overlays that complete userspace runtime on the
legacy image and replaces the bundle's old PipeWire client-library copy too.
The build checks both compiled and linked versions against its default 1.4.10
pin. It never installs anything or restarts services on the VM itself.
`Dockerfile.video` remains the historical 1.0.5 **comparison/base** recipe, not
by itself the newly qualified reference. Mutable apt repositories and an
external throwaway metadesk bundle still preclude a reproducible-build claim.

The 1.2.7 comparator used the same external Meson options, tag archive SHA-256
`e75568ed18bcbe75e9779af57cb9cc256fd7ebfaadc12bb347a0717055d1d3a9`.
The image deliberately retains the distro dpkg database: it still lists the
1.0.5 package. `pipewire-runtime.txt` records **actual compiled/linked runtime
versions, daemon/client-library hashes and daemon ldd**, rather than treating
dpkg's original package version as the active source-overlay version.

### A separate metadesk flag-validation defect

The first unmodified-library pilot on **each** newer runtime failed fresh
capture before the rest of its positive cells could run. Those failures were
not the allocation skeleton: concrete mapped `SPA_DATA_MemFd` buffers had
`maxsize=8294400`, `chunk.size=8294400`, stride 7680 and **flags=8**.

| Descriptor flags | Meaning and decision |
|---|---|
| 0, observed on valid 1.0.5 buffers | Access unspecified; retained legacy acceptance, subject to all mapping/type/bounds checks |
| 8, observed on 1.2.7 and 1.4.10 | `SPA_DATA_FLAG_MAPPABLE`, not a write-only permission; accept valid mapped descriptors |
| 4 | `DYNAMIC`, not an access permission; likewise does not imply write-only |
| 1 or 3, with optional 4/8 | `READABLE` or `READWRITE`; accept valid mapped descriptors |
| 2, 6, 10, 14 | Explicit `WRITABLE` without `READABLE`; still reject |

The old predicate rejected *any* nonzero flags lacking `READABLE`. The approved
fix rejects **only explicit write-only access**, leaving NULL, extent, stride,
type and DMA-BUF checks intact. Thus 1.0.5's flags=0 behavior is unchanged;
`SPA_ID_INVALID`/unmapped/maxsize=0 skeletons remain rejected on every runtime.
The newer server's backwards-compatibility path explicitly adds MAPPABLE to
MemFd buffers whose older producer omitted it; see upstream
[client-node.c, 1.2.7](https://github.com/PipeWire/pipewire/blob/1.2.7/src/modules/module-client-node/client-node.c)
and [the flag definitions](https://github.com/PipeWire/pipewire/blob/1.4.10/spa/include/spa/buffer/buffer.h).
This flag repair is **not** claimed as the fix for the original upstream
allocation-publication violation.

Once-per-start diagnostics now distinguish an unresolved allocation skeleton
from a 5-second first-frame deadline. Version text explicitly identifies the
**client library**, not an unqueried server version. No retry, sleep, deadline
increase, authorization bypass or unsafe descriptor publication was added to
production capture.

### Repeated full sequences (no retries-to-green)

| PipeWire | Full sequences | Valid pixel/held-buffer receipts | Reopen starts/passes | Rejected descriptors |
|---|---|---|---|---|
| 1.0.5, original master library | **10 pass, 10 fail / 20** | 191 / 171 | 102 / 98 (later cycles unexercised after failure) | **104**, all unresolved skeletons |
| 1.2.7, corrected library | **20 pass, 0 fail / 20** | 220 / 200 | **120 / 120** | **0** |
| 1.4.10, corrected library | **20 pass, 0 fail / 20** | 220 / 200 | **120 / 120** | **0** |

1.0.5 failed sequences: 1, 4, 5, 6, 8, 9, 12, 13, 15, 19. Fresh and saved-token
reuse passed 20/20; unknown UUID passed 19/20; cleared-token capture passed
14/20; the six-cycle cell completed 16/20 sequences; private-PipeWire loss
passed 20/20. All malformed/retention and other expected negatives passed.
The 1.2.7 and 1.4.10 batches observed **2,219** and **2,224** validated frames,
respectively; all expected negatives passed. All three batches used one
consistent image/probe/library identity per version across their 20 invocations.

Supplementary, not replacements for failures: original 1.0.5 pilot **1 pass**;
fixed-library, trace-enabled 1.0.5 matrices **3/3 pass** (logging can perturb
this race); final source-producer/default-pin image **1 full matrix pass**.
The two pre-fix newer-version pilots failed fresh capture on the distinct
MAPPABLE flag rejection described above and are not counted as ordering failures.

Every invocation uses the full four-cell driver, normal static-idle checks,
default upstream logging and **six** same-context start/stop reopens. It checks
fresh/reuse/unknown UUID/malformed UUID retention/clear/reopens/PipeWire loss,
changing pixels, monotonic timestamps, negotiated size, held-buffer lifetime,
pixman denial, explicit denial and stalled-chooser timeout. Failed invocations
remain failures; an early positive failure can leave later positive cells
unexercised. Twenty invocations are not twenty successful reopens on one context.

The original 1.0.5 batch used master `595b71d`'s library; newer-runtime batches
required the approved flag fix. Separate 1.0.5 runs with that same fixed library
are reported explicitly, rather than silently replacing baseline failures.
No individual upstream ordering-fix commit or earliest fixed release was
identified. Release/source inspection is not a causal bisect; successful
qualification establishes the tested version/stack, not every intervening or
future PipeWire version.

### Actual host recovery, separate from backend reliability

**Natural 1.0.5 run: 20/20 cleared-token host starts eventually streamed,
953 decoded frames.** Eighteen host starts were clean. Two started with **zero
acquired frames**, rejected 48 unresolved descriptors each, and recovered after
**one actual host watchdog restart each: 2/2 natural recoveries**. Sequence 11
then acquired 60 frames / decoded 54; sequence 17 acquired 66 / decoded 57.
All 20 retained authorization. These two faults take the existing immediate
invalid-descriptor `MD_CAPTURE_ERROR` path; they do not depend on waiting five
seconds for the new no-first-frame deadline.

**Do not call the complete recovery preparation matrix 20/20 green:** sequence
5's earlier unknown-UUID probe returned 6 (zero frames), although its subsequent
cleared-token host streamed 47 decoded frames. Thus the strict full-sequence
gate is **19 pass, 1 fail**. That batch ran before the aggregate preparation gate
was hardened: its original process exit was 0, but re-evaluation of the saved
receipts with the final gate fails it. The final driver now returns nonzero for
this condition; the failure was neither retried nor dropped.

**Separate injected no-first-frame run: 5/5 pass**, 329 decoded frames. Every
run suppressed 83–87 valid initial-stream buffers, acquired zero generation-1
frames, emitted the production **5-second deadline** diagnostic, restarted once,
and then acquired/decoded real generation-2 frames. Suppression lasted
9.007–9.014 seconds, matching the unchanged 5-second startup deadline plus
4-second host error watchdog. No rejected descriptor contaminated these runs;
authorization remained present. These five are **not** natural allocation-race
recoveries. A separate final-driver/native corrected-library smoke sequence
also passed, decoding 48 frames without a restart.

`validate-video-recovery.py` executes real preparatory captures on one private
backend, clears the token, and launches the **unmodified host** for the reopened
stream. The real decoder client negotiates NIP-17, consumes the one-use stream
binding, and must decode at least 30 frames. The relay/signers/FIPS-control
endpoint are fixtures; this is local TCP encode/decode proof, **not real FIPS
transport or a production-relay acceptance claim**. Trace receipts establish
whether any initial-generation buffer was acquired before a watchdog restart.
Preparatory failures are retained and fail the aggregate; they are not retried.

The fixture uses the existing NIP-01 live-only ephemeral mode for NIP-46 RPCs,
while retaining stored authorization/session history. Preliminary fixtures did
not enable this mode and could hit their 256-event bound; those exploratory or
interrupted runs are excluded from the qualified 20-run recovery batch, not
counted as PipeWire failures or passes. The failed setup of a simultaneous
injection run (overlapping fixture subnet) likewise exercised no capture.

The explicit `no-first-frame.c` interposer is **test-only** and is never bundled
or enabled by default. It withholds only complete mapped reference-output CPU
frames on the initial host stream, passes other descriptors through to actual
production validation, and becomes transparent after that stream is destroyed.
Injected rows require valid-drop receipts, the 5-second deadline, exactly one
initial-zero-frame watchdog restart and subsequent decoding. Any natural
rejected descriptor contaminates/fails the injection result. These injected
counts must never be added to natural allocation-fault recovery counts.

### Reproduce and quality gates

Build the legacy image/bundle and matching probe using the earlier commands,
but use the dedicated **read-only** `/home/bizarro/nostrc-master` dependency tree,
never the obsolete shared `~/nostrc`. Then, inside a new task-owned source copy:

```sh
./deploy/containers/reference/build-pipewire.sh \
  "$HOME/md-l10/pw-work" "$PWD/deploy/containers/reference/pipewire-runtime"
podman build --layers=false -t localhost/md-l10-video:1.4.10 \
  --build-arg REFERENCE_IMAGE="$(podman image inspect localhost/md-l10-video:1.0.5 --format '{{.Id}}')" \
  -f deploy/containers/reference/Dockerfile.pipewire deploy/containers
# Run this for each i=1..20, retaining every exit/summary, including failures.
python3 deploy/containers/reference/validate-video.py \
  localhost/md-l10-video:1.4.10 ./capture-probe "./video-$i" \
  --prefix "md-l10-video-$i" --render-node /dev/dri/renderD128 --reopens 6 \
  --library "$PWD/build-container/libmetadesk.so.0.1.0"
```

For recovery, prepare a **new** private fixture work directory and matching
host/client/signers-enabled image as in [client-validation.md](client-validation.md#reproduce)
(`client=true,signer_nip46=true,signer_nip5f=true`), containing `tests/`,
`mesh-peer.py`, `client-fixture.py`, `bin/nip5f-crypto`, `bin/nip46-bunker`:

```sh
python3 deploy/containers/reference/validate-video-recovery.py \
  --work "$HOME/md-l10/recovery" --prefix md-l10-recovery \
  --video-image localhost/md-l10-recovery:1.0.5 --probe ./capture-probe --sequences 20
# Separate, explicitly labelled fault-injection run, never a natural-fault count:
cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
  deploy/containers/reference/no-first-frame.c $(pkg-config --cflags libpipewire-0.3) \
  -ldl -pthread -o no-first-frame.so
# Supply --fault-injection "$PWD/no-first-frame.so" and a NEW --work/--prefix.
# Do not run two fixture networks with the same subnet simultaneously.
```

The production-descriptor seam does not duplicate the validator: it includes
`capture_pipewire.c` and substitutes only dequeue/queue. It exercises all 16
flag combinations, valid MemPtr, NULL/type/skeleton/size/offset/stride rejection,
publication versus requeue, and actual CPU-frame metadata:

```sh
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Isrc \
  deploy/containers/reference/test-pipewire-descriptors.c \
  $(pkg-config --cflags --libs libpipewire-0.3 dbus-1) \
  -Lbuild-container -lmetadesk -pthread -o test-pipewire-descriptors
LD_LIBRARY_PATH="$PWD/build-container" ./test-pipewire-descriptors
# Also compiled/run with -fsanitize=address,undefined -fno-omit-frame-pointer.
```

Final gates: Linux native **34/34**, no skips/timeouts; actual production
`on_process` seam **35/35 native and ASan+UBSan**, leak detection enabled, no
sanitizer report. Only the seam compilation was instrumented; this is not a
claim of a full sanitized runtime matrix. The seam compiled against pre-fix
source fails on flags=8 (exit 1), while the corrected source passes. Reference
Python tests **37/37**, including nine recovery receipt/gate cases; Linux also
ran the receipt tests. Both reference C probes compile with
`-Wall -Wextra -Werror`; the final source producer and version assertion were
actually executed, not just syntax-checked.

| Input | SHA-256 / image ID |
|---|---|
| Source baseline | `595b71d084f351c33a32d8d1b2a0d41e00ad3005` |
| Production repair + seam commit | `04884c8` |
| Probe | `4c3be1390b7dc3f8b9d371e3cbc1a1fc66a82c466c831b99dbafe45656a04ca3` |
| Original matrix library | `35255327dfcdd10fbfe89537d956a16782a400a438eefbaeb28d162c76ee784f` |
| Corrected matrix library | `4c53ee1e8259de6b7bd4c80b08a72650c694f686349b3392d06c04cd1f4f6b15` |
| 1.0.5 image | `5728a4733e09734ba6e717f94b0754755788aa83c4ab41116c69728d1d724ba2` |
| 1.2.7 image | `ef6f0b2b71a5eba9756f2adf52246cfdb6afc63d81e233417bce192adaa62a07` |
| 1.4.10 image, 20 sequences | `e133add3b7cccd79d1d48973ddb4b5f19ead7978c888fee8cd0e450d00aa9a9c` |
| Final pinned-producer image, extra matrix | `68eb8289126cef52f81386d8a4a5d300cef0c44a2633fae0f785d621a74403de` |
| Natural-recovery host/client image | `80b7d0e7dfdd4d8215a52bae71b7107a1f8604fa40f834ae09caff311cf452b8` |
| Injected/final-smoke host/client image | `a8b22d362c9091be4251ea757ce3e78b912294dc33b59f7fbe157cfb57ccc8e8` |

The actual installed libraries in the two recovery images hash to
`3b87addb73652b366014281474cd20ed19371f31a3565e6328bdbc94c97ccbe2`
and `85a96e25d47738c67727d7554429cca89448ac72cfdc3f4f8f4d966cd6c13b78`,
respectively (producer RPATHs stripped). The former is the unmodified master
host/library with md-3nv; the latter adds this lane's approved flag repair and
diagnostics, not a host watchdog change. Preliminary recovery experiments:
one pilot streamed; an exploratory six-attempt batch streamed four before
fixture startup/session failures; an interrupted five-attempt batch streamed
four. These are preserved separately, not merged into the final 20 attempts.

Diff-scoped Nostr Protocol Review (A–J): no new relay polling, timeout-driven
subscription closes, manual relay frames or ignored protocol acknowledgements
were introduced. Existing subscription/OK/EOSE-aware session logic is reused;
the color redraw interval is a visual test stimulus, not a Nostr delivery timer.
Oracle review found three false-green/injection risks; preparation failures now
fail the gate, injected runs require explicit path receipts, and invalid
buffers remain visible to production validation. Nine classifier/gate tests
cover these distinctions. Oracle's first artifact-based request had no usable
context because worktree artifact aliases were rejected by the tab; the actual
implementation was supplied inline for the completed review.

Cleanup verified: all six task images/layers, task containers, fixture networks,
volumes, build containers and the complete `~/md-l10` tree were removed. No
task processes remained. Only the pre-existing Ubuntu image/default Podman
network remained; the shared nostrc/metadesk trees and live GNOME/AT-SPI session
were untouched. VM free disk space after cleanup was recorded in the private
cleanup receipt. Sanitized counts, hashes, selected recovery receipts and test
results remain under macOS `~/Library/Caches/metadesk-md-l10-20261121`; no keys,
tokens, home/runtime state or raw backend/relay logs were retained there.

**Closure recommendation: md-atg can close after the L10 commits land with the
1.4.10 reference pin and the documented 1.2.7 support floor.** This remedies the
supported reference by replacing the faulty runtime and removes the separate
metadesk compatibility rejection. It does not repair upstream 1.0.5 or certify
all Linux desktops. The old-runtime failures and the 19/20 complete recovery
preparation gate remain failures, even though all 20 cleared-token host starts
eventually streamed. No bead was closed, no push or merge performed.

Jev triage: eight find-lines judgments (24 requests, 190,668 input tokens,
approximately $0.00801) narrowed source/history reads; no model judgment was
used as runtime or cleanup evidence.
