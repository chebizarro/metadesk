# Tier A FIPS mesh reference — md-ah9.5

Validated 2026-11-18 on the Linux VM, **rootless Podman**, with two real
`fipsd` processes, two real TUN interfaces and the live GNOME desktop.
All required mesh/auth/restart cells below passed. No Docker engine was
installed; Docker, rootful Podman and non-arm64 cells are **NOT RUN**.

This is Tier A observation, not input injection or video. The remote client
invoked `metadesk_windows` and read the UI tree containing `gnome-calculator`.
Input/capture tools were absent. Nothing restarted or changed the live GNOME
session, accessibility bus or Calculator.

## Pins and scope

| Component | Measured pin |
|---|---|
| metadesk | master `c571b5d`, task-local Tier A bundle produced with `throwaway-bundle.sh`, `-j2` |
| nostrc C daemon | `5d09cab9183a09a0b0b88d89c20495b55beb2a50`; `fipsd 0.1.0`, `libfips-core 0.1.0`, `aarch64-linux-gnu` |
| C port's upstream compatibility baseline | Rust FIPS `0.6.0-dev`, `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, as recorded in that pin's `libfips-core/UPSTREAM.md` |
| Rootless runtime | Podman `4.9.3`, crun, Ubuntu 24.04 aarch64, kernel `7.0.0-42-generic` |
| Compose client | standalone Docker Compose `5.5.1`, Podman Unix API, **not Docker Engine** |
| Compose binary SHA-256 | `732e3a84c1a0f67256ce80bc2598a24546b10ca05f9faa97efceb1171ece2ef7`, matched upstream asset checksum |
| Base, arm64 manifest | `docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8` |
| Mesh/proxy image | `localhost/ah9-5-mesh:5d09cab`, image ID `f8aeb73e1bd907131162fed1b9e633fb466328b1a838111d24f344fe41fb6aad` |
| Tier A image | `localhost/ah9-5-agent:c571b5d`, image ID `37205c5d9572d5208574340fa3b84592b8baf368ef3792624127f598dac5a789` |
| Proxy | Ubuntu HAProxy `2.8.30-0ubuntu0.24.04.2` |
| Test signing library | distro libsecp256k1 `0.2.0-2` on VM |

**Tier correction:** the adapter is the requested nostrc C daemon, not a
claimed upstream v0.6.x release. Its own version is 0.1.0; the upstream
compatibility pin is a development snapshot. Dependencies are distro packages,
not a fully reproducible apt snapshot; retain the qualified image IDs or
requalify a rebuild. No upstream daemon image name/flags were invented.

## Matrix and live receipts

| Cell | Rootless Podman 4.9.3 |
|---|---|
| Real two-node UDP FIPS mesh and TUN-routed IPv6 TCP | PASS |
| Only mesh role has `NET_ADMIN` and TUN | PASS, on both nodes |
| Agent has no FIPS control directory/key mount | PASS |
| Remote NIP-98 → initialize/initialized, tools/list, windows tool, UI tree | PASS |
| Remote bearer → same live desktop operations | PASS |
| Unauthenticated remote peer | PASS, HTTP 401 in both credential runs |
| Foreign Host / Origin through proxy | PASS, HTTP 403 / 403 in both runs |
| SSE GET with NIP-98 / bearer / no credential | PASS, 200 / 200 / 401; foreign Host and Origin each 403 |
| Restart mesh only; both auth modes recover | PASS, three cycles (restart, stop/start, restart with SSE checks) |
| Restart agent only; both auth modes recover | PASS, three cycles (last includes SSE checks) |
| Mesh stopped: local Tier A HTTP and live Calculator widgets still work | PASS |
| Mesh public listener reachable on underlay IPv4 | NOT EXPOSED, connection refused |
| Agent port 7710 reachable on mesh IPv6 | NOT EXPOSED, connection refused |
| Docker / rootful Podman / video / input / relay rendezvous | NOT RUN |

Node addresses (throwaway identities, not production credentials):

- Host: `fd4d:ec6:2ea9:e068:9ded:42de:8141:10d1`, underlay `10.89.95.2:2121`.
- Peer: `fd77:c7db:7718:228a:5afb:3ef0:f4c9:3ffa`, underlay `10.89.95.3:2121`.
- Public MCP URL: `http://[fd4d:ec6:2ea9:e068:9ded:42de:8141:10d1]:7711/mcp`.

The peer's kernel route to the host address was **dev fips0**, with the peer's
FIPS IPv6 source. Each daemon reported `tun_name=fips0`, `tun_state=active`,
`peer_count=1`, `link_count=1`, `session_count=1`, `host_drops=0`.
`show peers` showed a connected UDP path and Noise counters; `show sessions`
showed a proven, established session. Host forwarding counters after the first
auth pair included 177 delivered packets / 33,152 delivered bytes. This was
not a fake mesh or an HTTP request to the underlay bridge.

The host holder, mesh, agent and proxy shared `net:[4026532673]`; the remote
holder and mesh shared a different namespace, `net:[4026532607]`. The host
inode remained unchanged across all tested mesh and agent restarts.

First restart run: mesh restart began `09:20:44Z`; both auth modes had passed
by the next phase at `09:21:30Z`. Agent restart plus both auth runs completed
by `09:21:42Z`. These are **whole-cell times**, not a sub-5-second remote
readiness claim. FIPS reconnection/session recovery can take tens of seconds.
The client uses bounded socket deadlines, not sleeps to declare success.
A second mesh stop/start and agent restart also passed both auth modes.
After review hardening, a third mesh restart and agent restart passed both
credential modes again, now including SSE GET gates and explicit stream
cancellation immediately after authenticated headers (not continuous-stream
reconnect qualification).

During the second mesh-down cell, the existing Tier A smoke additionally
verified the Calculator window, buttons `0`, `7`, `+`, `=` and two display
text nodes. The agent stayed up independently of FIPS.

Runtime privilege receipts:

| Role | Effective caps | `/dev/net/tun` | Application mounts |
|---|---|---|---|
| holder, peer holder | `0000000000000000` | absent | none |
| mesh, peer mesh | `0000000000001000` (NET_ADMIN only) | char `10:200` | its own daemon YAML only |
| agent, UID/GID 1000 | `0000000000000000` | absent | session bus, AT-SPI directory, bearer file, public allowlist |
| proxy, UID/GID 1000 | `0000000000000000` | absent | proxy config only |

All six used read-only roots and no-new-privileges. The agent's `/run/fips`
was absent. Podman's Docker-compatible inspect returned an empty
`HostConfig.Devices` even for the mesh, so device qualification used actual
in-container character-device checks, not that incomplete API field.

## Build and provision

Use a task-owned directory (`~/wt-ah9-5` in this run). Do not edit `~/nostrc`,
`~/metadesk` or another agent's files. Check `df -h /` at each build phase;
stop below 3 GiB free. Builds here used `-j2`. Build the Tier A bundle/image
using the existing bundle contract and `Dockerfile.agent`.

Export the **exact** nostrc commit, rather than copying a dirty checkout:

```sh
# On the source machine; stream into the VM's new, task-owned directory.
git -C /Users/bizarro/Documents/Projects/nostrc archive \
  5d09cab9183a09a0b0b88d89c20495b55beb2a50 |
  ssh bizarro@192.168.64.3 'mkdir -p ~/wt-ah9-5/nostrc; tar xf - -C ~/wt-ah9-5/nostrc'
# Copy Dockerfile.mesh from this checkout into ~/wt-ah9-5, then on VM:
cd ~/wt-ah9-5
podman build --layers -t localhost/ah9-5-mesh:5d09cab \
  -f Dockerfile.mesh \
  --build-arg BASE=docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8 \
  nostrc
```

The image builds fipsd and fipsctl **inside** the builder, using the pinned
source's packaging feature flags, a system nsync, no tests/apps/GTK, and
`FIPSD_GIT_REV` for exported-tree provenance. Runtime includes HAProxy and
Python to keep this small reference to one mesh/proxy/test image. The agent
uses its own Tier A image, not the daemon image.

Provision three distinct throwaway keys: mesh host, mesh peer, MCP signer.
`mesh-peer.py --generate-key PATH` writes a new mode-0600 hex key without
printing it; `--identity PATH` prints only npub/pubkey/derived IPv6. The tool
requires distro libsecp256k1 and is a **test client**, not a production signer.
Use real managed keys/secret provisioning for deployment.

Keep the private provisioning directory mode 0700. Each daemon YAML contains
its explicit, stable identity, so its IPv6 survives restart. Never place a
secret key in Compose environment variables. Example schema (**replace all
placeholders**):

```yaml
node:
  identity: {nsec: REPLACE_WITH_PRIVATE_HEX_FROM_FILE, persistent: false}
  control: {enabled: true, socket_path: /run/fips/control.sock}
  rendezvous: {nostr: {enabled: false}}
tun: {enabled: true, name: fips0, mtu: 1280}
dns: {enabled: false}
transports:
  udp: {bind_addr: "0.0.0.0:2121"}
peers:
  - npub: REPLACE_WITH_OTHER_NODE_NPUB
    addresses:
      - {transport: udp, addr: "10.89.95.3:2121"}
```

For the peer, use its own key and the host address `10.89.95.2:2121`.
An explicit key wins over `persistent: false`; no identity is regenerated on
restart. The daemon runs as `0:777` inside the keep-ID user namespace, with
only NET_ADMIN. Mount its YAML read-only with readable DAC permissions for
that mapped identity. This run used mode 0444 **inside a mode-0700 host
parent**, mounted as a single file only into its daemon. No other container
receives that private directory. The control directory is a private tmpfs;
there is no need for a shared control volume in Tier A.

The packaged fipsd example's commented peer fields `address` and `transport`
are stale at this pin: they warn and then fail validation. Use `addresses`
as above. Follow-up **md-uzf** records the exact refusal; no nostrc code was
changed here.

For MCP, create a random, desktop-owned **0600** bearer file and a public
allowlist file containing the MCP signer's npub. The private MCP key exists
only in the remote test client; neither proxy nor agent receives it.

## Start and test

Copy `compose.mesh.yaml` and `mesh-proxy.cfg` together into the task directory.
Set these variables in a private env file (paths are absolute):

```dotenv
MD_BASE=docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
FIPS_IMAGE=localhost/ah9-5-mesh:5d09cab
MD_AGENT_IMAGE=localhost/ah9-5-agent:c571b5d
FIPS_CONFIG=/home/bizarro/wt-ah9-5/secrets/host.yaml
MCP_TOKEN_FILE=/home/bizarro/wt-ah9-5/secrets/token
MCP_ALLOW_FILE=/home/bizarro/wt-ah9-5/secrets/allow
AT_SPI_DIR=/run/user/1000/at-spi
MD_MESH_IP=REPLACE_WITH_HOST_FIPS_IPV6
```

Use the discovered AT-SPI parent directory, not one socket inode. All desktop
mounts stay read-only; never recursively relabel the live user runtime dir.

Start a task-owned Podman API socket, then the holder before its consumers:

```sh
podman system service --time=0 unix:$HOME/wt-ah9-5/podman.sock
# In another shell; standalone Compose executable placed in the task directory:
export DOCKER_HOST=unix://$HOME/wt-ah9-5/podman.sock
./docker-compose --env-file mesh.env -f compose.mesh.yaml up -d --no-recreate netns
./docker-compose --env-file mesh.env -f compose.mesh.yaml up -d --no-recreate
```

**Runtime caveat (md-ba4):** on the measured API/client combination, a plain
second `up -d` attempted to recreate the unchanged holder and Podman refused
because of dependent containers, leaving a temporary renamed replacement.
The qualified commands use `--no-recreate`. To change configuration, remove
all namespace consumers **before** replacing the holder, then start afresh.
Do not use `--force-recreate` on the holder alone. This reconciliation path
is not qualified. `restart mesh` / `restart agent` are qualified separately.

The manifest deliberately uses Podman's `userns_mode: container:ah9-5-netns`
and tmpfs `U` ownership. Docker cannot use this manifest unchanged. The
Podman 4.9.3 compatibility API rejected tmpfs `uid=`/`gid=` options; `U`
worked without relabeling or changing any desktop bind mount.

HAProxy binds **only** the configured mesh IPv6 on port 7711. If it starts
before TUN's address exists, its failed bind exits and on-failure retries.
TCP mode preserves the entire HTTP byte stream, including Authorization,
Host, Origin and the signed body. No signer, auth cache or HTTP header rules
live in the proxy. See the [HAProxy 2.8 configuration manual](https://docs.haproxy.org/2.8/configuration.html).
The agent binds only `127.0.0.1:7710`, and `--mcp-public-url` is the exact
public URL including `/mcp`; the client signs that URL. Foreign Host/Origin
403 tests traverse this proxy, so success is not hiding a localhost rewrite.

For the remote node, create another capability-free keep-ID holder on
`ah9-5-underlay`, IP `10.89.95.3`. Run the same mesh image as `0:777`, joining
that holder's **user and network namespaces**, with only NET_ADMIN,
`--device /dev/net/tun`, read-only root, no-new-privileges, private
`--tmpfs /run/fips:rw,nosuid,nodev,noexec,mode=0750,U`, and its own YAML.
Then run the test client in that remote namespace (repeat with `--token`
instead of `--key`):

```sh
podman run --rm --name ah9-5-peer-client \
  --network container:ah9-5-peer-netns --userns container:ah9-5-peer-netns \
  --user 1000:1000 --cap-drop ALL --security-opt no-new-privileges --read-only \
  -v "$PWD/mesh-peer.py:/test/mesh-peer.py:ro" \
  -v "$PWD/secrets/mcp.key:/run/secrets/key:ro" \
  -v "$PWD/secrets/token:/run/secrets/token:ro" \
  localhost/ah9-5-mesh:5d09cab python3 /test/mesh-peer.py \
  --url "http://[${MD_MESH_IP}]:7711/mcp" --key /run/secrets/key
```

`mesh-peer.py` performs real HTTP requests with signed NIP-98 payloads,
then checks the actual JSON-RPC results. An initialized notification (202)
precedes operations. A nonce prevents accidental same-second replay of a
test event. HTTP proxies from the environment are disabled, redirects are
refused, and cryptographic calls/evidence gates remain active under `python3 -O`.
SSE checks authenticate GET, require the event-stream content type, then
explicitly cancel the test stream after headers; no SSE body polling is used.

Repeat both auth runs after `podman restart -t 5 ah9-5-mesh` and after
`podman restart -t 5 ah9-5-agent`, checking the holder inode before/after.
Also stop mesh only, run the existing agent-local `probe.py --http` and
`smoke_mcp.py --http --calculator-widgets --expect gnome-calculator`, start
mesh again, and repeat remote auth. No relay polling or sleep-based
completion is involved; these are endpoint operations and measured success.

## Quality gates, review and cleanup

- Runtime-image builds and in-image DT_NEEDED closure: PASS.
- Local Python reference tests: **17/17 PASS**, both normal and `-O`; VM
  client tests: **5/5 PASS**, both normal and `-O`. These include crypto,
  credential-bearing redirect rejection, and an optimized CLI regression
  requiring missing desktop evidence to fail. A wrong checksum in the
  initially written expected npub fixture was corrected against the pinned
  daemon's existing native-protocol fixture before the passing runs.
- `docker-compose config -q`: PASS. Actual API startup required the Podman
  tmpfs syntax and lifecycle commands documented above.
- Nostr Protocol Review of this change: no relay client, REQ/publish path,
  EOSE boundary, subscription close, or rendezvous implementation added.
  Offline allowlist plus NIP-98 HTTP only; rendezvous explicitly disabled.
  No ignored relay OK/CLOSED/AUTH or polling-as-delivery introduced. Proxy
  idle/connect timeouts and endpoint socket deadlines are operational bounds,
  not relay completion timers. Existing host auth code was not changed.
- Disk: 9.3 GB initially, 4.4 GB lowest observed, 5.2 GB after pruning the
  task's builder and superseded image by exact ID. Never below 3 GB.
- Oracle review completed over the six actual file attachments after the
  initial selection payload arrived empty. Fixed its two P1 findings:
  credential-bearing redirects and assertion-elided gates/signing calls.
  Also added its suggested real-mesh SSE GET authentication checks. Re-ran
  tests and both auth modes across another mesh/agent restart; all passed.
  Both live credential modes also passed with the final client run using
  `python3 -O`, including SSE GET.
- Jev triage used 10 `find-lines` calls (30 window requests, 248,690 input
  tokens, about $0.01044) to narrow long design/validation/FIPS reads;
  low-presence ranges were not treated as evidence.
- Final cleanup: **COMPLETE**. No `ah9-5` containers, images, networks or
  task API process remained; `~/wt-ah9-5` (including all throwaway keys,
  configs, binaries and logs) was absent. Task builder/superseded/runtime
  image layers were removed by exact IDs/tags, not a global prune. Shared
  Ubuntu base and sibling `atg-video:baseline` image were left untouched.
  The API socket disappeared during directory cleanup once; the retry
  confirmed the entire directory absent after the API exited.
- Final VM free disk: **5.7 GB** (`df -h /`), versus 9.3 GB at start; other
  agents shared the VM throughout. GNOME still returned the original live
  `/run/user/1000/at-spi/bus_0` address after cleanup. No Docker package was
  installed, so no Docker purge was necessary. The temporary standalone
  Compose download/checksum were also removed locally.

Remaining work: **md-uzf** (external packaged config example) and **md-ba4**
(Compose/API reconciliation). Neither was worked around by pretending an
underlay connection was a mesh or by granting extra agent privileges.
No `src/` or existing Tier A files were modified.
