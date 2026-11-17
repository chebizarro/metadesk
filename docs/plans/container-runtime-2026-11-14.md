# Running metadesk Host/Client in Containers: Plan

**Spike:** `md-ah9` · **Deliverable:** this design doc + reference deployment artifacts (`deploy/containers/`) + ready-to-file work items. Scope is "make it run headless in a container"; OCI image publication is md-kb7. The `## Background` under §2 is curated Phase-2 evidence.

# 1. Summary

**md-ah9 will deliver deployment designs, reference artifact specifications, and validation gates—not production runtime changes or published images.** Start with a Linux MCP host attached to an existing, operator-managed desktop accessibility session; disable input and expose no network endpoint by default. Use direct-IP/local transport for the minimal deployment and an external FIPS sidecar with a shared network namespace for the mesh reference. For second-tier video, standardize the experiment on **Sway/wlroots headless**, not Cage or GNOME headless, with `xdg-desktop-portal-wlr`; treat portal capture as unverified until tested and require a named Wayland virtual-input follow-on for reliable isolated control. Coordinate the build/runtime contract with **md-kb7** and the container acceptance cells with **md-5zi**.

Artifact excerpts below are partial reference shapes; the accompanying contracts specify the intended files without supplying production implementations. Nothing here is built, deployed, or validated yet — every reference artifact is labeled "unvalidated until the listed gates pass."

## 1.1 Execution index

Work items to file as spin-offs of `md-ah9` (details in §6; file impact in §4; privilege detail in §3.5). "Size" is rough: S ≤ a few days, M ≈ 1–2 weeks, L ≈ 3+ weeks.

Corrected against the 2026-11-14 design review (`docs/reviews/container-runtime-plan-critique-2026-11-14.md`); see §7 for the code-verified corrections and open decisions those IDs reference. Spin-off work items (W1–W9) are **not yet filed** in bd.

| # | Goal | Done when | Key files | Depends on | Size |
|---|------|-----------|-----------|------------|------|
| P0 | Land this spec + reference artifact drafts | Plan updated; artifacts drafted; md-kb7/md-5zi review requested; bundle-ownership (§7) decided | `docs/plans/container-runtime-2026-11-14.md`, `deploy/containers/*` | — | S |
| W1 | MCP container lifecycle, transport, **security/correctness** (**code**) | Input-requiring tools error under `--no-input` (no fake success); Host/Origin reject on HTTP; host actually calls `md_mcp_bridge_shutdown`; wakeable stdio; blocking a11y unsubscribe (3 backends); read-timeout < 5 s SIGTERM < grace; no MCP-branch leak; §3.3 acceptance + Nostr Protocol Review | `src/host/main.c`, `src/core/mcp_{bridge,http,stdio,server}.*`, `a11y.*`, `a11y_atspi.c`, `a11y_axui.m`, `a11y_uia.cpp`, `agent.c`, `tests/test_mcp*` | P0 | L |
| W2 | Validate attached-desktop MCP host (Docker + rootless Podman) | Expected app tree via MCP; negative identity/socket tests fail clearly; no port 7700/capture/device | `deploy/containers/{Dockerfile.agent,reference/*}` | W1, bundle (§7.2) | M |
| W3 | FIPS **binary-session** sidecar reference (re-tiered, §7.1) | Only mesh owns TUN/NET_ADMIN; netns-holder so restart recovers; paired with `--npub`/`fips_control` host-or-client, not Tier-A MCP | `deploy/containers/{compose.agent.yaml,kubernetes/*}` | W4/W7, pinned FIPS adapter | M |
| W3b | Kubernetes agent-only Pod reference | Node pinning, `hostPath` desktop sockets, RBAC port-forward, clean termination | `deploy/containers/kubernetes/agent-pod.yaml` | W2 | M |
| W4 | Sway headless portal-capture reference | Pinned compositor/backend capability + frame-delivery report (capture only) | `deploy/containers/reference/sway-session.sh`, `Dockerfile.video` (new) | P0 video decision | M |
| W5 | Linux Wayland virtual-input backend for wlroots (**code**) | Target-only pointer/keyboard via existing HAL; explicit `wayland` selection, no uinput fallback | `src/core/input_wayland.c`, `input.c/.h`, `src/host/main.c`, `meson*`, `tests/test_input.c` | W4 (soft) | M |
| W6 | Unattended portal policy + secure restore-token lifecycle (**code**) | Fail-closed capture, bounded consent, atomic 0600 token, no auto-delete on no-frames | `src/core/capture_pipewire.c`, `capture.*`, host CLI, capture tests | W4 (soft) | M |
| W7 | Container client qualification (decode-only + GUI) | Separate headless vs display authorization results | client build; `deploy/containers/*` | md-5zi | S |
| W8 | Add container acceptance cells to md-5zi | Cells incorporated into the owning matrix (individual cells mark "pending W1/W4") | md-5zi matrix | P0 | S |
| W9 | Consume md-ah9 runtime-bundle/deploy contract in md-kb7 | Packaging handoff acknowledged; publishing stays in md-kb7 | `deploy/containers/reference/bundle-contract.md` | P0 | S |

# 2. Current-state analysis

## Evidence conventions

The revised document should distinguish:

- **Established code fact:** directly visible in the supplied source.
- **Curated evidence:** existing Background findings and historical file:line anchors, retained below.
- **External fact:** behavior attributed to upstream documentation.
- **Design decision/inference:** the deployment architecture selected by this spike.
- **Validation required:** behavior not demonstrated by the supplied evidence.

Historical anchors must remain intact. Where current source differs from a Background characterization, append a correction rather than silently rewriting the evidence.

## Background

*Retain the existing section below as curated evidence.*

### What the metadesk host requires from the OS today (`src/core`, `src/host`)
- **Screen capture (`capture_pipewire.c`):** connects to the session D-Bus
  (`DBUS_BUS_SESSION`), drives `org.freedesktop.portal.Desktop` ScreenCast,
  requests a **monitor** source w/ embedded cursor (43–72, 294–301, 355–380).
  `Start()` is the consent step, empty parent handle, waits up to **180 s**
  (396–426). A valid `restore_token` can skip re-consent; stored at
  `$XDG_STATE_HOME/metadesk/screencast.token` (or `$HOME/.local/state/...`,
  else `/tmp/...`; dir made 0700, file mode is umask-dependent) (244–285).
  `OpenPipeWireRemote` returns a **Unix fd** → `pw_context_connect_fd`; on
  portal failure falls back to `pw_context_connect()` (default PW server)
  (484–514, 788–811). No explicit physical-monitor check. PipeWire ≥0.3.65 +
  D-Bus (meson 200–218).
- **Input (`input_uinput.c`):** opens `/dev/uinput` (write, nonblock), creates
  virtual keyboard + mouse via `UI_SET_*`/`UI_ABS_SETUP`/`UI_DEV_CREATE`
  (59–143); `ABS_X/Y` mapped 0..screen−1. Needs **write access to
  `/dev/uinput`** (root or `input` group); does **not** open
  `/dev/input/event*`; no capability escalation in code.
- **Accessibility (`a11y_atspi.c`):** `libatspi` → the **AT-SPI2 registry on a
  separate accessibility bus** (discovered via `org.a11y.Bus.GetAddress`),
  walks the tree, runs the event loop (206–324). Needs a reachable user-session
  AT-SPI/D-Bus and accessible apps; `md_a11y_create` returns NULL on failure.
- **FIPS (`fips_control.c`, `fips_addr.c`, README):** metadesk does **not** open
  `/dev/net/tun` or issue TUN ioctls — it relies on an **external FIPS daemon**.
  Control socket is `AF_UNIX`: `/run/fips/control.sock` →
  `$XDG_RUNTIME_DIR/fips/control.sock` → `/tmp/fips-control.sock` (override
  available) (fips_control.c 413–438, 614–651). `<npub>.fips` via
  `getaddrinfo`, else deterministic IPv6 fallback (fips_addr 324–371). The host
  FIPS health check runs only under `--npub` (which also requires a signer)
  (host/main.c 656–683).
- **MCP / headless path:** `--mcp` = stdio JSON-RPC on stdin/stdout; `--mcp-http
  [7710]` = IPv6 TCP, localhost default, `POST/GET /mcp` (host/main.c 611–617,
  882–905; mcp_http.c 572–660). **The MCP branch initializes a11y (required,
  non-null) + input (optional) then returns before video capture and before the
  normal port-7700 stream listener** (mcp_bridge.c 30–41; host/main.c 857–929).
  So MCP mode is **video-headless but still needs AT-SPI reaching a desktop
  root**; `/dev/uinput` is optional (bridge runs without input). Default MCP
  flags perform no FIPS check.

### External container facts (research; cite links in References)
- **Headless display:** wlroots headless (`WLR_BACKENDS=headless`,
  `WLR_LIBINPUT_NO_DEVICES=1`) / cage; `gnome-shell --headless` + GNOME Remote
  Desktop; Xvfb+X11 (X11 capture, not a portal). Capture needs a **matching
  portal backend** (`xdg-desktop-portal-wlr` | `-gnome` | `-hyprland`) — the
  portal does not itself create a desktop. SW rendering ≠ guaranteed DRI-free
  capture.
- **Non-interactive consent:** no universal "headless = approve". `xdpw` has
  `[screencast] chooser_type=none` for unattended capture (operator policy).
  `restore_token` restores prior auth only, is single-use (replace each time),
  `persist_mode` 1 (while running) / 2 (until revoked); invalid tokens fall back
  to prompting. GNOME unattended RD is a **different** authorization path than a
  generic portal ScreenCast consumer. **Implication:** metadesk's portal path is
  GNOME-style (monitor source + `Start`); validate it against each headless
  backend.
- **Input in containers:** `/dev/uinput` via `--device` + open permission
  (UID/GID/ACL, device-cgroup, SELinux/AppArmor); **no blanket CAP needed** for
  ordinary uinput. But uinput makes a **kernel device, not seat-local** — the
  headless compositor must then **discover/consume** the created
  `/dev/input/event*` (a headless backend is not automatically a libinput
  backend). Cleaner headless alternative: Wayland **virtual-pointer /
  virtual-keyboard** protocols (`zwlr_virtual_pointer_manager_v1`,
  `zwp_virtual_keyboard_manager_v1`) inject through the compositor, no
  `/dev/uinput`, work **when the compositor implements them** (wlroots yes;
  not GNOME/KDE/Weston). GNOME-type: RemoteDesktop portal `ConnectToEIS`/libei.
  **metadesk implements only `/dev/uinput`** — virtual-input support would be
  new work (note as an option, not current capability).
- **PipeWire/portal/D-Bus/AT-SPI:** either run an **own private session** in the
  container (private `XDG_RUNTIME_DIR`, one session bus via `dbus-run-session`,
  PipeWire at `pipewire-0`, portal frontend+backend, a11y) or **connect to the
  host session** (mount sockets, talk to the host portal; `OpenPipeWireRemote`
  returns an fd — mounting `pipewire-0` alone is insufficient; don't start a
  competing portal frontend). AT-SPI is a **separate bus**, so sharing only the
  session bus can be insufficient.
- **FIPS/TUN:** `/dev/net/tun` + `CAP_NET_ADMIN` in the userns governing the
  target netns + host TUN support (userspace WG doesn't remove this). Rootless
  Podman: invoking user needs device access, `--group-add keep-groups`, SELinux
  caveats. **Sidecar pattern:** mesh container gets TUN + NET_ADMIN; app shares
  its netns (`--network=container:<mesh>` / Podman pod); K8s Pod already shares
  the netns, set `NET_ADMIN` only on the mesh sidecar.
- **Kubernetes:** no universal `--device`; expose `/dev/uinput` and
  `/dev/net/tun` via a **device plugin / CDI** (hostPath ≠ full device
  authorization). Per-container `securityContext`. HW encode needs a GPU/DRI
  device plugin + userspace drivers — metadesk already falls back to SW x264.

### Prior art
- No container docs. Spec `docs/metadesk-spec.md`: §2.4 core/UI boundary permits
  **headless agent-only operation**, §2.2.1 external FIPS v0.6.x runtime
  contract.

## Additional code findings

| Established fact | Consequence |
|---|---|
| `src/host/main.c::main` initializes the signer, session log, and optional Nostr negotiation **before** entering its MCP branch. | MCP skips capture, not necessarily all normal startup. A supplied signer can cause a 60-second negotiation wait. |
| The host prints startup and MCP diagnostics with `printf`. | `--mcp` currently contaminates its JSON-RPC stdout. A clean stdio reference requires remediation. |
| MCP mode always attempts `md_input_create`. `tests/test_input.c` documents that a non-null object may be unready. `md_mcp_bridge_create` grants `MD_CAP_INPUT` based on pointer presence. | Omitting `/dev/uinput` is not equivalent to explicitly disabling input or accurately reporting capabilities. |
| `MdMcpBridgeConfig.http_bind_addr` already flows to `MdMcpHttpConfig.bind_addr`; the host CLI does not populate it. | Reuse this seam for a future explicit bind option; do not add a second HTTP transport. |
| `md_mcp_http_create` defaults specifically to IPv6 `::1`. | A colocated proxy must use `http://[::1]:7710`; do not assume `127.0.0.1` works. |
| The host signal handler changes `g_running`, but MCP transport loops use their own shutdown mechanisms. | SIGTERM does not currently establish graceful MCP shutdown. |
| `md_mcp_bridge_create` registers an accessibility callback carrying its server pointer. Destruction frees that server without visibly unsubscribing. | Callback quiescence is a release gate, including construction failure after subscription. |
| `atspi_get_tree_unlocked` can return a synthetic desktop root with zero applications. | A non-null context or root is insufficient readiness evidence. |
| `-Dclient=false` removes SDL/ImGui targets, but core still links capture, FFmpeg, accessibility, input, and Nostr dependencies. | Runtime-headless does not currently mean a media-free build. |

### Relevant ownership and flow

**MCP:**

`main` owns a11y and input → bridge borrows both → bridge owns `MdSession`, `MdAgent`, `MdMcpServer`, and transport → transport dispatches JSON-RPC → tools/resources use agent/a11y.

AT-SPI change callbacks execute through the backend’s GLib event thread. HTTP requests run concurrently through libgo handlers. Teardown must stop both sources of access before freeing the server or borrowed HAL contexts.

**Video:**

Session bus → ScreenCast portal requests → consent and stream node → `OpenPipeWireRemote` fd → PipeWire thread → borrowed `MdFrame` → capture thread → synchronous encoder callback → TCP stream.

Input travels separately:

Client action/raw input → host dispatch → `MdInput` → kernel virtual devices. Device creation does not prove the intended compositor receives those events.

**FIPS client connection:**

Daemon status → peer/session readiness → Nostr authorization → DNS or deterministic address derivation → final TCP connection.

The deterministic address is not readiness evidence. MCP HTTP does not inherit this binary-session authorization flow.

### Change strategy

Use a **targeted deployment addition plus narrowly scoped runtime follow-ons**, not a core refactor. Existing HALs, MCP configuration, FIPS control client, and Meson client switch provide the necessary seams. A media-free library split, alternative session negotiation, or container orchestration inside metadesk is unnecessary for this spike.

# 3. Design

## 3.1 Supported tiers and boundaries

| Tier | Selected architecture | Initial status |
|---|---|---|
| **A: MCP agent host** | Container attaches to an operator-managed Linux desktop’s session and accessibility buses. No capture, encoder instance, SDL window, or port 7700 listener. | First reference; input disabled. |
| **B: isolated video host** | Private Sway/wlroots headless session, PipeWire, portal frontend plus `xdg-desktop-portal-wlr`, AT-SPI, and applications. | Experimental second tier. |
| **C1: headless client** | Existing `metadesk-client --no-display`; receives and decodes video without creating a renderer. | Separate validation cell. |
| **C2: human client** | SDL/ImGui client connected to an operator-approved host display socket or nested desktop. | Opt-in; not equivalent to C1. |

`--no-display` is **not** a semantic-only agent client: the executable still contains SDL integration, creates a decoder, and requests video/agent/input capabilities during npub negotiation.

### Tier A desktop source

Choose **desktop attachment**, not an in-container desktop, for the first reference. This isolates the container question from compositor provisioning while exercising the real MCP/a11y path.

The desktop may itself be headless, but an operator must already have started:

1. A user session and session D-Bus.
2. AT-SPI bus discovery and registry.
3. At least one accessible application belonging to that session.

The reference container starts none of these services. It must not create a private bus with `dbus-run-session`, because that would select a different desktop root.

**Initial behavior is observation-only.** Existing tools remain registered; operations requiring unavailable input must return existing tool errors rather than report success. Full desktop control is a subsequent acceptance level, not an implied property of MCP availability.

## 3.2 Tier A session attachment and readiness

### Host preparation contract

Reference identity is UID/GID **1000:1000**. Other identities require rebuilding the image with matching numeric IDs and adjusting manifests.

Before deployment, the operator must:

1. Log into or provision the intended desktop session.
2. Enable toolkit accessibility and start a known accessible application.
3. Query `org.a11y.Bus.GetAddress` through that session’s D-Bus.
4. Confirm its returned address uses a reachable **filesystem Unix socket**.
5. Record the socket’s path and its containing directory.
6. Confirm the AT-SPI registry desktop contains the intended application.

For example, session discovery can use:

```sh
gdbus call --session --dest org.a11y.Bus \
  --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress
```

Do not infer the accessibility address solely from `/run/user/1000/at-spi/bus`; that is a common layout, not a universal contract.

### Mount and environment contract

| Item | Reference value/rule |
|---|---|
| User identity | Same effective host user as the desktop; rootless mappings must preserve the required host identity. |
| `HOME` | `/home/metadesk` |
| `XDG_RUNTIME_DIR` | `/run/user/1000`, image-created, owned by 1000, mode 0700 |
| `DBUS_SESSION_BUS_ADDRESS` | `unix:path=/run/user/1000/bus` |
| Session bus mount | Exact host socket → `/run/user/1000/bus`, read-only mount |
| Accessibility mount | Discovered socket directory → **the same absolute directory path** returned by discovery |
| Writable storage | Private `/tmp` tmpfs; no persistent volume required for Tier A |
| Deliberately absent | `/dev/uinput`, `/dev/input`, `/dev/dri`, Wayland/X11 sockets, PipeWire socket, host PID namespace |

Mount the accessibility **directory**, rather than only its current socket inode, to tolerate a socket replacement within that directory. Session-bus replacement still requires recreating the container.

A read-only socket mount does **not** make D-Bus access read-only. This deployment grants substantial access to that user session and is suitable only for a trusted metadesk image/operator.

Filesystem-socket attachment is the initial supported case. Abstract sockets, TCP bus addresses, Flatpak-style proxy addresses, and UID-remapped authentication failures must fail preparation rather than trigger a host-network workaround.

### Startup and health

Reference scripts belong to deployment tooling, not `libmetadesk`.

**`entrypoint.sh`:**

- Validate required paths, ownership, and socket types.
- Wait up to **30 seconds**, checking at most once per second, for session discovery and the AT-SPI registry.
- Confirm the desktop has at least one registered application.
- Print failures only to stderr.
- `exec` the host; do not remain as an ad hoc supervisor.
- Exit nonzero after the bounded wait. Do not start substitute services.

**`probe.py`:**

- Use existing command-line D-Bus tooling for discovery and registry inspection; do not implement a second AT-SPI walker.
- For HTTP deployments, additionally verify an HTTP response from `[::1]:7710`.
- Do not repeatedly send MCP `initialize`, mutate subscriptions, or invoke tools.
- Treat zero registered applications as unready for this reference.
- Emit no tree contents in health logs.

Readiness indicates **bus/root/application reachability plus transport availability**, not permission to perform every action. A separate functional smoke test verifies the expected application label through `metadesk://ui-tree`.

Use readiness failure to remove service availability, not restart loops. Liveness is process existence only; desktop outages must not cause Kubernetes to churn containers.

## 3.3 MCP transport and required runtime follow-on

### Default transport policy

- Dockerfile default: stdio.
- Long-running Compose/Kubernetes reference: HTTP on `::1:7710`.
- No default Docker published ports, Kubernetes Service, Ingress, or host networking.
- Do not use `--npub` as HTTP authentication.
- Do not supply signer/Nostr flags in reference MCP deployments.

For local stdio, use interactive stdin without a TTY. A local agent launches the container as its transport process. Do not multiplex multiple clients through one stdio stream.

For HTTP:

- Compose access is through an authorized local container-runtime operation or an explicitly added proxy.
- Kubernetes access is through RBAC-controlled port forwarding, subject to validating IPv6-loopback forwarding on the selected runtime.
- Remote access requires a colocated reverse proxy using **mTLS**, or an existing authenticated gateway with authorization for desktop control.
- Authenticate both POST and SSE GET requests.
- Reject unsolicited browser origins; do not introduce permissive CORS.
- A trusted-network-only exception must identify its firewall, principals, and lack of application authentication explicitly.

### Runtime work item: “MCP container lifecycle and transport correctness”

These are prerequisites to calling the reference supported; **md-ah9 does not implement them**.

| Component | Required behavior |
|---|---|
| Host CLI | Reject simultaneous `--mcp` and `--mcp-http`. Add `--no-input`; reference manifests pass it explicitly. |
| MCP startup | Dispatch before binary-session Nostr negotiation and session-log setup. Reject `--npub`/signer options combined with MCP rather than imply HTTP authorization. |
| Stdio | All diagnostics go to stderr; stdout contains only transport JSON-RPC. Audit linked logging paths with an end-to-end test. |
| Input | With `--no-input`, never call `md_input_create`. Otherwise pass input to the bridge only when `md_input_is_ready` is true; destroy an unready instance. |
| HTTP bind | Add `--mcp-http-bind ADDR`; pass through existing `http_bind_addr`. Default remains null/`::1`. Require an explicit numeric address; reject use outside HTTP mode. |
| Shutdown | Signal handling requests shutdown through normal execution context, not by calling bridge functions from a signal handler. The signal handler may only set state and wake a self-pipe. |
| Transport cancellation | Shutdown wakes stdio reads, stops HTTP accepts, interrupts idle connection I/O, and drains workers before destruction. |
| Callback lifetime | Accessibility callbacks must be detached and in-flight callbacks drained before server/agent destruction, including partial construction failure. |
| HTTP concurrency | Serialize operations that mutate the shared agent/session/a11y snapshot; do not assume concurrent request sinks make those objects thread-safe. |

Keep existing callable signatures where possible:

- `md_mcp_bridge_run(MdMcpBridge *) → int`
- `md_mcp_bridge_shutdown(MdMcpBridge *) → void`
- `md_mcp_http_shutdown(MdMcpHttp *) → void`

Their strengthened contract is that shutdown is idempotent and permits bounded completion before destruction. The host owns and joins its MCP runner; it destroys borrowed a11y/input contexts only after callback and request quiescence.

**Validation required before that work item is implemented:** the supplied material omits `a11y.h`, platform-independent a11y lifecycle code, and stdio implementation. Inspect their existing unsubscribe/wakeup facilities before choosing an additive HAL interface. Do not invent an incompatible unsubscribe contract from this excerpt.

Acceptance:

- No protocol contamination.
- No startup Nostr wait.
- No input device open in observation-only mode.
- Input capability absent when input is disabled/unready.
- SIGTERM exits within **5 seconds when no tool is executing**.
- Idle POST, open SSE, stdin wait, a11y notification, and construction-failure teardown do not hang or access freed memory.
- Existing MCP JSON-RPC formats and default HTTP binding remain unchanged.

## 3.4 Networking: minimal direct transport and FIPS reference

### Minimal deployment

No FIPS daemon, control socket, TUN device, or added capability is required for:

- Local MCP stdio.
- Colocated/local MCP HTTP.
- Existing direct-address binary client connections on a separately secured network.

For stdio-only observation, disable container networking entirely where supported.

Direct-address binary traffic is plaintext through the current host CLI path. Core TLS helpers exist, but neither supplied executable enables them. Do not advertise direct-IP networking as equivalent to FIPS encryption.

### Mesh reference

Use a **FIPS sidecar sharing the workload network namespace**.

| Responsibility | Owner |
|---|---|
| TUN creation, routes, mesh discovery, peer retry | External FIPS daemon |
| `/dev/net/tun`, `NET_ADMIN` | FIPS container only |
| `/run/fips/control.sock` | FIPS daemon; exposed through a dedicated shared volume |
| Capture/a11y/input/MCP | metadesk container |
| MCP authentication | Explicit gateway/proxy, not FIPS socket availability |
| Signer keys | Existing signer infrastructure; never application env/config |

**Docker Compose:** metadesk uses `network_mode: service:mesh`. Published proxy ports, if later enabled, belong on the namespace-owning mesh service, not the application.

**Podman:** prefer a Pod with the two containers sharing its network namespace. A rootful mesh reference is the baseline. Rootless FIPS is conditional on TUN access and correct capability ownership of the relevant network namespace.

**Kubernetes:** containers in one Pod already share networking. Use a dedicated volume for the control socket and add `NET_ADMIN` only to the mesh container.

A shared control socket without shared routing is insufficient. Conversely, a shared network namespace does not automatically expose an AF_UNIX control socket.

### FIPS dependency behavior

- Pin an upstream FIPS **v0.6.x** runtime version.
- Mount the control directory so socket replacement is visible.
- Reference socket permissions: directory/socket group accessible to metadesk’s selected supplementary GID; metadesk receives no daemon-owned key files.
- FIPS health checks use stable documented status fields, never the opaque diagnostic `state` string.
- Peer readiness remains `show_peers`/`show_sessions` through the existing metadesk client path.
- Mesh failure does not terminate a local MCP observation session.
- Remote mesh reachability is a separate readiness axis.

The supplied material does not establish upstream FIPS image names, exact launch flags, or credential provisioning. Those remain **required adapter inputs**, to be validated against the pinned upstream release—not fabricated in the manifest.

## 3.5 Least-privilege matrix

| Deployment | Devices/capabilities | Sockets/access | Qualification |
|---|---|---|---|
| Tier A observation | No devices; drop all capabilities | Session D-Bus and discovered AT-SPI directory | Broad user-session trust remains |
| Attached desktop with uinput | `/dev/uinput` write access; no added Linux capability normally required | Same buses | Explicitly test intended seat consumption; kernel-global effect |
| Isolated Sway with virtual input | No uinput; Wayland socket inside private session | Private session services | Requires new backend |
| Software video capture/encode | No encoder device requirement | Portal and PipeWire session access | Compositor/capture may still require render-device support |
| GPU video | Vendor GPU devices or selected DRI render node | Driver/runtime libraries | Validate actual renderer and encoder separately |
| FIPS sidecar | `/dev/net/tun`; `NET_ADMIN` | Control directory and external credential mechanism | Never grant these to metadesk |
| Headless decoder client | No display/GPU device normally required | Network; optional FIPS control socket | Still requires client build dependencies |
| Human client | Selected Wayland or X11 socket; GPU optional | Display-server authorization | Can expose the host desktop; opt-in only |

### Runtime-specific rules

**Docker**

- Use non-root application user, `--cap-drop=ALL`, read-only root filesystem, private `/tmp`, and `no-new-privileges`.
- Device-bearing variants use explicit `--device`; never recommend `--privileged`.
- Host DAC/ACL permissions and LSM policy still apply after device assignment.
- Do not relabel a live user runtime directory recursively with `:Z`.

**Rootless Podman**

- Use a keep-ID mapping that authenticates as the desktop owner.
- `--group-add keep-groups` can retain invoking-user device access when the selected OCI runtime supports it.
- UID/GID mapping, SELinux labeling, supplementary groups, and device access must be tested independently.
- Do not present rootless FIPS or GPU access as guaranteed merely because Podman accepts configuration.

**Kubernetes**

- Application: `runAsNonRoot`, UID/GID 1000, `allowPrivilegeEscalation: false`, read-only root, all capabilities dropped, `RuntimeDefault` seccomp.
- Pin the Pod to an explicitly selected Linux desktop node.
- Host socket mounts are a privileged trust decision even without Linux capabilities.
- TUN/uinput/GPU allocation uses a cluster-supported device plugin/CDI integration. A device `hostPath` alone is not a portable authorization mechanism.
- The FIPS variant requires a namespace/admission policy permitting its narrowly scoped `NET_ADMIN`; it is not compatible with an unmodified Restricted Pod Security profile.
- Rootless containers and device plugins cannot manufacture missing host kernel, GPU, compositor, or driver support.

## 3.6 Second-tier video: selected compositor and input path

### Decision: Sway/wlroots headless

Use **Sway**, rather than Cage, because the reference needs an explicit output, predictable geometry, application launch configuration, and controllable seat/session behavior. Cage remains a possible kiosk-specific derivative, not another supported reference.

Private-session composition:

1. Create private runtime directory, mode 0700.
2. Start one session bus.
3. Start Sway with the pinned release’s supported headless backend configuration.
4. Configure one output at **1920×1080, scale 1**.
5. Export the actual Wayland display/session environment to D-Bus activation.
6. Start PipeWire and its session manager.
7. Start the portal frontend with explicitly selected wlr backend.
8. Start AT-SPI and accessible applications.
9. Probe portal capabilities, desktop root, output geometry, and input protocols.
10. Start metadesk only after these probes pass.

Use software rendering and x264 as the initial measurement target. Whether that combination is genuinely DRI-free is a test result, not an architectural promise.

### Compatibility verdict

| Path | Verdict |
|---|---|
| Existing monitor ScreenCast request against xdpw | Plausible; must validate supported source/cursor/persistence properties and successful frame delivery |
| Existing `/dev/uinput` against pure headless Sway | **Not accepted as the reference input path**; no demonstrated event-device discovery/seat routing |
| Existing portal + uinput against GNOME headless | **Not established**; GNOME Remote Desktop authorization is not proof that a generic ScreenCast consumer works |
| New Wayland virtual input against selected Sway | Recommended follow-on, conditional on advertised protocol versions and permissions |

“GNOME-style” in the Background describes the current request pattern and experience; ScreenCast is a desktop-portal API, not a GNOME-only protocol.

### Named follow-on: “Linux Wayland virtual input for isolated wlroots sessions”

Extend the existing input HAL; do not create another agent action pipeline.

Planned files:

- `src/core/input_wayland.c`: Wayland backend.
- `src/core/input.c`, `input.h`: runtime selection/configuration.
- `meson.build`, `meson_options.txt`: optional Wayland dependency and protocol generation.
- Host CLI and input tests.

Proposed selection shape:

```text
input backend = auto | uinput | wayland
existing default = uinput
isolated video reference = explicit wayland
```

Requirements:

- Implement the existing `MdInputBackend` operations using virtual pointer/keyboard protocols.
- Discover supported globals before reporting ready.
- Explicit Wayland selection fails rather than falling back to host-global uinput.
- Own a Wayland connection and serialize its protocol operations.
- Release pressed keys/buttons on disconnect and destruction where possible.
- Define keymap, text-entry, scroll-unit, and coordinate mappings.
- Report unsupported text input accurately; do not claim arbitrary UTF-8 support.
- Test the selected desktop exclusively receives the events.

Keep this backend optional: it is not required for Tier A observation or ordinary attached-desktop deployments.

## 3.7 Consent, capture state, and encoder behavior

### Portal authorization

- Unattended authorization is an operator policy, not a container flag.
- For xdpw, configure `chooser_type=none` only in the **private video session**, select the intended output explicitly, and verify the pinned backend honors that configuration.
- No generic unattended GNOME claim.
- Invalid or revoked restore tokens must not leave an unattended deployment waiting indefinitely for an invisible prompt.

### Persisted state contract

Tier A has **no screencast token volume**.

Tier B reserves:

| Field/path | Contract |
|---|---|
| `XDG_STATE_HOME` | `/home/metadesk/.local/state` |
| Token | `metadesk/screencast.token`, existing opaque string format |
| Directory mode | 0700 |
| File mode | 0600 |
| Scope | One session/backend/identity; not shared across replicas |
| Rotation | Replace atomically when a fresh token is returned |
| Logging | Never log the token |
| Revocation | Remove local state and follow backend revocation procedures |

No token schema migration is needed. A follow-on must preserve readable existing token content, tighten permissions, and use atomic replacement. Rollback can read the same opaque format.

Current capture behavior remains evidence, not policy to endorse:

- Portal failure falls back to direct PipeWire.
- Consent wait can reach 180 seconds.
- The host watchdog may restart capture and delete saved authorization after repeated stalls.

For unattended video, require a future explicit **portal-required** policy: no direct fallback after denial, bounded startup, clear failure, and no automatic deletion of authorization merely because no frames arrived. The reference must remain experimental until that policy exists.

### Encode constraints

- Initial video acceptance uses software x264.
- `--no-nvenc` only disables the NVENC preference; current encoder code still tries other hardware encoders.
- Test the **actual selected encoder**, not just the flag.
- GPU variants allocate only the required GPU/render devices and matching userspace libraries.
- Hardware encoding does not require giving the application `NET_ADMIN`.
- Capture/render device requirements and encoder device requirements are separate axes.
- No DMA-BUF zero-copy claim: the current host submission path consumes CPU pixel data.

## 3.8 Reference deployment artifacts

### Artifact set

Add these **reference drafts**, clearly labeled “unvalidated until the listed gates pass”:

```text
deploy/containers/
  README.md
  Dockerfile.agent
  compose.agent.yaml
  kubernetes/agent-pod.yaml
  reference/entrypoint.sh
  reference/probe.py
  reference/smoke_mcp.py
  reference/bundle-contract.md
```

Do not add registry publishing, release tags, signing workflows, or image promotion.

### `Dockerfile.agent`

Use a **runtime-bundle Dockerfile**, not an invented nostrc package installation recipe.

The current Meson build depends on installed libraries and potentially nostrc build-tree objects. Rebuilding that ecosystem inside this spike would duplicate md-kb7’s packaging work.

Exact artifact contract:

| Stage/item | Specification |
|---|---|
| Base | Ubuntu 24.04 Linux; pin a digest when recording validation |
| Build context input | `runtime-bundle/opt/metadesk/...`, built on matching Ubuntu/architecture |
| Binaries | `metadesk-host`; no client required |
| Libraries | `libmetadesk` plus complete private non-glibc runtime dependency closure |
| Base-owned runtime | Matching glibc/loader, `libgcc`, `libstdc++`, CA certificates |
| Probe tooling | Python 3 and GLib D-Bus command-line tools |
| Library lookup | Private `/opt/metadesk/lib` in loader configuration |
| Identity | Build arguments for UID/GID, default 1000:1000 |
| Directories | Owned home and mode-0700 runtime directory |
| Entrypoint | Reference session-preparation script |
| Default arguments | `metadesk-host --mcp --no-input`, after the runtime prerequisite lands |
| Exposure | No `EXPOSE` needed; no secrets or desktop state copied into image |

Partial shape:

```dockerfile
FROM ubuntu:24.04
ARG MD_UID=1000
ARG MD_GID=1000
COPY runtime-bundle/opt/metadesk/ /opt/metadesk/
# Runtime packages, identity, directories, and reference tooling follow
# the contract above.
```

The bundle producer must verify:

- Correct architecture and compatible distro.
- No unresolved `DT_NEEDED` dependencies.
- Required dynamically loaded runtime modules remain available.
- No build-tree paths, keys, tokens, or desktop sockets are packaged.
- A no-desktop invocation fails diagnostically rather than hanging.
- `-Dclient=false` was used; a media-free build is not claimed.

### `compose.agent.yaml`

Two services in the FIPS reference:

| Service | Required fields |
|---|---|
| `mesh` | Operator-supplied pinned FIPS image/config; `/dev/net/tun`; drop all capabilities then add `NET_ADMIN`; dedicated control-socket volume; bounded shutdown; no default published ports |
| `agent` | Locally built reference image; UID/GID 1000; `network_mode: service:mesh`; read-only root; private `/tmp`; capabilities dropped; no-new-privileges; desktop socket mounts; control directory mounted read-only; command `--mcp-http 7710 --no-input` |

Partial shape:

```yaml
services:
  agent:
    network_mode: service:mesh
    read_only: true
    cap_drop: [ALL]
    command: ["metadesk-host", "--mcp-http", "7710", "--no-input"]
    # Exact mounts, identity, security options, and probes use the tables above.
```

Additional rules:

- Set the session-bus environment explicitly.
- Keep the accessibility directory at its discovered absolute path.
- Mark required desktop paths, FIPS image/config, and socket-group GID as required operator inputs.
- `depends_on` is not peer-route readiness.
- Do not add `ports: ["7710:7710"]`: it neither preserves localhost-only binding nor supplies authentication.
- Document a minimal **agent-only** invocation without mesh, using the same image and socket contract.
- Document rootless Podman as a separate launch procedure using a Pod/keep-ID, not an assumption that every Compose field maps identically.

### `kubernetes/agent-pod.yaml`

Use a **single reference Pod**, not a replicated Deployment. It attaches to one specific desktop session and must not silently migrate to another node.

| Item | Required fields |
|---|---|
| Scheduling | Linux node selector plus explicit operator-set desktop-node label |
| Agent identity | UID/GID 1000; non-root |
| Agent security | Drop all capabilities; no privilege escalation; read-only root; RuntimeDefault seccomp |
| Desktop volumes | Session socket `hostPath` of type `Socket`; discovered AT-SPI directory `hostPath` of type `Directory`; both mounted read-only |
| Writable storage | Memory-backed `emptyDir` at `/tmp` |
| Mesh socket | Dedicated `emptyDir` shared with FIPS |
| FIPS device | Site-installed device-plugin/CDI allocation; resource name supplied by cluster operator |
| FIPS security | Drop all capabilities; add only `NET_ADMIN`; other identity requirements follow pinned FIPS image |
| Process command | MCP HTTP on default `::1`, with input disabled |
| Probes | Exec readiness; bounded startup allowance; no network-driven liveness restart |
| Shutdown | 30-second Pod grace period |
| Exposure | No Service, Ingress, host ports, or host networking |
| API access | `automountServiceAccountToken: false` |

Partial shape:

```yaml
spec:
  automountServiceAccountToken: false
  containers:
    - name: agent
      securityContext:
        runAsNonRoot: true
        runAsUser: 1000
        readOnlyRootFilesystem: true
        allowPrivilegeEscalation: false
      # Desktop mounts, capability drop, command, and probes follow the contract.
```

Required setup is intentionally visible:

- An appropriate desktop node exists.
- Filesystem bus addresses and numeric identity match.
- The cluster permits the specified host socket mounts.
- The TUN device allocation mechanism and permitted mesh security context exist.
- The external FIPS image/config/credential adapter is supplied.

This is coherent as a deployment design, but not a claim of portability to arbitrary Kubernetes clusters.

## 3.9 Verification and coordination

### Proposed md-5zi matrix cells

Submit these as proposed cells and acceptance axes to **md-5zi**; do not build a competing E2E harness.

| Cell | Required assertion |
|---|---|
| Docker Linux / MCP stdio / no FIPS | Clean JSON-RPC, correct app tree, no input/capture |
| Rootless Podman / attached desktop | Correct UID authentication and separate a11y-bus reachability |
| Docker Compose / FIPS shared netns | App has no TUN/NET_ADMIN; mesh route and socket restart recovery verified |
| Kubernetes / pinned desktop node | Readiness, socket mounts, RBAC port forwarding, clean termination |
| MCP HTTP / local only | No externally reachable 7710 by default |
| MCP HTTP / authenticated proxy | Unauthorized POST and SSE denied; authorized tree access works |
| Headless client / direct IP | Decode without display server; no false semantic-only claim |
| Headless client / FIPS | Control readiness precedes connection |
| Sway video / software | Portal capture, changing frames, x264 selection, correct dimensions |
| Sway video / virtual input | Target-only input and a11y-observed result |
| GPU variant | Actual hardware selection and software fallback independently verified |

Common negative axes:

- Wrong UID.
- Session bus missing.
- Session bus available but a11y bus missing.
- Empty desktop or wrong application.
- Input disabled/unready.
- Portal denial/revocation.
- FIPS socket available but route absent.
- IPv6 disabled.
- SIGTERM during idle request, SSE delivery, and a11y notification.

Use an isolated fixture desktop for action tests. Existing input tests can create real kernel devices when permission exists; do not run them against an operator’s working desktop indiscriminately.

### md-kb7 handoff

Send **md-kb7**:

- Runtime-bundle directory and loader contract.
- Ubuntu/architecture compatibility requirements.
- `-Dclient=false` build configuration and remaining dependencies.
- Default non-root identity and runtime directory ownership.
- Stdio/HTTP commands and health semantics.
- Required runtime-remediation dependency.
- FIPS image adapter and external credential boundary.
- Explicit ownership: md-kb7 handles reproducible builds, dependency pinning, SBOMs, signatures, registry publication, tags, and upgrades.

Record coordination in md-ah9 as **requested**, **acknowledged**, or **blocked**, with links to both owning issues. Do not mark coordination complete without responses.

### References

Preserve the original reference subjects and add explicit upstream links. These links support the curated research; they were not fetched or revalidated during this text-only pass.

- metadesk: `docs/metadesk-spec.md` §§2.2.1, 2.4, 10.4; supplied source and tests.
- [ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- [RemoteDesktop portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)
- [Portal system integration](https://flatpak.github.io/xdg-desktop-portal/docs/system-integration.html)
- [xdg-desktop-portal-wlr configuration](https://github.com/emersion/xdg-desktop-portal-wlr/blob/master/xdg-desktop-portal-wlr.5.scd)
- [WayVNC FAQ](https://github.com/any1/wayvnc/blob/master/FAQ.md)
- [GNOME Remote Desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop)
- [PipeWire documentation](https://docs.pipewire.org/)
- [AT-SPI bus sources](https://gitlab.gnome.org/GNOME/at-spi2-core/-/tree/main/bus)
- [Wayland virtual-pointer protocol](https://wayland.app/protocols/wlr-virtual-pointer-unstable-v1)
- [Wayland virtual-keyboard protocol](https://wayland.app/protocols/virtual-keyboard-unstable-v1)
- [Linux uinput](https://docs.kernel.org/input/uinput.html)
- [ydotool](https://github.com/ReimuNotMoe/ydotool)
- [Linux TUN/TAP](https://docs.kernel.org/networking/tuntap.html)
- [Podman run](https://docs.podman.io/en/latest/markdown/podman-run.1.html)
- [Docker container networking](https://docs.docker.com/engine/network/)
- [Tailscale container guidance](https://tailscale.com/kb/1282/docker)
- [Kubernetes Pods](https://kubernetes.io/docs/concepts/workloads/pods/)
- [Kubernetes security contexts](https://kubernetes.io/docs/tasks/configure-pod-container/security-context/)
- [Kubernetes device plugins](https://kubernetes.io/docs/concepts/extend-kubernetes/compute-storage-net/device-plugins/)
- [Intel device plugins](https://github.com/intel/intel-device-plugins-for-kubernetes)

# 4. File-by-file impact

## Files delivered by md-ah9

| File | Change and rationale | Dependency |
|---|---|---|
| `docs/plans/container-runtime-2026-11-14.md` | Replace open questions with this architecture, preserve Background, add contracts, caveats, phases, acceptance, and issue coordination. | None |
| `deploy/containers/README.md` | Host preparation, socket discovery, identity mapping, security warnings, transport access, Docker/Podman/Kubernetes procedures. | Design decisions |
| `deploy/containers/Dockerfile.agent` | Reference runtime-bundle image definition; no publishing. | md-kb7 bundle agreement |
| `deploy/containers/compose.agent.yaml` | Shared-netns FIPS reference with localhost-only MCP and no app devices. | FIPS adapter inputs |
| `deploy/containers/kubernetes/agent-pod.yaml` | Node-pinned desktop attachment, shared socket volume, per-container privileges. | Cluster device allocation contract |
| `deploy/containers/reference/entrypoint.sh` | Bounded session-readiness gate followed by `exec`. | Desktop contract |
| `deploy/containers/reference/probe.py` | Non-mutating readiness checks. | D-Bus tooling; transport behavior |
| `deploy/containers/reference/smoke_mcp.py` | One functional session checking expected app tree and capability behavior; no independent E2E harness. | Runtime fixes; md-5zi agreement |
| `deploy/containers/reference/bundle-contract.md` | Exact staged layout, ABI/dependency rules, exclusions, validation checklist. | md-kb7 coordination |

Keep these drafts out of application installation targets. `meson.build` and `meson_options.txt` need **no change in the spike**.

## Production follow-on impact—not md-ah9 implementation

| Files | Planned responsibility |
|---|---|
| `src/host/main.c` | Early MCP dispatch, mode validation, stderr diagnostics, input disable/readiness normalization, bind option, shutdown ownership |
| `src/core/mcp_bridge.c`, `mcp_bridge.h` | Callback/request quiescence and borrowed-context lifetime contract |
| `src/core/mcp_http.c`, `mcp_http.h` | Cancellable connection lifecycle; bounded shutdown |
| `src/core/mcp_stdio.c`, `mcp_stdio.h` | Wakeable shutdown while preserving caller-owned fd semantics |
| `src/core/a11y.c`, `a11y.h`, relevant backends | Reuse or add safe subscription detachment after inspecting omitted interfaces |
| `src/core/mcp_server.c`, `mcp_server.h` | Validate shared-state serialization boundary; change only if existing dispatch lacks it |
| `tests/test_mcp.c`, `test_mcp_stdio.c`, `test_mcp_http.c`, new host process test | Protocol cleanliness, capabilities, concurrency, lifecycle regression coverage |
| `src/core/input_wayland.c`, `input.c`, `input.h`, `tests/test_input.c` | Optional virtual-input backend and explicit selection |
| `meson.build`, `meson_options.txt` | Register new runtime tests; later gate Wayland backend dependencies |
| `src/core/capture_pipewire.c`, `capture.c`, `capture.h`, host CLI, capture tests | Portal-required policy, bounded unattended startup, secure token handling |
| `tests/test_stream.c`, video E2E cells | Validate fragmented receive timeouts and simultaneous video/control sends before declaring video support |

Do not change `fips_addr.c` or duplicate `fips_control.c` readiness logic for containerization.

# 5. Risks and migration

- **User-session exposure:** bus access is a trust grant even with read-only mounts and no devices. Prefer a dedicated desktop user/session.
- **Runtime compatibility change:** rejecting signer/npub options in MCP mode is intentional; document that those options never authenticated HTTP requests.
- **Socket lifecycle:** a restarted session bus can invalidate bind mounts. Recreate the container; do not silently attach to another session.
- **Rootless feasibility:** passing runtime configuration is not proof of usable host devices or matching D-Bus credentials.
- **FIPS administration:** `NET_ADMIN` affects the shared namespace. Keep unrelated workloads out of that namespace.
- **Incomplete MCP lifecycle:** current stdout, shutdown, capability, and callback behavior prevent claiming production support.
- **Video correctness:** existing stream code also needs validation for concurrent sends and partial-read timeouts; a successful first frame is insufficient.
- **Persistence:** Tier A introduces none. Tier B retains the existing token format and requires permission/atomic-write hardening before support.
- **Rollback:** deployment drafts can be removed without data migration. Roll back production follow-ons separately; never depend on an old binary honoring a new security flag.

# 6. Implementation order

## Phase 0 — Land the spike specification

1. Update the plan while preserving Background and historical anchors.
2. Add the reference artifact drafts and their required-input contracts.
3. Request md-kb7 and md-5zi review; record actual coordination status.

**Exit criteria:** every open architectural question has a decision; every reference dependency is either concrete or explicitly operator-supplied; no image publication or production source change occurs.

## Phase 1 — Repair the MCP container prerequisite

4. Land CLI/startup/stdout/input-capability changes with host-process tests.
5. Land transport cancellation and callback/request quiescence with lifecycle tests.
6. Keep ownership-related changes atomic: bridge teardown and subscription cancellation must land together.

**Exit criteria:** the runtime acceptance list in §3.3 passes, existing MCP wire behavior is preserved, and observation mode performs no input initialization.

## Phase 2 — Validate the minimal attached-desktop host

7. Produce a local runtime bundle through the md-kb7-agreed path.
8. Build locally and test Docker stdio, then localhost HTTP.
9. Validate rootless Podman independently.

**Exit criteria:** expected application visible through MCP; malformed/missing session setup fails clearly; no port 7700, capture initialization, portal prompt, device access, or unauthorized network listener.

## Phase 3 — Validate FIPS and Kubernetes references

10. Supply a pinned upstream-compatible FIPS adapter/config.
11. Validate Compose shared routing/control socket behavior.
12. Validate the node-pinned Kubernetes Pod and cluster device allocation.
13. Submit measured results to md-5zi.

**Exit criteria:** only mesh owns TUN/NET_ADMIN; application remains non-root; FIPS failure is distinguished from a11y failure; HTTP remains local unless an authenticated proxy is explicitly added.

## Phase 4 — Establish the experimental video session

14. Provision private Sway/PipeWire/xdpw/AT-SPI with fixed geometry.
15. Validate portal capture and consent behavior without claiming input support.
16. Complete virtual-input and unattended-capture follow-ons before promoting the tier.

**Exit criteria:** changing frames, actual encoder identification, correct tree, deterministic input effect, consent-denial behavior, reconnect, and clean shutdown all pass on the pinned stack.

## Phase 5 — Client and optional GPU cells

17. Validate the existing no-display client separately from the human client.
18. Add display-socket and GPU cells only after explicit authorization and host-support checks.

**Exit criteria:** md-5zi records the precise tested runtime/device combinations; md-kb7 receives the validated runtime contract without inheriting unverified compatibility claims.

## Final decisions

- **First:** MCP observation host attached to a real user desktop accessibility session.
- **Minimal:** local/direct transport, no FIPS privileges.
- **Mesh reference:** external FIPS sidecar, shared network namespace and separate control-socket volume.
- **Video experiment:** Sway/wlroots headless plus xdpw; neither existing capture nor uinput is assumed compatible.
- **Input follow-on:** optional Wayland virtual input, explicitly selected.
- **Security default:** no input devices and no externally exposed MCP endpoint.
- **Ownership:** md-ah9 designs; md-kb7 distributes; md-5zi owns E2E matrix integration.

## Ready-to-file spin-off work items

| Proposed title | Dependency | Completion evidence |
|---|---|---|
| **MCP container lifecycle and transport correctness** | md-ah9 | Clean stdout, early startup, accurate capabilities, safe cancellation/quiescence tests |
| **Validate attached-desktop MCP containers on Docker and rootless Podman** | MCP prerequisite; md-kb7 bundle | Correct desktop tree and negative identity/socket tests |
| **FIPS sidecar shared-netns container reference validation** | Pinned FIPS adapter | Route/control separation, least privileges, restart recovery |
| **Sway headless portal capture reference** | md-ah9 video decision | Pinned compositor/backend capability and frame-delivery report |
| **Linux Wayland virtual input for isolated wlroots sessions** | Sway reference | Target-only pointer/keyboard control through existing HAL |
| **Unattended portal policy and secure restore-token lifecycle** | Capture reference | Fail-closed capture, bounded consent, atomic 0600 token updates |
| **Container client qualification: decode-only and GUI** | md-5zi matrix agreement | Separate headless/display authorization results |
| **Add proposed container acceptance cells to md-5zi** | Runtime prerequisites | Cells incorporated into the owning matrix |
| **Consume md-ah9 runtime bundle/deployment contract in md-kb7** | Distribution review | Acknowledged packaging handoff; publishing remains in md-kb7 |

## 7. Design-review corrections and open decisions (2026-11-14)

Applied from `docs/reviews/container-runtime-plan-critique-2026-11-14.md` (code-verified against metadesk; `capture_pipewire.c` not re-checked). These correct or extend §1–§6; where a correction contradicts body text, the correction wins. Leftover context_builder-session phrasing in the body ("the revised document should distinguish", "*Retain the existing section*", "the supplied material omits `a11y.h`", "this text-only pass", Phase-0 "update the plan") should be read as repo statements — the plan *is* the updated doc.

### 7.0 Status update (2026-11-16)

Work landed since the review shrinks W1:
- **Resolved:** blocking a11y unsubscribe with in-flight drain, detached in bridge destroy (**md-6h0**; `md_a11y_unsubscribe_changes`, `a11y.h:163`, `mcp_bridge.c:218`); wakeable stdio loop (100 ms `poll` slices observing shutdown, `mcp_stdio.c:117–146`); bounded HTTP/transport writes and notification quiesce before drain (**md-ed7**); coalesced a11y change notifications (**md-ayo**). a11y backend liveness is now queryable via `md_a11y_is_connected()` (`a11y.h:219`, **md-eiy**) — the natural hook for desktop-loss detection (§7.4 Q4).
- **Still open (W1):** no-input fake success (`agent.c:177` skips injection yet returns success); capability claimed from pointer presence (`mcp_bridge.c:73`) even when the host passes an unready input (`host/main.c:894–902`); host never calls `md_mcp_bridge_shutdown`, so SIGTERM is ignored in MCP mode; no Host/Origin/auth checks on HTTP; `CLIENT_READ_TIMEOUT_SEC 30` (`mcp_http.c:40`) vs the 5 s SIGTERM budget; `--mcp-http-bind` / `--no-input` CLI flags not yet added; MCP-branch leaks.

### 7.1 Corrections the code requires (highest priority)
- **`--no-input` would fake success (§3.1 is wrong).** With `agent->input == NULL`, `agent.c:192–200` skips injection and still `action_count++`/`return 0`; `mcp_tools.c:286–298` reports that as success — so `metadesk_click/type/set_value/screenshot` would **report success while doing nothing**. **W1 must** make input-requiring actions return an MCP tool error (`is_error=true`) naming the missing capability, with a `test_mcp` case. (This is the 2026-07 fake-success pattern.)
- **HTTP concurrency is already done** — `mcp_server.c:35` `dispatch_mu` serializes dispatch (`:357–365`); the agent has its own `mu`. Downgrade that W1 item to a concurrent-POST **regression test** in `test_mcp_http.c`.
- **HTTP shutdown mostly exists; real gaps are narrow.** The accept loop re-checks shutdown each second, `md_mcp_http_shutdown` closes SSE clients, reads are `SO_RCVTIMEO`-bounded, destroy joins the wait-group. The actual gaps: (a) **no production caller** of `md_mcp_bridge_shutdown`/`md_mcp_http_shutdown` — SIGTERM only flips `g_running` (`main.c:44–48`), which the MCP loops ignore; (b) **stdio `read()` can't be woken** (`mcp_stdio.c:93–114`); (c) **`CLIENT_READ_TIMEOUT_SEC=30`** (`mcp_http.c:35`) breaks the "5 s SIGTERM" target and equals the K8s 30 s grace → add an explicit budget table: read timeout < SIGTERM target < `terminationGracePeriodSeconds`.
- **a11y unsubscribe doesn't exist and the fix spans 3 backends.** `a11y.h` has `subscribe_changes` and **no** unsubscribe; the AT-SPI path invokes the callback **after** unlocking (`a11y_atspi.c:238–246`), so clearing the pointer doesn't quiesce an in-flight call. Specify `md_a11y_unsubscribe_changes(ctx, cb, ud)` as a **blocking** detach (clear under lock, then wait on an in-flight counter/condvar) implemented for `a11y_atspi.c`, `a11y_axui.m`, `a11y_uia.cpp`; call it in `md_mcp_bridge_destroy` before `md_mcp_server_destroy`, covering the construction-failure path.
- **FIPS sidecar is paired with the wrong tier.** The Compose `agent` runs `--mcp-http 7710 --no-input` on `::1`, which never touches FIPS (the FIPS check is under `--npub` only, `main.c:660–683`) and isn't reachable over the mesh. Either pair the FIPS reference with the **binary-session** host/client (Tier B/C1) where `--npub`/`fips_control` run, or keep Tier A and add an authenticated proxy container on the mesh address (dropping the control-dir mount from the agent).
- **Probe must not `GET /mcp`.** Routes are only `POST/GET /mcp`; GET opens an SSE stream capped at 4 clients (`mcp_http.c:29,440`), so a periodic probe would lock out real clients. Use a TCP connect or a 404 path — or add a `/healthz` route to W1.
- **`127.0.0.1` already works** — the listen socket clears `IPV6_V6ONLY` and v4-maps `bind_addr` (`mcp_http.c:587,593`), so `--mcp-http-bind 127.0.0.1` needs no new code. Reword the §2 code-findings row; note Docker-without-IPv6 and port-forward-dials-IPv4 as external behaviors to validate.
- **Use `[::1]:7710`**, not `::1:7710` (§3.3).

### 7.2 Gaps absent from both documents
- **MCP HTTP has no auth / Origin / Host check.** In a shared netns (`network_mode: service:mesh`, any K8s Pod) `[::1]:7710` is reachable by every container incl. the FIPS sidecar; `kubectl port-forward` exposes it to workstation browsers (DNS-rebinding). **W1:** reject non-loopback `Host`, reject non-allowlisted `Origin`; decide whether a file-mounted bearer token (mode 0600) is in scope.
- **Signal model (multithreaded host):** block SIGINT/SIGTERM before the first thread, run a dedicated `sigwait`/`signalfd` thread that calls `md_mcp_bridge_shutdown`, stdio polls a wake fd. (`close_sse_clients` takes a mutex → can't run in a handler.)
- **PID 1 during `entrypoint.sh`'s 30 s wait** ignores SIGTERM → add `trap 'exit 143' TERM INT` or `init: true`/tini; document the stdio client's startup-timeout exposure to that wait.
- **Desktop loss has no recovery owner:** have the host exit nonzero on a11y/session-bus loss so the restart policy recovers it (restart re-resolves bind mounts; recreate isn't needed) — or an operator runbook. Whether the AT-SPI backend detects bus loss is unverified.
- **Compose shared-netns restart** strands the agent when `mesh` restarts (new namespace). Add a minimal `netns` **pause/holder** service and have both `mesh` and `agent` use `network_mode: service:netns` (matches Podman Pod / K8s topology) so W3 "restart recovery" is achievable.
- **MCP-branch teardown leaks:** `main.c:909–914` frees signer/a11y/input but never `nostr`/`session_log` (created `main.c:725`/`:694`); bridge-creation failure returns without freeing `mcp_input`/`signer` (`main.c:896–899`). Fold into W1 acceptance.
- **Host-process test needs a fixture:** `mcp_bridge.c:36` requires non-NULL a11y and there is no mock backend (live a11y gated behind `-Dlive_tests`). Provide (a) a test-only mock a11y backend, or (b) a CI `dbus-run-session` + `at-spi-bus-launcher` + accessible test app (also serves md-5zi).
- **`XDG_RUNTIME_DIR` on `read_only: true` root** has nothing writable — specify a tmpfs (Docker) / memory `emptyDir` (K8s) at `/run/user/1000` with the bus socket + AT-SPI dir bind-mounted inside it.
- **Evidence-label anchors:** add file:line to the §2 "Additional code findings" table (verified: signer/Nostr setup `main.c:635–760` runs before the MCP branch `:857–916`; 60 s `go_select_timeout(…,60000)` ~`:744`; `printf` diagnostics `:860–915`; capability-from-pointer `mcp_bridge.c:47`; subscribe-no-unsubscribe `:91–96` vs destroy `:168–187`; `http_bind_addr` pass-through `:114`; `::1` default `mcp_http.c:611`).

### 7.3 Index/ownership corrections
- **Runtime-bundle circularity:** W2 needs a bundle but W9 has md-kb7 *consuming* md-ah9's contract — nobody produces the first bundle. Decide: md-ah9 ships a throwaway local procedure (`meson install --destdir` + DT_NEEDED closure check) in `bundle-contract.md`, or W2 is blocked on md-kb7 tooling.
- **W8 depends on P0** (propose cells), not W1–W4; **W9 path** is `reference/bundle-contract.md`; **W4 creates** `Dockerfile.video`+`sway-session.sh` (no Tier-B artifacts existed); **W5/W6 soft-depend** on W4 (buildable against a host xdpw / any nested wlroots); **K8s agent-only Pod** is its own item (W3b).
- **Phase-0 exit criterion** "every open architectural question has a decision" is not yet met. Bundle ownership, auth and bind are decided (§7.4 Q1–Q3, 2026-11-16). The FIPS tier, desktop-loss recovery, `--no-input` surface and K8s scope remain open (§7.4 Q4–Q7).

### 7.4 Open decisions (answers change design or order)
1. ~~Who builds the first runtime bundle?~~ **Resolved 2026-11-16: md-ah9 throwaway.** W2 ships `reference/bundle-contract.md` plus a clearly-labelled throwaway procedure (`meson install --destdir`, `-Dclient=false`, DT_NEEDED closure check against the image). md-kb7 inherits the *contract*, not the script. W2 is not blocked on md-kb7.
2. ~~In-process MCP HTTP auth?~~ **Resolved 2026-11-16: Host/Origin checks + file-mounted bearer token (option C), plus NIP-98 as a second accepted credential.** See §7.7.
3. ~~Reference HTTP bind?~~ **Resolved 2026-11-16: explicit `127.0.0.1`** in all reference artifacts via `--mcp-http-bind 127.0.0.1` (W1 adds the flag). The code default stays `[::1]`. W2/W3 still validate Docker-without-IPv6 and IPv4-dialing port-forwards on the pinned runtime versions.
4. Desktop-loss policy — host exits on bus loss (restart-policy recovery) vs stay-up-unready + runbook?
5. `--no-input` tool surface — unregister input tools from `tools/list`, or keep listed and return errors (plan's choice, needs §7.1)?
6. Which tier does the FIPS reference serve (Tier B/C1 vs Tier A + proxy)?
7. Is the agent-only Kubernetes Pod (W3b) in md-ah9 scope or deferred to md-5zi?

### 7.7 MCP HTTP authentication design (resolves §7.4 Q2)

**Always on (W1):** reject a non-loopback `Host` unless it matches a configured public URL, and reject any request carrying an `Origin` header (MCP agents don't send one; browsers always do on cross-origin fetches). This closes DNS rebinding independently of credentials.

**Credentials.** The server accepts either of two schemes on every `POST /mcp` and on the SSE `GET /mcp`:

| Scheme | Header | Who it suits | Authorizes |
|---|---|---|---|
| Bearer token | `Authorization: Bearer <token>` | Any MCP client today (static `headers` config) | Possession of a 0600 file (`--mcp-token-file`, a mounted secret) |
| NIP-98 | `Authorization: Nostr <base64 kind-27235 event>` | Nostr-native agents, or any client behind a signing shim | An **npub**, checked against an allowlist |

- **Configuration.** If neither credential is configured, loopback-only binds still serve requests, Host/Origin checks still apply, and the server logs a warning. That keeps local dev unchanged. A non-loopback `--mcp-http-bind` without a credential is refused at startup. All reference artifacts configure one.
- **NIP-98 validation reuses nostrc `nips/nip98`.** `nostr_nip98_parse_auth_header` plus `nostr_nip98_validate_auth_event` already check kind 27235, the ±60 s window, `u`, `method` and the signature. Linking it follows the nip51 build-tree-object pattern in `meson.build`. metadesk adds four things nostrc does not do:
  1. **Replay cache.** Reject a reused `event.id` within 2× the time window, using a small bounded set. This also satisfies the AGENTS.md dedupe-by-id rule.
  2. **Payload binding.** `POST` requires a `payload` tag equal to the SHA-256 of the body (NIP-98 says SHOULD; we make it MUST), so a captured header can't carry a different body.
  3. **URL matching.** nostrc does an exact `strcmp` on `u`, but the URL an agent signs is the one it dialed. Behind a proxy or `kubectl port-forward` that is not `http://127.0.0.1:7710/mcp`. Add a repeatable `--mcp-public-url` and accept `u` if it equals any configured URL or the loopback forms.
  4. **Authorization.** The event pubkey must be in an allowlist. Sources: `--mcp-allow-npub` / `--mcp-allow-file` (a mounted file of npubs; works offline in containers), or the host account's NIP-51 `metadesk-allowlist` (kind 30000), which is the same list that gates video sessions (md-70v §7.6 host role). That list comes from an `authors`+`#d` REQ subscription kept live, not polled. The relay-backed path goes through a Nostr Protocol Review.
- **Cost model.** NIP-98 requires one signature per POST, i.e. per tool call. That is cheap with a local or Grotto/NIP-55L key. With NIP-46 it costs a relay round-trip per call, so the reference docs recommend a local or delegated agent key. Session-bound credentials after the first NIP-98 check are deferred, because they would just be a bearer token again.
- **Client reality.** No mainstream MCP client signs NIP-98 today, which is why the bearer token is the universal baseline. A small stdio↔HTTP signing shim, for example a `metadesk mcp-proxy` subcommand that uses an `MdSigner`, makes NIP-98 usable from any stdio MCP client. It is a candidate follow-on, not part of W1.
- **Ties to Q6.** With NIP-98 the mesh-side proxy in option B can be a dumb TCP/HTTP forwarder that passes `Authorization` through. The agent's *npub*, not its network position, is what gets authorized. That is the seed of "MCP access governed by the same Nostr allowlist as video access". The proxy must forward the original `Host`, or the public URL must be configured.
- **Tests (`test_mcp_http.c`):** missing or invalid credential → 401; `Origin` present → 403; foreign `Host` → 403; NIP-98 checks for wrong `u`, wrong method, stale timestamp, replayed id, body/payload mismatch and unknown npub each → 401; valid token and valid NIP-98 → 200. SSE GET follows the same rules. Also confirm a ~1 KB `Authorization` header fits the request read path (`READ_BUF_SIZE 4096`, `mcp_http.c:37`).


