# Tier B/C1 binary video over FIPS — md-ah9.6

**Runnable reference delivered; acceptance remains blocked.** A real unpatched
Tier B → C1 video session crossed fips0 and decoded frames, but repeated streams
and restart recovery are not reliable. Do **not** close md-ah9.6 on the strength
of the scratch-image diagnostic described below. No production `src/` change is
part of this reference.

## Topology and privilege contract

`validate-mesh-video.py` creates two rootless Podman nodes on a private
**IPv4-only** bridge (`10.89.91.0/24`). Each node has a stable, capability-free
keep-ID holder. A real fipsd sidecar and metadesk share that holder's network
and user namespaces. The host and client nodes do not share namespaces.

- Host: isolated Sway → xdg-desktop-portal-wlr → PipeWire → metadesk x264.
  A changing solid-colour background damages only the private headless output.
  `--npub CLIENT --fips-control /run/fips/control.sock --bind HOST_MESH_IP`,
  `--no-input --capture-unattended`; explicit unattended consent applies only
  to that output. No host desktop/display/session-bus mount.
- Client: `Dockerfile.client`'s **decode** target, `--npub HOST --fips-control
  /run/fips/control.sock --no-display`. Software decode, no display/GPU grant.
- Each node key is also its role's signer key: the host checks show_status's
  npub against its signer before hosting. A mismatched daemon is refused.
- A third container supplies the private NIP-01 relay, NIP-46 bunkers and an
  account-setup NIP-5F signer. These are throwaway credentials, not production
  custody. `access add CLIENT video` publishes the host role's v1 grant;
  NIP-17 session negotiation and one-use stream binding are real. **There is
  no FIPS control fixture in this topology.** Relay/signing traffic uses the
  underlay; binary video uses the npub-derived IPv6 addresses on fips0.

| Role | UID / effective caps | Devices | FIPS control mount |
|---|---|---|---|
| Namespace holders and path probe | 1000 / none | none | none |
| fipsd sidecars | userns root, group 777 / NET_ADMIN only (`0x1000`) | TUN only | own per-node volume, read-write |
| Tier B host | 1000, supplementary fips (777) / none | renderD128 only | host volume, read-only |
| C1 client | 1000, supplementary fips (777) / none | none | client volume, read-only |

All these roles have read-only roots, no-new-privileges, init, and PID limits.
The fipsd secret YAML files are individually mounted read-only from a private
0700 task directory. Only each daemon receives its own nsec. The render-node
exception is the already-qualified Sway/xdpw capture requirement, not NET_ADMIN
or input-device access. Input injection is deliberately outside this cell.

The driver checks effective capabilities, devices, UID/group, mount mode,
read-only root and netns membership while host/client are running. It also
checks `ip -6 route get HOST_MESH_IP` selects fips0, the host mesh address is
on fips0, the underlay has no global IPv6, and the video listener refuses an
underlay-IP connection. Both ends log the peer's npub-derived address.

## Measured images and evidence boundary

Ubuntu 24.04 aarch64 Linux VM; **rootless system Podman 4.9.3**, crun;
`/dev/dri/renderD128` virgl. This video run is separate from the nested Podman
5.8.10 Compose experiment in [mesh-validation.md](mesh-validation.md).

| Component | Measured pin |
|---|---|
| Metadesk source baseline | `3611939` (`md/l3-containers` before the deploy-only reference edits) |
| Metadesk's nostrc | dedicated `nostrc-master`, commit `cfcb86296`, `_build` libraries |
| Host image | `md-l3-video:wip`, ID `2148eb603a99…` |
| C1 image | `md-l3-client:wip-decode`, ID `fabea0493a02…` |
| Mesh image | `md-l3-mesh:5d09cab`, ID `d99ad7c35ef8…`; nostrc/fipsd revision `5d09cab9183a09a0b0b88d89c20495b55beb2a50` |
| Runtime base | `docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8` |
| Capture/codec | Sway 1.9, xdpw 0.7.1, PipeWire 1.0.5; x264, 1920×1080, C1 software decode |

The daemon revision is intentionally the existing mesh-reference pin, distinct
from the newer nostrc libraries required by metadesk. No shared nostrc tree was
modified. Builds used `-j2`. No live GNOME bus, compositor, PipeWire socket or
uinput device was used.

## Results

Final **hardened, unpatched** rerun: **127 PASS, 11 FAIL, 16 INFO**, 703.6 s,
exit 1. All negative-receipt EOF barriers, privilege/path checks, and confirmed
resource-removal checks passed. Four cells failed the ≥60-frame gate and client
shutdown (<5 s); two retained-history checks saw extra new reconnect request
IDs. The midstream cell explicitly failed `restart recovery exercised` because
its stream prerequisite failed, so that disruption was **not run**, not passed.
Peer-down refused at FIPS readiness with the expected peer-not-configured error.

The hardening records cleanup failures, keeps failed removals owned, checks
negative logs through EOF, restricts peer-down to recognized failures, captures
the disruption cursor before restart, and requires a fresh post-restart
accepted-address receipt. Offline tests cover those cleanup/log boundaries.

Recovered earlier complete **unpatched** run: **104 PASS, 9 FAIL, 17 INFO**,
689.5 s, exit 1. Its cell results and real 94-frame receipt are below. A resumed
pre-hardening rerun was also red: **98 PASS, 10 FAIL, 16 INFO**, 723.3 s. The
footer's “non-failing” count includes INFO and is not a passing-assertion count.
The differing frame outcomes demonstrate unreliability, not qualification.

| Cell | Unpatched result |
|---|---|
| Real Nostr negotiation, binding, mesh addresses and privilege receipts | PASS |
| Initial stream and fresh sessions after restarting each node's daemon | Negotiation/binding PASS; each failed the ≥60-decoded-frame gate |
| Stable holders across individual daemon restarts | PASS: same ID, start time and netns |
| Peer daemon down / local daemon absent / wrong host daemon identity | PASS: no stream, nonzero refusal; readiness's exact refusal point is recorded separately |
| Stranger signer on the client node | PASS: pinned-host peer rejects it; no stream |
| Midstream session before disruption | PASS: **94 decoded, 0 displayed**, 0.8 MB received |
| Host fipsd restart during that stream | Stream lost; client entered reconnect |
| Host container restart after the loss | New session and TCP connection, but decoded-frame recovery gate FAIL |
| Shutdown during failed renegotiation | 3 FAIL: SIGKILL/137 after the 10 s Podman grace, no summary |
| Exactly-once retained history checks | 2 FAIL after automatic reconnect attempts added extra request IDs; checks intentionally not weakened |

A real unpatched end-to-end receipt (one session, not a sustained reliability
claim):

```text
host: encoder ready (x264, 1920x1080)
client: session accepted (id=b1748a75-efdb-41bc-f52c-1aa97951fef1)
INFO: stream: FIPS resolved npub1u6fm0mv... → fd10:364b:9b33:368a:1ab:9ab2:840b:5e09
INFO: stream: accepted connection from fd43:ca6e:fe5d:7329:d078:af15:af03:7e5f
client: receiving stream... (Ctrl+C to stop)
client: packets=94 decoded=94 displayed=0 decode_avg=1.3ms rtt=0ms rx=0.8MB
```

### Blocking product defects (already filed; not fixed by this lane)

- **md-jjbk (P1)**: `src/core/stream.c:155–208,695–738` uses a whole-read
  deadline with a stack-local partial byte count. Header/payload bytes consumed
  before timeout are discarded, but timeout leaves the stream connected. The
  next call parses mid-packet bytes as a header. The 16 ms client receive poll
  exposes this on slower mesh delivery. This needs resumable partial-packet
  state, including TLS, not larger test deadlines or lower coverage.
- **md-2cf9 (P2)**: client `--npub` dials once after link-level readiness
  (`src/client/main.c:456–460`); startup/restart link flaps can temporarily
  remove the route. Stale established-session fallback can also pass readiness
  without a usable link. The reference's explicitly diagnostic raw TCP path
  gate retries bounded transport connects before starting each fresh session;
  it is **not** a product recovery fix or Nostr completion signal.
- **md-yotd (P2)**: `host_on_dm` latches `session_requested` after one accepted
  request (`src/host/main.c:132,175`). A disconnected client negotiates new
  sessions that this still-running host will not answer. Recovery currently
  requires a host-process restart as well as transport recovery.
- **md-7gqk (P3)**: negotiation does not observe SIGTERM promptly while waiting
  for session_accept. Keep the existing <5 s assertion; do not extend grace.

A VM-only **scratch** build changed receive timing so an in-progress packet
finished before returning (bounded by a separate 10 s packet deadline). That
run produced **116 PASS, 0 FAIL, 17 INFO**, 274.5 s: initial/restarted sessions
100/96/92 decoded frames; midstream 95 → 161 across daemon restart, then
host-restart recovery. Images were `md-l3-video:scratch` (`e114e697ec45…`) and
`md-l3-client:scratch-decode` (`76219df78178…`). This isolates the framing defect,
but is **not acceptance**, not a reviewed production fix, and not part of the
committed source. Both scratch and unpatched images are removed in cleanup.

## Reproduce (expect nonzero until the product defects are fixed)

Use a fresh task-owned directory on matching Ubuntu 24.04/arm64, with system
rootless Podman, TUN and the isolated render-node grant available. Keep all
names task-prefixed; do not use or modify another lane's checkout. Build the
host/decode images and fixture binaries as in
[client-validation.md §Reproduce](client-validation.md#reproduce), but use
`nostrc-master` at **6f7a8d025 or newer**, not the historical `~/nostrc` example.
The producer reads `NOSTRC_ROOT/_build`. Include `-Dclient=true
-Dsigner_nip5f=true -Dsigner_nip46=true`; no GUI image is needed.

Build the separate daemon image from the exact nostrc archive named in
`Dockerfile.mesh` (not from metadesk's nostrc library build):

```sh
# In your dedicated nostrc source checkout, export only; do not change its HEAD.
mkdir -p FIPSD_CONTEXT
git archive 5d09cab9183a09a0b0b88d89c20495b55beb2a50 | tar -x -C FIPSD_CONTEXT
podman build --layers -f SRC/deploy/containers/Dockerfile.mesh \
  --build-arg BASE=docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8 \
  -t localhost/md-l3-mesh:5d09cab FIPSD_CONTEXT
```

Populate a **new** private WORK directory with `mesh-peer.py`,
`client-fixture.py`, `tests/account_startup_fixture.py`, `bin/nip5f-crypto`,
`bin/nip46-bunker`. Keep `validate-client.py` alongside
`validate-mesh-video.py` (the latter imports its shared helpers). Then:

```sh
python3 -B SRC/deploy/containers/reference/validate-mesh-video.py --work WORK \
  --prefix md-l3-video-session --video-image localhost/md-l3-video:REV \
  --decode-image localhost/md-l3-client:REV-decode \
  --mesh-image localhost/md-l3-mesh:5d09cab
```

Default frame gate is 60; keep it. The driver waits on log events/conditions,
retains the same relay across fresh sessions and checks correlation after logs
reach EOF. Between sessions it restarts each daemon, proving its holder did
not change; the final cell separately disrupts a live stream and restarts the
host. Deadlines bound failure, never declare success. It removes created
containers, per-node volumes and its network in `finally`, and writes
`WORK/evidence.json` (logs/results/public identities, no private keys). Preserve
that receipt privately before deleting WORK and its throwaway key files.

Local reference tests: **28/28 PASS, 0 skipped**, normally and under `-O`,
on both macOS and the Ubuntu VM. Coverage includes real WebSocket
EVENT/OK/REQ/EOSE/CLOSE/CLOSED, ephemeral boundaries, live recipient filters,
>256 ephemeral events, cleanup failures retaining ownership, and restart-log
boundaries:

```sh
python3 -B -m unittest discover -s deploy/containers/reference -p 'test_*.py'
python3 -B -O -m unittest discover -s deploy/containers/reference -p 'test_*.py'
```

## Nostr Protocol Review — diff-scoped, A–J

No new production relay path. Shared fixture mode defaults remain unchanged;
`--no-store-ephemeral` only changes this long-run private relay to forward kinds
20000–29999 live without filling its 256 stored-event budget. Session giftwraps
and grants still retain history. It ACKs ephemeral publishes with OK and uses
the same relay-side filters; REQ history ends at EOSE and CLOSE/CLOSED behavior
is inherited/tested. The fixture intentionally delegates crypto verification
to the real nostrc consumers; it is not a production relay implementation.

History is not cleared between sessions. The driver keeps the existing
request/session correlation and exactly-once checks, so extra renegotiations
produce failures rather than being silently filtered out. Extra history IDs
after connection loss are new reconnect requests, not evidence that deduping an
identical event regressed. The output damage
generator and raw TCP bootstrap retry are not relay polling or sleep-based
history completion. Private single-relay/no-AUTH fixture evidence is **not**
NIP-42, public-relay discovery, multi-relay or live production qualification.

## Cleanup receipt — 2026-11-21

After preserving private result/log receipts locally, the final sweep removed
`md-l3-czf-p5`, its `md-l3-czf-store` volume, and **38/38 task image IDs**
(including untagged build stages identified from the two task build logs).
The outer test container needed SIGKILL after its 5 s stop grace; it was removed
successfully. All inner Podman state disappeared with its dedicated store.

Verified **zero md-l3 containers, pods, networks, volumes or tagged images**;
all 38 recorded image IDs absent. `~/md-l3` (keys, bundles, static Podman,
API/runtime/storage files and source copies) and any user-local Podman binary
are absent. The pre-existing Ubuntu base `50ec65424811` was preserved;
`/usr/bin/podman` remains **4.9.3**. No global image/system prune was used.
The ignored local `__pycache__` was also removed, and is not committed.

## Parent spike disposition

As read for this lane, md-ah9's only open children are **md-ah9.6** (this
reference; product defects above block acceptance) and **md-ah9.7** (deferred
Kubernetes Tier B Pod; restricted-Pod-Security exceptions, probes and shutdown
still need qualification). All other numbered children are closed in bd.
The design deliverable exists at `docs/plans/container-runtime-2026-11-14.md`;
its original P0/“nothing validated yet” status text is historical and should be
reconciled with the landed references and closed children, not read as current
absence of all artifacts. The original spike also mentions systemd-nspawn;
that runtime needs explicit qualification or a scope disposition, not an
inference from Podman/Docker receipts. md-ah9 remains open. md-czf can close on
its measured negative result; it does not unblock reliable video recovery.
