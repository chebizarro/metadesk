<!-- validated on Docker 29.1.3 / rootless Podman 4.9.3 for the cells below; other cells unvalidated. -->
# md-ah9.4 / md-5zi Tier A container results — 2026-11-17

## Scope and versions

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

## Matrix (proposed md-5zi container cells)

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
| Desktop logout → exit → restart → recovery | **pending md-ah9.15** | **pending md-ah9.15** |
| Docker Compose HTTP/secret/readiness/shutdown | PASS, healthy; exit 0 in 0.925 s | Podman Compose unvalidated; explicit CLI used |
| DT_NEEDED in runtime image | PASS, 144 resolved entries | PASS, 144 resolved entries |

No human/video/input/capture, FIPS, remote proxy/port-forward, GPU, confined
host-peer, SELinux, or Kubernetes cells are filled. StdIO child SIGTERM exits 0;
HTTP host PID 1 exits 0; preparation exits 143 intentionally. No five-second
SIGKILL fallback was used for those termination assertions.

## Evidence and deviations

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

## Reproduction and quality gates

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

## Cleanup and follow-up

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

**md-ah9.16** tracks actual GNOME registry requalification after the operator
restores its missing socket; the task must not repair or restart that live
desktop. Backend recovery remains md-ah9.15. No new product-code defect bead
was needed. Changes are for local review only: **no push or merge**.
