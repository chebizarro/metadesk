# FIPS Spec Update: Plan

## Goal
Update metadesk to align with the current FIPS v0.3.x-era protocol/runtime surface: FIPS owns overlay discovery, NAT traversal, STUN/TURN, gateway behavior, and platform packaging; metadesk keeps its application-layer session negotiation and final TCP-over-FIPS stream path.

This is a migration plan, not an implementation patch. The first implementation pass should target Linux + macOS, treat `fips-gateway` as optional Linux-only deployment infrastructure, and leave Windows/OpenWrt support as follow-up documentation/research.

## Background
- Current FIPS release baseline is v0.3.0, released 2026-05-11. It is wire-compatible with v0.2.x and adds Nostr overlay discovery, STUN-assisted UDP NAT traversal, `fips-gateway`, ACLs, BLE experimental transport, platform packaging, and DNS resolver changes without requiring a flag-day upgrade (`fips/docs/releases/release-notes-v0.3.0.md:1`, `fips/docs/releases/release-notes-v0.3.0.md:5`, `fips/docs/releases/release-notes-v0.3.0.md:13`, `fips/docs/releases/release-notes-v0.3.0.md:43`).
- The FIPS Nostr surface now uses three event kinds: kind `37195` overlay adverts, kind `21059` traversal signaling, and kind `10050` NIP-17 inbox relay lists (`fips/docs/reference/nostr-events.md:9`). Overlay adverts use `d=fips-overlay-v1`, include endpoint lists, optional `signalRelays`, and optional `stunServers` (`fips/docs/reference/nostr-events.md:27`, `fips/docs/reference/nostr-events.md:45`, `fips/docs/reference/nostr-events.md:62`).
- FIPS traversal signaling is NIP-59/NIP-44-shaped but intentionally uses FIPS-specific ephemeral kind `21059`; ordinary NIP-59 gift wraps remain kind `1059` in the NIPs. The FIPS docs describe the kind `21059` rumor/seal/wrap structure and traversal payloads (`fips/docs/reference/nostr-events.md:84`, `fips/docs/reference/nostr-events.md:93`, `fips/docs/reference/nostr-events.md:108`).
- metadesk currently publishes FIPS transport information as kind `30078` with `d=fips-transport`, where event content is the derived FIPS IPv6 address (`metadesk/src/core/nostr.c:1175`, `metadesk/src/core/nostr.c:1187`, `metadesk/src/core/nostr.c:1194`). It subscribes to the same kind/tag for a host (`metadesk/src/core/nostr.c:1235`).
- metadesk’s embedded `fips-nat` publishes STUN-discovered endpoint JSON by reusing `md_nostr_publish_transport()`, so the NAT JSON is still sent through kind `30078`, `d=fips-transport`, despite comments/TODOs pointing toward a separate `fips-nat-endpoint` tag (`metadesk/src/fips-nat/publish.c:1`, `metadesk/src/fips-nat/publish.c:118`). This overlaps with, and is superseded by, current FIPS overlay adverts and traversal signaling.
- metadesk derives FIPS IPv6 addresses locally as `0xfd || SHA-256(pubkey)[0..15]` and falls back to direct computation if `.fips` DNS resolution fails (`metadesk/src/core/fips_addr.c:185`, `metadesk/src/core/fips_addr.c:203`, `metadesk/src/core/fips_addr.c:260`, `metadesk/src/core/fips_addr.c:307`). Current FIPS derives `NodeAddr` as the first 16 bytes of SHA-256(pubkey), then constructs `FipsAddress` by prefixing `0xfd` and using the first 15 node-address bytes (`fips/src/identity/node_addr.rs:34`, `fips/src/identity/address.rs:36`). The derivation is compatible in shape but must be cross-tested against FIPS outputs.
- metadesk’s connection path still treats FIPS as a TUN-routed TCP substrate: `md_stream_connect_fips()` resolves `npub1…` to IPv6 and calls the generic TCP connect (`metadesk/src/core/stream.c:483`, `metadesk/src/core/stream.c:490`, `metadesk/src/core/stream.c:501`). Host startup derives/publishes its IPv6 address and then talks to `fips-nat` only via status IPC (`metadesk/src/host/main.c:421`, `metadesk/src/host/main.c:515`). The current client does not use the subscribed kind `30078` content as the actual connect target; it recomputes/resolves from the host npub before dialing, so the legacy transport event is primarily a gating/readiness signal rather than the transport input.
- Current FIPS has its own discovery and traversal runtime: `OverlayAdvert`, `OverlayEndpointAdvert`, `TraversalOffer`, and `TraversalAnswer` are first-class types (`fips/src/discovery/nostr/types.rs:81`, `fips/src/discovery/nostr/types.rs:88`, `fips/src/discovery/nostr/types.rs:115`, `fips/src/discovery/nostr/types.rs:139`). The daemon can also accept control-socket `connect` requests with `{npub,address,transport}` (`fips/src/control/commands.rs:18`, `fips/src/control/commands.rs:23`).
- Build/runtime packaging currently keeps `fips-nat` as an optional metadesk executable enabled by default and requiring `libnice`; host links `fipsnat_ipc.c` and `stun.c`, while the `fips-nat` binary compiles STUN, publish, punch, TURN, and IPC sources (`metadesk/meson.build:215`, `metadesk/meson.build:344`, `metadesk/meson.build:365`). This should be revisited now that FIPS owns discovery, STUN, NAT traversal, gateway, and platform packaging.
- Prior metadesk docs describe FIPS-native remote desktop, `fips-nat` as a companion daemon, TCP over FIPS TUN on port `7700`, UDP over FIPS reserved on `7701`, and FIPS daemon transport on `2121`. `docs/metadesk-spec-phase-2.1.md` planned kind `30078`/`d=fips-transport` before the current FIPS overlay-discovery design.

## Recommendation
Make the FIPS daemon the authoritative owner of discovery and reachability. metadesk should stop publishing or consuming FIPS network-discovery events directly, stop maintaining its own NAT sidecar as the recommended path, and instead integrate with the local FIPS daemon through its control socket and existing TUN/DNS behavior.

The target ownership split is:

| Area | Owner after migration | metadesk role |
| --- | --- | --- |
| Overlay adverts (`37195`) | FIPS daemon | Do not publish/parse for bootstrap in first pass |
| Traversal signaling (`21059`) | FIPS daemon | Do not implement or wrap directly |
| Inbox relay list (`10050`) | FIPS daemon / NIP-17 tooling | Keep metadesk session DMs separate |
| STUN, UDP hole punch, TURN fallback | FIPS daemon | Remove from recommended metadesk runtime path |
| FIPS control/readiness | FIPS daemon API | Add thin metadesk control client |
| FIPS address derivation / `.fips` DNS | FIPS daemon + deterministic compatibility | Keep local helper, validate it |
| Remote desktop session approval/accept | metadesk | Keep current application-layer session semantics |
| TCP stream over FIPS TUN | metadesk over FIPS route | Keep as final dial step only |
| `fips-gateway` | Operator-managed FIPS service | Document as optional Linux-only infrastructure |

## Target Architecture
The migrated runtime should look like this:

1. Operator installs/configures a current FIPS daemon.
2. FIPS daemon handles relay discovery, overlay adverts, STUN, traversal signaling, NAT punching, retry/cooldown, ACLs, and mesh route maintenance.
3. metadesk host/client open a local FIPS control client at startup to verify daemon health and inspect peer/path readiness. For Linux/macOS, socket discovery should follow FIPS’s documented client-side default order unless a metadesk config override is provided: `/run/fips/control.sock`, then `$XDG_RUNTIME_DIR/fips/control.sock`, then `/tmp/fips-control.sock` (`fips/docs/reference/control-socket.md:7`).
4. metadesk keeps its own Nostr session request/accept flow for application approval and capability negotiation.
5. Before opening the remote desktop stream, metadesk verifies that the target npub is reachable or in-progress through the local FIPS daemon, then uses `md_stream_connect_fips()` as the final TCP connect over the FIPS-managed route.

This deliberately avoids recreating FIPS inside metadesk. The plan should not require metadesk to parse `OverlayAdvert` or construct `TraversalOffer`/`TraversalAnswer` in the first pass; those formats are evidence for deprecating the legacy path, not new metadesk-owned protocol work.

## Resolved Decisions
- **Deprecate `fips-nat`, but do not delete it in the first implementation patch.** Freeze the code, remove it from the recommended startup path, disable it by default after the daemon-control path lands, and delete it after one stability window.
- **Prefer the FIPS control socket over direct FIPS advert parsing.** metadesk should use control queries for daemon readiness and peer observability. Direct parsing of kind `37195` is only a later UX enhancement if control data proves insufficient.
- **First-pass platform scope is Linux + macOS.** Both are relevant to metadesk and align with Unix-socket FIPS control behavior. Gateway remains Linux-only. Windows/OpenWrt should stay informational until a dedicated platform plan handles their control/runtime differences.
- **Do not block on new upstream FIPS APIs.** Current FIPS `connect` requires `{npub,address,transport}`. The first pass should assume peers are configured in FIPS (`via_nostr`, `auto_connect`, or equivalent operator config) and use control state/readiness rather than assuming an npub-only connect command exists. Ad-hoc `--npub` to a peer that is not configured or discoverable by the local FIPS daemon is out of scope for the first migration pass; metadesk should fail clearly and point operators to FIPS peer/discovery configuration.

## Keep / Replace / Remove Matrix
| Current surface | Decision | Notes |
| --- | --- | --- |
| `md_nostr_publish_transport()` / `md_nostr_subscribe_transport()` (`src/core/nostr.c`) | Replace in FIPS bootstrap | Keep only if still needed for a backward-compatible transition flag; do not use as primary FIPS discovery. |
| `src/fips-nat/publish.c` NAT JSON on kind `30078` | Deprecate, then remove | Superseded by FIPS overlay adverts and traversal runtime. |
| `src/fips-nat/stun.c`, `punch.c`, `turn.c` | Deprecate, then remove | Duplicates FIPS v0.3.x runtime behavior. |
| `src/fips-nat/fipsnat_ipc.*` and host IPC status path | Replace | New control client should target the FIPS daemon, not metadesk’s sidecar. |
| `src/core/fips_addr.c` | Keep and validate | Deterministic address helper remains useful as DNS fallback, not as reachability proof. |
| `md_stream_connect_fips()` (`src/core/stream.c`) | Keep, reposition | Final TCP dial helper after daemon-managed reachability is ready. |
| metadesk Nostr session request/accept | Keep | Application-layer approval/capability negotiation is separate from FIPS discovery. |
| `meson.build` `fips_nat` default | Change later | Move from enabled-by-default to legacy/off-by-default after the replacement path is stable. |
| `docs/metadesk-spec-phase-2.1.md` | Mark stale/superseded | Its kind `30078` assumptions predate the current FIPS spec. |

## Work Items

### Item 1 — Add a thin FIPS control client seam
**Goal:** Introduce a small reusable metadesk core module for one-shot FIPS control socket requests on Linux/macOS.

**Done when:** Host and client code can discover the Linux/macOS control socket using FIPS’s documented default path order plus an explicit metadesk override, issue line-delimited JSON control requests, parse status/error responses, set timeouts, and report daemon-unavailable vs daemon-error distinctly without depending on `fips-nat` IPC.

**Key files:** `metadesk/src/core/`, `metadesk/src/host/main.c:515`, `fips/docs/reference/control-socket.md`, `fips/src/control/protocol.rs`, `fips/src/control/commands.rs:18`.

**Dependencies:** None; this is the new integration seam.

**Size:** Medium.

### Item 2 — Validate address derivation compatibility
**Goal:** Prove metadesk’s local FIPS address derivation matches current FIPS identity/address rules before other work assumes `md_stream_connect_fips()` reaches the same mesh address FIPS would derive.

**Done when:** Tests compare `md_fips_addr_from_pubkey_hex()` and `md_fips_addr_from_npub()` against FIPS-derived outputs for representative keys; documentation states that local derivation is a deterministic fallback, not a discovery/reachability mechanism.

**Key files:** `metadesk/src/core/fips_addr.c:185`, `metadesk/src/core/fips_addr.c:260`, `fips/src/identity/node_addr.rs:34`, `fips/src/identity/address.rs:36`, `metadesk/meson.build:444`.

**Dependencies:** Item 1 can proceed in parallel, but Items 3–4 should not rely on final dial behavior until this compatibility check passes.

**Size:** Small.

### Item 3 — Define daemon readiness and peer-readiness checks
**Goal:** Specify and implement the control queries metadesk uses before attempting a FIPS stream connection.

**Done when:** The `--npub` path checks daemon health first, then performs a bounded poll for peer/path readiness before `md_stream_connect_fips()`. A first-pass “dial-safe” predicate should treat a matching peer as ready only when control data shows an active/usable peer or session: for example, matching `npub` in `show_peers` with connected/active `connectivity` and a non-empty `link_id`, or matching `npub` in `show_sessions` with `state=established` if `show_peers` does not expose mid-traversal state. If no matching peer appears, report “peer not configured or not discovered by local FIPS daemon” and point to FIPS peer/discovery config; if a peer appears without a usable link/session, report “route still converging” until the bounded retry expires.

**Key files:** `metadesk/src/client/main.c`, `metadesk/src/host/main.c`, `fips/docs/reference/control-socket.md:108`, `fips/docs/reference/control-socket.md:113`, `metadesk/src/core/stream.c:483`.

**Dependencies:** Items 1–2.

**Size:** Medium.

### Item 4 — Rework client and host bootstrap around FIPS-owned discovery
**Goal:** Remove kind `30078` transport advert publication/subscription from the primary FIPS session bootstrap.

**Done when:** The host no longer publishes its FIPS IPv6 address as the primary reachability signal, the client no longer blocks on `on_transport` before session negotiation, and both sides rely on FIPS daemon readiness plus metadesk’s existing session request/accept for application authorization.

**Key files:** `metadesk/src/core/nostr.c:1175`, `metadesk/src/core/nostr.c:1235`, `metadesk/src/core/nostr.h`, `metadesk/src/client/main.c`, `metadesk/src/host/main.c:421`.

**Dependencies:** Items 1–3.

**Size:** Large.

### Item 5 — Deprecate `fips-nat` and legacy NAT signaling
**Goal:** Freeze metadesk’s duplicated STUN/punch/TURN implementation and move it out of the supported runtime path.

**Done when:** `fips-nat` is marked legacy in docs/help text, host startup no longer suggests it as the default fix, kind `30078` NAT endpoint JSON is no longer part of the recommended path, and build options are prepared to flip `fips_nat` off by default after the replacement path is verified.

**Key files:** `metadesk/src/fips-nat/*`, `metadesk/src/fips-nat/publish.c:118`, `metadesk/src/host/main.c:515`, `metadesk/meson.build:215`, `metadesk/meson.build:365`.

**Dependencies:** Items 1–3 should provide the replacement path first.

**Size:** Medium.

### Item 6 — Update packaging and operator documentation for FIPS as an external runtime
**Goal:** Document metadesk’s expected FIPS runtime dependency instead of shipping/advertising a metadesk-owned NAT sidecar as the integration path.

**Done when:** Linux and macOS setup docs identify the required FIPS daemon version/capabilities, control socket expectations, relay/discovery configuration, and how metadesk reports readiness failures. `fips_nat` is no longer enabled by default in the documented build path once Item 5 lands. The same docs distinguish normal FIPS-node metadesk sessions from operator-managed `fips-gateway` LAN bridge deployments; gateway is documented as Linux-only, external, and FIPS-owned, with no metadesk control of gateway lifecycle or nftables/proxy-NDP assumptions.

**Key files:** `metadesk/README.md`, `metadesk/docs/metadesk-spec.md`, `metadesk/docs/metadesk-spec-phase-2.1.md`, `metadesk/meson_options.txt`, `metadesk/meson.build:215`, `fips/docs/releases/release-notes-v0.3.0.md:1`, `fips/docs/design/fips-gateway.md`, `fips/docs/reference/cli-fips-gateway.md`.

**Dependencies:** Items 1–5.

**Size:** Medium.

## Migration Order
1. Add the FIPS control client seam, including socket path discovery.
2. Validate address derivation compatibility before depending on final dial behavior.
3. Add daemon/peer readiness checks and diagnostics.
4. Rework host/client bootstrap so metadesk no longer depends on `kind:30078` for FIPS reachability.
5. Mark `fips-nat` legacy and remove it from the recommended runtime path.
6. Update docs/build defaults for FIPS as an external runtime dependency, including gateway positioning.
7. File deferred issues for npub-only FIPS control connect, richer readiness data, Windows control transport support, optional direct overlay-advert UX, and eventual deletion of legacy `src/fips-nat/*`.

## Risks and Mitigations
- **Control API gap:** Current FIPS control `connect` requires `{npub,address,transport}` rather than npub-only discovery. Mitigate by requiring peers to be configured/discoverable in FIPS for the first pass, treating npub-only connect as follow-up, and making ad-hoc unconfigured `--npub` fail with setup guidance rather than a misleading network timeout.
- **Readiness races:** FIPS discovery can lag daemon startup. Mitigate with bounded polling and diagnostic states instead of a single control snapshot.
- **Temporary dual-path confusion:** `fips-nat` will remain in-tree during transition. Mitigate by marking it legacy immediately and removing it from docs/defaults before deleting code.
- **Gateway scope creep:** `fips-gateway` can look like a tempting metadesk feature. Mitigate by documenting it as external Linux-only infrastructure and keeping gateway lifecycle out of metadesk.
- **Address/reachability conflation:** A computed FIPS address does not prove a route exists. Mitigate by updating comments/docs and placing control readiness before `md_stream_connect_fips()`.

## Validation and Acceptance Criteria
- metadesk no longer depends on kind `30078`, `d=fips-transport` as the primary FIPS bootstrap mechanism.
- `fips-nat` is no longer in the recommended runtime path and has a documented deprecation/removal sequence.
- Linux + macOS users have documented expectations for the external FIPS daemon and control socket.
- `md_stream_connect_fips()` is documented and used as a final TCP dial over an existing daemon-managed FIPS route.
- Address derivation tests demonstrate compatibility with current FIPS identity/address rules.
- Gateway documentation is accurate: optional, Linux-only, FIPS-owned, and not embedded in metadesk.

## Open Questions
None blocking for the first implementation pass. The remaining uncertainties are tracked as deferred work: npub-only FIPS control connect, richer readiness data if `show_peers`/`show_sessions` are insufficient for convergence diagnostics, Windows support, optional direct overlay-advert UX, and eventual deletion timing for `src/fips-nat/*`.

## References
- `fips/docs/releases/release-notes-v0.3.0.md`
- `fips/docs/reference/nostr-events.md`
- `fips/docs/reference/control-socket.md`
- `fips/docs/reference/configuration.md`
- `fips/docs/design/fips-nostr-discovery.md`
- `fips/docs/design/fips-gateway.md`
- `metadesk/src/core/nostr.c`
- `metadesk/src/fips-nat/publish.c`
- `metadesk/src/core/fips_addr.c`
- `metadesk/src/core/stream.c`
- `metadesk/src/host/main.c`
- `metadesk/src/client/main.c`
- `metadesk/meson.build`
