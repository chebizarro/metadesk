<!-- validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture + live GNOME + accessibility-loss restart recovery below; other cells unvalidated. -->
# md-ah9.4 / md-ah9.16 / md-5zi Tier A container results — 2026-11-17

## Desktop accessibility-loss restart recovery — md-ah9.4

This completes the last open Tier A cell on source master `323292d`, with
**deploy-only** changes. Scope is the explicitly authorized live accessibility
bus loss, **not logout or replacement of the session bus**. A changed session
socket inode still requires container recreation. Earlier pending md-ah9.15
statements below are historical and superseded by this section.

### Fix and recovery matrix

The entrypoint no longer requires/pins `AT_SPI_BUS_ADDRESS` from `up` time.
On every start and preparation retry, `probe.py` calls `org.a11y.Bus.GetAddress`
on the mounted session bus, validates both filesystem sockets and their desktop
UID ownership, then requires registered applications. The entrypoint exports
that validated address to the host. The entire preparation remains bounded at
30 s; PID 1 still handles TERM. Health probes independently rediscover the bus,
since container-exec environments do not inherit PID 1's exported address.
Compose mounts the **whole AT-SPI directory** at its original absolute path.

One controlled SIGTERM of launcher, accessibility.conf daemon and registry was
used for the measured cell in each runtime. Docker had one additional initial
restart: a test assertion incorrectly required the socket *basename* to change;
it was corrected to require the full address to change. Total: **two Docker,
one Podman**. The session bus, shell and desktop session were never terminated.
Old calculators were terminated and relaunched on `wayland-0` to re-register.
No sibling containers, images, build trees or live input were touched.

Times start immediately before SIGTERM of those three AT-SPI processes. Host
exit uses a Linux pidfd for container PID 1 (not the later runtime die event).
Recovery ends at successful authenticated MCP initialize/tools/list/tree read,
asserting Calculator window, buttons **0/7/+/=**, two display text nodes and
absence of input/capture tools. Missing bearer still returns 401.

| Recovery cell | Docker 29.1.3 / Compose 2.40.3 | Rootless Podman 4.9.3 / user systemd |
|---|---|---|
| Host exits nonzero after desktop loss | **PASS: 0.680 s**, exit 1 | **PASS: 0.641 s**, exit 1 |
| Automatic restart owner / backoff | Compose `on-failure`, one engine restart; die → start **0.259 s** (engine initial 100 ms backoff plus runtime overhead) | Single systemd owner, `Restart=on-failure`, `RestartSec=5s`, burst 5/300 s; one restart; systemd failure → next start **5.093 s**, container died → start **5.266 s** |
| GetAddress returns new bus | **0.721 s** after loss | **0.671 s** after loss |
| New-address authenticated widget tree | **PASS: 3.324 s** after loss | **PASS: 6.626 s** after loss |
| New full address in recovered PID 1 environment; dynamic HTTP health | PASS | PASS, even with deliberately stale `/stale/old-bus` container env |

Observed socket path stayed `/run/user/1000/at-spi/bus_0`; the socket was
replaced and GUID changed on each restart. **Live basename-change recovery is
not claimed**: `bus` → `bus_0` discovery is covered by focused unit tests using
real Unix sockets and mocked GetAddress replies. No path was hardcoded to make
this cell pass. New full addresses (GUIDs):

- Docker: `1290ea87c2e39c0275512adc6afd38b7` →
  `7758dddea2d84264f38b5dec6afd38ee`.
- Podman: `7758dddea2d84264f38b5dec6afd38ee` →
  `7681f49ef99e5cb711fb70b56afd3911`.

The Docker cell used the supplied Compose manifest (only a task-specific
container-name override), read-only root, no caps/devices/published ports,
explicit AppArmor profile and Docker-only IPv6 sysctl. Podman used the README's
keep-ID/read-only CLI wrapped in transient user systemd, **not** native Podman
restart or a claim about Compose backoff. Rootless AppArmor remains disabled.
The updated rootless Compose compatibility-API healthy/tree cell also passed;
its restart behaviour is not the systemd recovery result above.

### Build and regression receipts

Same Ubuntu 24.04 aarch64 VM/dependencies and pinned base as md-ah9.16 below.
Bundle built with `throwaway-bundle.sh`, `-j2`, in `~/wt-ah9-4r`.
Docker built image `sha256:5d8fc733d268e709b5e12068eb3ea7eb0890e34ccf5bbb29de44d4246bc13110`;
save → rootless load image
`7777acd3eaba5d76bfb4406103fd75aa9b0d2f34f8b673495fb99bc9c56fdf2d`.
No Podman-native build claim. In-image closure check passed. After the review
hardening (address syntax only), images were rebuilt and both live validation
suites rerun without further bus disruption: Docker
`sha256:2b130254b733502546a87975049b531998919d9660afbee1fc84fb023ee0a5cd`,
rootless loaded
`c2a051ca6725dbd06ec1ac7a212d00daf3fc082987f1ee238ee191084cfc8de5`.
Recovery timings above belong to the first image pair, not a second disruptive
qualification on the hardened pair.

- `meson test -C build-container --print-errorlogs`: **26 OK / 0 fail / 0 skip**.
- Python reference unit tests: **12 passed** on macOS and the VM, including
  dynamic discovery, stale env, socket owner/type, invalid replies and empty tree.
  Review identified address-list ambiguity: validation now rejects `;` and `%`
  so D-Bus cannot parse a different endpoint from the literal socket checked by
  stat. Regression uses real sockets with separator/escape characters in names.
  The review was completed with that straightforward fix applied.
- `validate.py docker --live` and `validate.py podman --live`: HTTP + stdio real
  Calculator widgets, identity/session/secret/no-desktop negatives, closure,
  no-input/capture/device/7700 checks and graceful termination passed.
- Preparation TERM now omits only the container's AT-SPI directory mount;
  no test alias is written into the live runtime directory. Exit 143 in
  **0.113 s Docker / 0.101 s Podman** in the first full updated-driver run.

### Final desktop and cleanup

- GetAddress succeeds on `/run/user/1000/bus` and returns
  `unix:path=/run/user/1000/at-spi/bus_0,guid=7681f49ef99e5cb711fb70b56afd3911`.
  Launcher/daemon/registry PIDs: **700705 / 700720 / 700785**.
- Exactly **one** calculator process and **one** registered accessible app:
  **gnome-calculator PID 743316**. D-Bus GetConnectionUnixProcessID agrees;
  its **Calculator** window has AT-SPI **VISIBLE** state. It was relaunched from
  `/home/bizarro` at cleanup so it does not retain the deleted task cwd.
  Shell/session bus stayed up; shell is still absent from the restarted registry.
- Docker before purge: **0 images / 0 containers / 0 volumes / 0 build cache**.
  Purged all seven newly installed packages: docker.io, docker-compose-v2,
  containerd, runc, bridge-utils, pigz, ubuntu-fan. Installed-package additions
  remaining **0**, baseline installed packages lost **0**. Docker binary, group,
  data/runtime directories, bridge and task-added Docker firewall/AppArmor
  state removed; pre-existing non-Docker firewall rules preserved.
- Removed only the two task-loaded rootless images; task containers/networks
  and transient systemd/API/event processes are gone. No Podman-native build
  cache or task volumes were created. Sibling input/video images and their
  Ubuntu base were preserved; no global Podman prune was run.
- `~/wt-ah9-4r` (bundle, build, token, logs, helper) removed. Final available
  root disk: **7,763,099,648 bytes (7.23 GiB)**. Lowest observed free disk was
  **6.2 GiB**, safely above the 3 GiB stop floor; concurrent sibling disk usage
  explains why the final value is lower than the initial 9.1 GiB.
- No new beads. md-ah9.4 is complete for the authorized accessibility-loss
  cell; full session-bus replacement, native Podman Compose backoff and hosted
  CI remain outside this qualification, not implied passes. Local commit only;
  no push or merge.

## Live GNOME requalification — md-ah9.16 (historical)

This section supersedes **only the missing-socket live row** in the historical
fixture matrix below. Evidence is local Ubuntu 24.04 aarch64 VM execution, not
hosted CI, release certification, video/client/input/capture acceptance, or
logout/recovery qualification. No `src/` changes. Producer source:
`92ab2cf2e37030f0ddb32df2b9d933fdb410be94` (fresh master checkout); runtime
artifacts include the deploy-only changes in this task.

The operator restored the desktop AT-SPI bus before this task. GetAddress on
session bus `/run/user/1000/bus` returned
`unix:path=/run/user/1000/at-spi/bus_0,guid=e6aa21521600f69c1761246e6afcb360`.
The socket was discovered, **not hardcoded**. Mounting its containing directory
at the same absolute path already works for `bus_0`; no entrypoint, probe or
mount-path logic fix was necessary. The live registry had **one application**:
`gnome-calculator` (PID **573511**). **gnome-shell is absent from the tree because
it predates the bus restart and did not re-register; a fresh login would add it.**
No logout/login was attempted. Launcher/daemon/registry PIDs
**573358 / 573371 / 573447**, and gnome-shell PID **300603**, were left running.

### Versions and image provenance

- Ubuntu 24.04, kernel `7.0.0-42-generic`, UID/GID 1000:1000, GNOME Wayland.
  AT-SPI `2.52.0-1build1`, GTK `3.24.41-4ubuntu1.3`,
  GLib `2.80.0-6ubuntu3.10`, gnome-shell `46.0-0ubuntu6~24.04.17`,
  gnome-calculator `1:46.3-0ubuntu0.1`; existing `/home/bizarro/nostrc/_build`,
  not rebuilt.
- Docker **29.1.3** (`29.1.3-0ubuntu3~24.04.2`), rootful default seccomp,
  explicit `metadesk-agent-ah9-4` AppArmor profile; Compose **2.40.3**
  (`2.40.3+ds1-0ubuntu1~24.04.1`). containerd `2.2.1-0ubuntu1~24.04.4`,
  runc `1.3.4-0ubuntu1~24.04.1` (temporary Docker installation).
- Rootless Podman **4.9.3**, keep-ID, seccomp enabled, **AppArmor disabled**
  in Podman host security info. Host AppArmor package
  `4.0.1really4.0.1-0ubuntu0.24.04.9`; active LSMs:
  `lockdown,capability,landlock,yama,apparmor,bpf,ima,evm`.
  No SELinux or confined desktop-peer qualification.
- Bundle rebuilt via `throwaway-bundle.sh` with two jobs in `~/wt-ah9-16`.
  Same pinned Ubuntu base:
  `ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8`.
- Exercised Docker image:
  `sha256:0339de9b3548f540430c4666e1021cd5564ce79730e09351346f5b2049ef739f`.
  Docker save → rootless Podman load, Podman image:
  `6d36787932c3e1c0c393746e670a5b2d1ffe4b3f8466adad98b6c506cb018661`.
  The existing local tag `metadesk-agent:ah9-4` is retained by the reference.
  Documentation/comment-only edits after assembly do not change these receipts.

### Live matrix

Every tree cell includes initialize → initialized → tools/list → resources/read
of `metadesk://ui-tree`. Assertions require the Calculator window, buttons
**0 / 7 / + / =**, and **two display text nodes**, not just an application name.
The untouched calculator display is empty (`TXT[...] ''`); no typing/clicking or
input access was enabled to populate it. Both transports returned the same real
widgets, including additional buttons `mod`, `÷`, `×`, `Undo`, and `Main Menu`.

| Live GNOME cell | Docker 29.1.3 | Rootless Podman 4.9.3 |
|---|---|---|
| HTTP bearer, bind 127.0.0.1:7710, calculator widget tree | PASS | PASS |
| Missing bearer on /mcp | PASS, 401 | PASS, 401 |
| Stdio calculator widget tree | PASS | PASS |
| HTTP SIGTERM, exit 0, <5 s | PASS, **1.030 s** | PASS, **1.098 s** |
| Stdio child SIGTERM, exit 0, <5 s | PASS, **0.130 s** | PASS, **0.131 s** |
| No 7700; nine input/capture tools absent; no input/DRI/PipeWire access | PASS | PASS |
| Read-only root, zero effective caps, no-new-privileges, no assigned devices, private netns/no published ports | PASS | PASS |
| Compose healthy → authenticated widget tree → stop exit 0, <5 s | PASS, **1.00 s** | PASS, **1.08 s**, compatibility API + override |
| Wrong identity / missing session socket / missing secret / direct no-desktop host | PASS, nonzero diagnostics | PASS, nonzero diagnostics |
| DT_NEEDED closure | PASS, 144 resolved entries | PASS, 144 resolved entries |
| Desktop accessibility loss → recovery | **PASS; see md-ah9.4 recovery matrix above** | **PASS; see md-ah9.4 recovery matrix above** |

Docker's namespace IPv6-disabled sysctl was also checked. Its listener appears
in `/proc/net/tcp6` as **IPv4-mapped 127.0.0.1**, not a wildcard/IPv6 bind; actual
IPv4 dial succeeded. Rootless IPv6-disabled is not claimed. The tighter listener
assertion initially assumed a `/proc/net/tcp` entry and failed; it now accepts
only IPv4 loopback or its exact IPv4-mapped representation. This was a test
assumption fix, not a host networking workaround.

Rootless Compose used Docker Compose 2.40.3 against a **task-owned rootless
Podman API socket**, not the Docker socket or `podman-compose`. New
`compose.agent.podman.yaml` replaces Docker-specific AppArmor/sysctls and tmpfs
options, preserving keep-ID and no-new-privileges. No SELinux relabeling or
`U` on the desktop directory. The initial API client invocation used malformed
`DOCKER_HOST=unix:...`; changing it to `unix://...` fixed that diagnostic.
No native Podman build/provider or general Compose compatibility claim.

### Reproduction and safety

Use `~/wt-ah9-16`, never `~/metadesk`. Follow the bundle contract and README to
build/load the pinned image and create the mode-0600 bearer file outside the
image context, then run from the task root:

```sh
python3 deploy/containers/reference/validate.py docker --live --dump-tree
python3 deploy/containers/reference/validate.py podman --live --dump-tree
```

`--dump-tree` intentionally prints real desktop text: opt in only for a known
nonprivate target. Live mode verifies the socket still exists and calculator
remains in the registry after every container run, HTTP smoke/shutdown and
cleanup (including failure paths). It skips the fixture's preparation-alias
simulation entirely; **nothing is written into the live AT-SPI directory**.
No private fixture was needed; no app was launched or closed by this task.

Compose used `-p ah9-16`, `up -d --no-build --wait --wait-timeout 45`,
`exec -T agent python3 /opt/metadesk/reference/smoke_mcp.py --http --expect
gnome-calculator --calculator-widgets --dump-tree`, `stop`, and `down --volumes`.
Docker used `compose.agent.yaml`; rootless used both it and
`compose.agent.podman.yaml` with the README's task-owned API service. Live guard
checks also ran after Compose startup, tree smoke, shutdown/removal and cleanup.

### Gates, timing receipts and cleanup

Bundle manifest SHA256:
`67956bf2475c21804a8d5947bb84fb7d7064f25f974c5f4df6108ec1e18572f8`.
In-image `sha256sum -c SHA256SUMS` passed on both runtimes. All **140 producer
ELFs** lacked RPATH/RUNPATH. `meson test -C build-container --print-errorlogs`:
**22 passed / 0 failed / 0 skipped / 0 timeout**. Shell syntax, Python in-memory
compilation, `git diff --check`, and both Compose config paths passed.

Review corrections scope widget checks to the **calculator window descendants**
(another app/window cannot supply them), use per-invocation UUID container names
to avoid cleanup collisions, and keep missing-widget diagnostics free of desktop
text unless `--dump-tree` is requested. Five synthetic smoke-gate regression
tests passed on macOS and the VM; they are **not live-desktop evidence**. Run:
`python3 -B deploy/containers/reference/test_smoke_mcp.py`. Both live drivers and
both Compose paths were rerun with the reviewed smoke image. Jev triage used
one `find-lines` query for entrypoint readiness/socket checks (presence 0.94;
1,450 input tokens; $0.000061); no absence judgment gated qualification.

Final reviewed-image timings are in the live matrix above. No five-second
SIGKILL fallback was used. Docker was initially absent and rootless Podman's
initial container/image/volume inventory was empty; cleanup restored that state:

- Docker `system df`: **Images 0 / Containers 0 / Volumes 0 / Build Cache 0**, all
  **0 B**. Rootless Podman `system df`: **Images 0 / Containers 0 / Volumes 0**,
  all **0 B**. No Podman-native build cache was created.
- Both Compose task networks removed. Only Podman's pre-existing built-in
  `podman` network remained. Docker's bridge and its IPv4/IPv6 firewall chains
  and rules were removed specifically (11 IPv4 rules / 7 chains, 7 IPv6 rules /
  7 chains); unrelated firewall rules were preserved.
- Purged all **seven newly installed packages**: `docker.io`,
  `docker-compose-v2`, `containerd`, `runc`, `bridge-utils`, `pigz`, `ubuntu-fan`.
  Docker command, empty newly created `docker` group, `/var/lib/docker`,
  `/var/lib/containerd`, runtime sockets/data and `/etc/docker` absent. No user
  group membership was added. Task profile and build-created `docker-default`
  AppArmor profile removed.
- Task-owned rootless API service stopped; `podman system migrate` stopped the
  pause helper after final inventory. The API left two task-only exec `conmon`
  helpers with `--exit-delay 300`; their exact container ID/name was verified
  before SIGTERM, and pidfd waits confirmed termination. Final process scan:
  **no Docker/containerd/metadesk/conmon/rootless/API/pause helpers**.
- Deleted `~/wt-ah9-16`, build/destdir, bundle, API socket, bearer file and logs.
  No fixture or ordinary application was launched. Calculator was left running.
- Final GetAddress socket **`/run/user/1000/at-spi/bus_0` exists**, calculator
  remains the **one registered application**, and gnome-shell/launcher/daemon/
  registry/calculator PIDs **300603 / 573358 / 573371 / 573447 / 573511** are
  unchanged. No user desktop service was killed, restarted or reconfigured.
- Final `df -h /`: **62 G total / 50 G used / 9.2 G available / 85%**;
  available-byte receipt: **9,869,406,208** (minimum observed: 7.8 G).

**md-ah9.16 live acceptance passed.** The only remaining md-ah9.4 acceptance item
is **logout/relogin recovery via md-ah9.15**. It was deliberately not exercised
against the live session. No new beads or product-code fixes were needed.
Delivery is a local branch commit only: **no push or merge**.

## Historical fixture qualification — md-ah9.4

### Scope and versions

Local VM evidence, not hosted CI, release certification, video/client acceptance,
or a production-GNOME attachment pass. Source: metadesk **8157ac0**; only deploy
artifacts changed. Ubuntu 24.04 aarch64, kernel `7.0.0-42-generic`, UID 1000,
GNOME Wayland seat0. AT-SPI `2.52.0-1build1`, GTK `3.24.41-4ubuntu1.3`, GLib
`2.80.0-6ubuntu3.10`. Prebuilt nostrc: `/home/bizarro/nostrc/_build`; no claim of
rebuilding or independently qualifying that checkout.

- Docker **29.1.3**, package `29.1.3-0ubuntu3~24.04.2`, rootful, default seccomp,
  explicit `metadesk-agent-ah9-4` AppArmor profile; Compose **2.40.3**.
- Rootless Podman **4.9.3**, keep-ID, AppArmor disabled (`podman info`); SELinux
  unvalidated. Docker tmpfs `uid=/gid=` rejected; private tmpfs `U` validated.
- Base: `ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8`.
- Tested Docker image ID: `sha256:81bd1b729b89b67d30027bc05a3046156ce45e16b4c0db4d447e003d2884786a`.
  Docker save → rootless Podman load; Podman ID:
  `07999d59ceca4e51be9d6c06466c50abd0fd4b4cd90da5bb28f3802cc424caf4`.
  Headers/docs may change after assembly; these IDs identify the exercised image.

### Matrix (proposed md-5zi container cells)

PASS means observed on a **task-owned attached GTK desktop fixture**. It does
not substitute for the blocked live GNOME registry cell below. `md-ah9.12` owns
incorporating these named cells into the wider md-5zi matrix.

| md-5zi container cell | Docker 29.1.3 | Rootless Podman 4.9.3 |
|---|---|---|
| Tier A / Linux Wayland fixture / MCP HTTP bearer initialize → initialized → tools/list → ui-tree | PASS | PASS |
| Tier A / same fixture / MCP stdio, expected button in real tree | PASS | PASS |
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
| Actual advertised GNOME registry tree | BLOCKED: missing socket | BLOCKED: missing socket |
| Desktop accessibility loss → exit → restart → recovery | **PASS; see recovery matrix above** | **PASS; see recovery matrix above** |
| Docker Compose HTTP/secret/readiness/shutdown | PASS, healthy; exit 0 in 0.925 s | Podman Compose unvalidated; explicit CLI used |
| DT_NEEDED in runtime image | PASS, 144 resolved entries | PASS, 144 resolved entries |

No human/video/input/capture, FIPS, remote proxy/port-forward, GPU, confined
host-peer, SELinux, or Kubernetes cells are filled. StdIO child SIGTERM exits 0;
HTTP host PID 1 exits 0; preparation exits 143 intentionally. No five-second
SIGKILL fallback was used for those termination assertions.

### Evidence and deviations

The real session's `org.a11y.Bus.GetAddress` advertised
`unix:path=/run/user/1000/at-spi/bus,guid=f965b62b37de77395eea767b6af82fe1`.
`stat`/connect returned ENOENT despite `ss -lxnp` showing original listener PID
300612 at that name. No user logout or restart was attempted. Original GNOME
PID 300603 and accessibility daemon PID 300612 remained unchanged.

During initial fixture setup, XDG_RUNTIME_DIR was set **after** dbus-run-session
started. Its task-owned activated launcher therefore briefly bound the already
missing common socket path. That entire task process group was stopped, and the
fixture was relaunched with XDG_RUNTIME_DIR set **before** dbus-run-session.
No original desktop service was killed/restarted. The final fixture used only
`/home/bizarro/wt-ah9-4/fixture-runtime/at-spi/bus_0`; the original path remained
missing. Do not repeat the initial ordering mistake.

The fixture was a real GTK window on the existing Wayland display, with its own
session and accessibility buses. Both transports found the **button** label
`md-ah9.4 expected accessible button`, not just a cached app/root name. libatspi
warned that an optional app peer-to-peer socket was not mounted; bus fallback
still returned the expected button. No extra peer socket directories were granted.

Docker default AppArmor failed D-Bus Hello with:
`org.freedesktop.DBus.Error.AccessDenied: An AppArmor policy prevents this sender`.
The separate checked-in profile resolved it; **no unconfined/host-network
workaround**. Rootless Podman's distinct AppArmor-disabled state is not equivalent
LSM coverage. Docker Compose secret mode/owner were accepted by entrypoint.

Final driver receipts (`reference/validate.py docker` and `... podman`):

```text
wrong identity: PASS: wrong desktop identity
missing bus socket: PASS: missing session bus socket
missing secret: PASS: missing /run/secrets/mcp-token
no desktop direct host: PASS: accessibility backend unavailable
DT_NEEDED closure: PASS: 144 resolved entries
readiness: PASS
missing bearer: 401
initialize/tools/read: PASS; expected app present; input/capture tools absent
only TCP listener=7710; 7700 absent; zero caps; no devices/PipeWire socket; IPv4 dial PASS
read-only root/no privileged/no device assignments: PASS
```

Docker native `on-failure:3` exercised four intentional missing-secret failures:
`RestartCount=3`, die-to-die intervals **0.220, 0.311, 0.547 s**. Rootless user
systemd used `Restart=on-failure RestartSec=2s StartLimitIntervalSec=30s
StartLimitBurst=3`: journal starts at **11:38:24, :26, :28 -0800**, then rate
limited at :30; `RestartUSec=2s`, `NRestarts=3`. These are policy tests, **not**
claims that backend disconnects trigger restart.

Controlled bus loss: both HTTP containers first passed the button smoke. Only
private AT-SPI daemon **560971**, verified in task process group **560944** with
the task-owned socket address, received SIGTERM. Event/process waits bounded at
five seconds showed **both hosts still running**, while probes returned nonzero
with ENOENT for the private socket. This reproduces md-ah9.15 without touching
the user's GNOME bus; no logout/relogin recovery pass is claimed.

### Reproduction and quality gates

Use an isolated checkout under `~/wt-ah9-4`, never `~/metadesk`. Follow the bundle
contract and README. For the safe fixture, from that task checkout:

```sh
mkdir -m700 fixture-runtime
nohup setsid env XDG_RUNTIME_DIR="$PWD/fixture-runtime" \
  WAYLAND_DISPLAY=/run/user/1000/wayland-0 DISPLAY=:0 NO_AT_BRIDGE=0 \
  dbus-run-session -- sh -c 'echo "$DBUS_SESSION_BUS_ADDRESS" > fixture-session-address; exec python3 deploy/containers/reference/fixture.py' \
  >fixture.log 2>&1 </dev/null &
echo $! > fixture-group
# Provision mode-0600 mcp-token in this task directory, not the image context.
# Load the AppArmor reference for Docker, then:
python3 deploy/containers/reference/validate.py podman
python3 deploy/containers/reference/validate.py docker
# Terminate only the recorded task process group at cleanup:
kill -TERM -- -$(cat fixture-group)
```

Set the runtime environment before the **bus daemon**, not just before the app.
`validate.py` discovers the task bus address and waits on host log events; it
does not sleep to infer success. Its 40 s startup deadline allows the explicit
30 s preparation budget. It names every validation container before launch and removes them in `finally`,
including runtime-CLI timeout paths.
Do not reuse its private-bus failure simulation against a real desktop.

`meson test -C build-container --print-errorlogs` on the VM:
**Ok 22 / Fail 0 / Skipped 0 / Timeout 0**. No live GNOME suite pass is implied.
Bundle SHA256SUMS verified in-image; all producer ELFs had no RPATH/RUNPATH.
Shell syntax (`sh -n`), Python in-memory compilation (no pycache), `git diff
--check`, and Compose config all passed. Review fixes covered sudo environment
preservation, byte-buffered deadline-bounded stdio reads, naming all containers
for timeout cleanup, and checking **all nine** action tools. Both runtime drivers
and Compose were rerun after those fixes. No metadesk code defect beyond existing
md-ah9.15 was found or patched.

Context triage: four Jev `find-lines` calls (presence 0.73–0.94) narrowed Meson,
host, and MCP-test reads; requested design sections were read explicitly.
20 requests / 163,124 input tokens / $0.006852; no absence judgment gated tests.

### Cleanup and follow-up

Cleanup verified on the VM:

- All task containers/images removed on both runtimes; no volumes were created.
  Docker `system df` before uninstall: **Images 0 / Containers 0 / Local Volumes 0 /
  Build Cache 0**, all **0 B**. Rootless Podman `system df`: **0 / 0 / 0**, all **0 B**.
- Compose's `ah9-4_default` network removed. Only Podman's built-in `podman`
  network remained; no task-created Podman network. Docker bridge removed and
  its IPv4/IPv6 firewall chains/rules removed specifically, preserving other
  rules and the observed ACCEPT forwarding policy.
- Purged **docker.io, docker-compose-v2, containerd, runc** (all newly installed).
  Docker command, newly created empty `docker` group, `/var/lib/docker`,
  `/var/lib/containerd`, runtime sockets/directories, and both task-loaded
  AppArmor profiles absent. No user group membership was added.
- Fixture process groups stopped; task-created diagnostic crash report removed;
  `~/wt-ah9-4`, both Meson builds, staged bundle, bearer file, and logs deleted.
  No task systemd units, fixture/container/runtime helpers remained. After final
  Podman inventory, `podman system migrate` stopped its task-created pause
  process; native process inspection confirmed no remaining helpers.
- Original GNOME/a11y PIDs **300603/300612** unchanged; original missing socket
  state unchanged. No logout/restart/repair of the live desktop.
- Final root filesystem: **62 G total / 50 G used / 9.2 G available / 85%**;
  byte receipt: **9,870,417,920 available** (initial `df -h`: 9.3 G available;
  minimum observed during work: 7.2 G). No macOS build or pycache dirs created.

At fixture qualification time, **md-ah9.16** tracked live GNOME requalification
after operator socket restoration. That live row is now superseded by the
md-ah9.16 results above. Accessibility-loss recovery is now qualified by
**md-ah9.4** above; actual session-bus replacement requires recreation. No new
product-code defect bead was needed. Changes are for local review only:
**no push or merge**.
