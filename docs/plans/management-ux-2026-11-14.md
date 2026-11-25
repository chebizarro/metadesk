# Host/Client Management UX — Design Specification and Phased Plan

**Spike:** `md-70v` · **Deliverable:** this design document + ready-to-file work items; no product implementation.

The `## Background` under §2 is the curated Phase-2 evidence; later sections distinguish current implementation from proposed behavior. The §6 work items are ready to file as spin-offs from `md-70v`; the execution index below summarizes them.

## 1. Summary

Introduce a UI-independent C management layer, a Linux-first GTK4/libadwaita companion application, and an authoritative host approval service. Keep SDL2/ImGui responsible for remote-session presentation and controls—not identity administration. Delegate durable key custody to external signers, implement reusable NIP-78 support in nostrc, and use encrypted NIP-51 lists for host collections and permissions, encrypted NIP-78 documents for application metadata, and explicit local configuration for operational state. Replace implicit relay defaults and automatic grants with discoverable relay routing, correlated session requests, capability enforcement, and fail-closed authorization.

## 1.1 Execution index

Work items to file as spin-offs of `md-70v` (details + verification in §6; file/impact in §4). "Size" is rough: S ≤ a few days, M ≈ 1–2 weeks, L ≈ 3+ weeks.

Corrected against the 2026-11-14 design review (`docs/reviews/management-ux-plan-critique-2026-11-14.md`); see §7 for the corrections and open decisions those IDs reference. **Status and filed bead IDs as of 2026-11-21: §8.1** (several items have shipped; P3.8 was superseded by `md-mrw`/`md-6ns`).

| # | Goal | Done when | Key files | Depends on | Size |
|---|------|-----------|-----------|------------|------|
| P0 | Land this spec + corrected contracts | Plan + spec updated; four flows trace cleanly; deliverable-location + bead cross-links recorded (§7) | `docs/plans/management-ux-2026-11-14.md`, `docs/metadesk-spec.md` | — | S |
| P1.1 | Add missing nostrc pool capabilities | **Per-subscription handles + public relay enumeration + in-flight publish cancellation** (EOSE/CLOSED observer and `publish_and_wait` already exist); use `set_recovery_window` for reconnect `since`; sanitizer-clean | nostrc `libnostr/include/nostr-simple-pool.h`, `nostr-relay.h` | — | M |
| P1.2 | Signer-independent unsigned builders (NIP-51 / NIP-78 / NIP-17-10050) | Unsigned builders + private-entry plaintext seams for all three; raw-key wrappers kept; follow `libporthome/head.h` unsigned-30078 precedent; reuse existing `NOSTR_KIND_DM_RELAY_LIST`/`_APPLICATION_SPECIFIC_DATA` | nostrc `nips/nip51`, `nips/nip17`, new `nips/nip78` | — | M |
| P1.3 | Make Grotto Control1 adapter consumable | Existing consumers still build; adapter installable or extracted to `gnome/grotto-control-client/` | nostrc `apps/grotto` adapter, build files | — | S |
| P2.4 | Encrypted management store + replacement/conflict | Deterministic ordering, owner-separated cache/outbox, safety latch; no plaintext/secrets on disk | `src/core/managed_store.c/.h` | P1.2 | M |
| P2.5 | Canonical host resolution + NIP-65/NIP-17 routing | NIP-05/19/65 linked; **metadesk-owned NIP-05 HTTPS fetch** (`nostr_nip05_resolve_from_json`; scrub `NIP05_ALLOW_INSECURE`/`NIP05_TIMEOUT_MS`); 10050 parse linked + unsigned builder (P1.2); pin-mismatch + unsafe-URL rejection; registry kind constants | `src/core/identity_resolver.*`, `relay_plan.*`; `meson.build` | P1.1 | M |
| P2.6 | Harden signer/bridge concurrency, DM auth, teardown | Serialized signer, live key-pin check, bounded unwrap, named NIP-17/59 validation (§7); **NIP-42 AUTH→MdSigner bridge off the reader thread**; safe shutdown | `src/core/nostr.c/.h`, `signer.c/.h` | P1.1 | M |
| P2.7 | **Signer accounts + user-assigned roles** (§7.6) | Account list over `MdSigner` backends (55L/5F/46); host + client role bindings; NIP-46 credentials in Secret Service/Keychain; role switch revokes generation; CLI binding for headless hosts | `src/core/accounts.c/.h` (new), `signer.c/.h`, `host/main.c`, `tests/test_accounts.c` | P2.6 | M |
| P3.7 | Host approval policy + capability grants + **headless admin CLI** | `MdHostAccess` state machine, load states, deadlines, no-UI denial; `metadesk-host access upsert/remove <npub> <caps>` for headless/macOS hosts; pure-SM tests pass | `src/core/host_access.c/.h`, `src/host/main.c` | P2.4, P2.6, P2.7, P3.8a | M |
| P3.8a | Session-v2 **types/parsers** | `request_id`/`stream_token`/deadlines/bind types in `session.c`; strict validation; bind payload sized to the 8 KiB limit (not `json_buf[4096]`) | `src/core/session.c/.h` | P2.5 | M |
| P3.8b | Session-v2 **entrypoint adoption** + one-use binding (**atomic**) | CSPRNG tokens, candidate-stream gate; no video/tree/input before bind; capability gates incl. MCP dispatch path | `src/core/stream.c/.h`, `host/main.c`, `client/main.c`, `mcp_server.c` | P3.7, P3.8a | L |
| P3.9 | Same-user host IPC + private client launch | **New** `SO_PEERCRED`/`getpeereid` peer-cred check (not "reuse"); socket-layout decision vs existing `metadesk-<name>.sock`; inherited FD; no creds in argv | `src/core/management_ipc.*`, `ipc_unix.c`, `client/main.c` | P3.7 | M |
| P4.10 | Linux management shell + signer onboarding | Adaptive nav, **signer account list + host/client role picker** (§7.6), Grotto identity panel, explicit relays; reuse grotto-panels `tests/fake-service.c` for GTK smoke; no GTK in host/core/client-only builds | `src/manager/*`, `meson_options.txt`, `meson.build` | P2.*, P3.9 | L |
| P4.11a | Host collections + connect flow (**ships on session v1**) | Hosts/favorites/tags, pin review, save/import/export, QR, client launch; keyboard-only + 360px + offline | `src/manager/md-page-{hosts,identity,relays}.c`, `md-client-launch.*` | P4.10, P2.* | M |
| P4.11b | Approval inbox + scoped grants UI | Scoped inbox, grant display, notification withdraw on expiry/cancel | `src/manager/md-page-{host,access}.c`, `md-approval.*` | P4.10, P3.7, P3.8b, P3.9 | M |
| P4.12 | Responsive connect/reconnect in SDL overlay | Async progress, explicit actions, input consumption, texture reset, grant display | `src/client/ui.cpp/.h`, `render.c/.h` | P3.8b | M |
| P5.13 | Migrate legacy public allowlists | Source event preserved; encrypted replacement; disclosure explained; mixed-visibility handled | `src/core/host_access.*`, `managed_store.*` | P3.7 | S |
| P5.14 | Management release matrix + regression gate | Linux manager + CLI/overlay + `client=false` + signer combos; failure/partition/cleanup exercised | `tests/*`, CI | P4.* | M |

Cross-link spin-offs to existing beads with `bd dep` rather than duplicating: **md-3nv** (reconnect reliability) and **md-d9y** (client reconnection) overlap P3.8b/P4.12; **md-aqf** (test-suite fake-behavior overhaul) overlaps §3.14's "no 'pool initialization skipped' tests."

## 2. Current-state analysis

## Background (Phase 2 findings — load-bearing evidence)

### Current surface is CLI + an in-session overlay only
- Client/host are configured purely by CLI args: npub/IP, `--relay` (×16),
  signer flags. Client `src/client/main.c` usage 130–146, parse 560–592; host
  `src/host/main.c` usage 446–467, parse 580–610. Default relay
  `MD_DEFAULT_RELAY = "wss://relay.sharegap.net"` (`src/core/nostr.h:45`).
- Signer abstraction `MdSigner` with backends direct / NIP-46 bunker / NIP-55L
  (D-Bus) / NIP-5F (unix socket), constructors + `md_signer_auto_detect`
  (`src/core/signer.h:105–163`; auto-detect tries 5F then 55L, not 46/direct,
  `src/core/signer.c:756–765`). Direct key is mlock'd (`signer.c:184–192`).
- Secrets: 1Password Connect `op://vault/item/field` client exists
  (`src/core/secrets.c`), but is **not wired into** host/client startup — it's
  an available core seam only.
- Nostr bridge `MdNostrConfig`/`MdNostrCallbacks` owns the relay pool + DM +
  allowlist state (`src/core/nostr.c:52–68, 442–461`). Session negotiation:
  client builds `MdSessionRequest` → `md_nostr_send_session_request`
  (`main.c:225–245`); host `host_on_dm` validates sender vs expected client pk
  and (if present) the allowlist (`host/main.c:114–147`), then auto-grants caps
  `VIDEO|AGENT|INPUT` and sends `session_accept` (`host/main.c:748–827`) — **no
  interactive approval prompt today**.
- NIP-51 allowlist: host subscribes to its own kind-30000 list id
  `metadesk-allowlist`, signature-validated, cached; `md_nostr_is_allowed`
  matches `p` tags (`nostr.c:348–378, 757–800`). Refresh is async; before the
  first list arrives `has_allowlist()` is false and the conditional rejection
  doesn't apply (startup-window warning).
- Session state machine + JSON wire in `src/core/session.c:18–51, 159–331`.

### Nostr primitives: what nostrc already provides vs. must be built
(from nostrc at `/Users/bizarro/Documents/Projects/nostrc`)
- **Implemented & linked by metadesk:** NIP-44 (`nips/nip44/.../nip44.h`),
  NIP-51 (`nips/nip51/.../nip51.h`, incl. private-entry encryption), relay
  pub/sub (`libnostr/include/nostr-relay.h` publish/publish_and_wait,
  `nostr-simple-pool.h` subscribe) — metadesk already uses these in
  `src/core/nostr.c`.
- **Implemented in nostrc but NOT linked by metadesk:** NIP-05
  (`nips/nip05/include/nip05.h` — `nostr_nip05_parse_identifier/lookup/validate`
  for user@domain→pubkey), NIP-65 (`nips/nip65/.../nip65.h` kind-10002 relay
  lists, `nostr_nip65_get_write_relays`; library parses/builds, is not itself an
  outbox router), NIP-19 (`nips/nip19/.../nip19.h` npub/nsec/nprofile/naddr).
- **Optional signer libs (build-gated):** NIP-46/55L/5F (`meson_options.txt:5–12`;
  `meson.build:297–323`).
- **MISSING from nostrc → must be built:** NIP-78 (kind 30078 application data).
  No implementation/header exists; only doc/issue references.

### UI surface today, and reusable shells nearby
- metadesk has **no management GUI** — only the ImGui + SDL2 + SDL_Renderer2
  client overlay (`src/client/ui.cpp`: a ~260pt stats panel with status /
  latency / FPS / Disconnect, plus an F1 hint; `ui.h` opaque `MdOverlay`).
  Single SDL event pump forwards to ImGui then to remote-input
  (`render.c:209–251`, `main.c:411–465`). Cross-platform.
- **Reusable in nostrc (GTK4/libadwaita, Linux/GNOME):**
  - **Grotto Panels** `nostrc/gnome/grotto-panels/` — an *installed shared
    library* (`grotto-panels-0`, CMake `GrottoPanels::Panels`) exposing
    **Identity, Connected Apps, Activity, Remote Signing** panels + an identity
    picker, each `AdwNavigationPage` with an injected service/model interface
    (`include/grotto-panels-0/grotto-panels.h:148–332`). Not just example code.
  - **nostr-settings** `nostrc/gnome/nostr-settings/` — adaptive libadwaita
    window hosting those 4 panels + **Relays (NIP-65 edit/publish, NIP-11)**,
    Notifications, Wallet, Media, Files (`src/ui/nss-window.c`,
    `nss-page-relays.c:633–824`). Delegates trusted security ops to Grotto.
  - **Grotto** app `nostrc/apps/grotto/` — the trusted signer/approval host:
    identity create/import/backup, pending-approval inbox, permissions, runs the
    NIP-55L local service + NIP-46 bunker.
  - **gnostr** `nostrc/apps/gnostr/` — NIP-51 mute/list picker dialog + relay
    manager (app-owned, reference). **nostr-gtk** — content widgets, not
    identity/relay management.

### Prior art / spec
- No `docs/plans` or `docs/designs` yet. Spec is `docs/metadesk-spec.md`:
  §4 session negotiation (NIP-44 + NIP-51 flow), §7 secret storage + signer
  abstraction, §2.4 core/UI boundary permits headless agent-only, §10.4 MCP.
  Open spec questions: relay selection (:748), macOS headless permissions
  (:749–751).
- Reviews: `docs/reviews/slop-audit-nostr.md` (authorization/allowlist/publish
  concerns), `slop-audit-fips.md` (hard-coded relay defaults, duplicated signer
  setup, example config has no reader).

### Evidence refinements and blocking behavior

The preceding line anchors are retained from the curated findings. Implementation should confirm their current locations by symbol; newly observed behavior below is anchored by actual functions rather than invented line numbers.

1. **The transport is NIP-17, not bare encrypted DMs.**  
   `md_nostr_send_dm()` builds rumor → seal → gift-wrap, using NIP-44 internally. Preserve this transport and correct the specification’s abbreviated terminology.

2. **Authorization currently ends before transport authentication begins.**  
   Host startup handles one request, then enters a reusable TCP accept loop. It exposes the stream to `on_encoded()` and sends the initial accessibility tree before validating `MD_PKT_SESSION_INFO`. `handle_client_packet()` does not compare its session ID with the negotiated session.

3. **Granted capability bits are not enforced at the host boundary.**  
   `MD_PKT_INPUT`, `MD_PKT_ACTION`, video publication, and initial tree publication are not gated by negotiated permissions. An approval dialog alone would therefore not provide scoped access.

4. **An empty allowlist is indistinguishable from no authorization policy.**  
   `md_nostr_has_allowlist()` tests entry count. It cannot distinguish loading, missing, explicitly empty, or unavailable data.

5. **Allowlist publication is not transactional.**  
   `md_nostr_allowlist_add()` and `remove()` change the cached list before signing or relay acknowledgment. Incoming replacements are not ordered by timestamp/event ID, and add/remove do not establish unique membership.

6. **The existing NIP-51 API cannot directly support external-signer private lists.**  
   Its creation/encryption functions require `sk_hex`. Reuse its list model and encoding, but add unsigned/plaintext serialization seams rather than retrieving a secret key.

7. **Callback and shutdown ownership require correction.**
   - DM callbacks run concurrently on unwrap workers; `volatile` request flags are not synchronization.
   - Host creates `session_req_ch` after starting callbacks.
   - Client closes its channel before destroying the producer.
   - `md_nostr_destroy()` frees allowlist state and its mutex before joining DM callbacks that may access them.
   - Host does not consistently destroy its borrowed signer after destroying `MdNostr`.

8. **Relay-pool extension cannot safely mean “call subscribe again.”**  
   `md_nostr_subscribe()` documents shared-filter lifetime problems. Dynamic discovery and multiple document subscriptions require an upstream ownership fix first.

9. **The specification overstates delivered management UX.**  
   The checked “2.6 UI session manager” milestone is contradicted by the selected sources. Mark it partial, retaining tree-format/delta work as completed.

**Approach:** A bounded management/session-control refactor is necessary; extending only `ui.cpp` would leave policy, lifecycle, and authentication defects intact. Capture, codec, accessibility backends, input backends, and FIPS discovery remain outside the refactor.

## 3. Design

### 3.1 Architectural decisions and rollout

| Decision | Resolution |
|---|---|
| Management toolkit | Linux-first GTK4 ≥ 4.14/libadwaita ≥ 1.5 companion, `metadesk-manager`. |
| Shared implementation | C17 management, document-sync, routing, authorization, and connection controllers in `libmetadesk`; no GTK/SDL types in their interfaces. |
| Existing overlay | Retain SDL2/ImGui for streaming, status, cancellation, disconnect, and a management-launch action. |
| macOS | Retain CLI and overlay; receive shared-core security/routing improvements. Native management frontend is a later work item. |
| Windows | No management rollout in this scope; preserve compile-time isolation of platform-specific dependencies. |
| Key custody | External signer by default. Grotto is the recommended Linux provider, not a required protocol implementation. |
| NIP-78 | Implement reusable event construction/parsing in nostrc, not a metadesk-only protocol implementation. |
| Host approval authority | The running metadesk host. Its authenticated local companion presents decisions; Grotto authorizes cryptographic operations, not desktop-session permissions. |
| First-run relays | Explicit operator selection. No silent public relay fallback. |
| Concurrent sessions | One active remote session per host initially; bounded pending approvals, no automatic takeover. |

The GTK companion does **not** embed SDL or run its event loop. It launches the existing video client as a separate process.

#### Process and ownership boundaries

```text
metadesk-manager
  GTK main context
    ├─ public identity panels / management pages
    ├─ MdManagement controller thread
    │    ├─ external signer
    │    ├─ MdNostr / relay plan
    │    └─ encrypted collections and app-data cache
    ├─ authenticated local host-control connection
    └─ inherited private launch/status channel → metadesk-client

metadesk-host
  host control loop
    ├─ MdManagement + signer + MdNostr
    ├─ MdHostAccess: requests, decisions, grants, revocation
    ├─ local management endpoint
    └─ existing capture/encode/input/a11y resources

metadesk-client
  SDL main thread
    ├─ renderer + overlay + decode/display
    └─ connection worker: same C connection/signaling implementation as CLI
```

The companion owns its client-side address-book state. The host owns its authorization list and pending/active sessions. A companion must edit host permissions through the host service, not publish an independent competing host policy.

### 3.2 UX precedents and information architecture

#### Short prior-art survey

These are public product-documentation patterns, not observations from a fresh product evaluation. Edition-specific details must be checked before using screenshots or making parity claims.

| Product | Documented pattern / reference | Design inference for metadesk |
|---|---|---|
| RustDesk | Remote IDs, saved peers, and attended/unattended access: [client documentation](https://rustdesk.com/docs/en/client/). | Put quick connection, saved hosts, and access configuration in distinct places. |
| Parsec | A computer-oriented connection surface with sharing and permission concepts: [support documentation](https://support.parsec.app/). | Explain the target computer and requested interaction scope before connecting. |
| AnyDesk | Addresses/aliases, address books, favorites/tags, and permission profiles: [support documentation](https://support.anydesk.com/). | Keep naming/organization separate from authorization; show reusable permission presets. |
| Moonlight | Pairing precedes use of a host/application launcher: [setup guide](https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide). | Make trust establishment a recognizable first connection step; keep the streaming interface lightweight. |

None of these precedents justify introducing centralized metadesk accounts, passwords, or proprietary identifiers.

#### Companion navigation

Use an adaptive sidebar/content layout modeled on `nss-window.c`, not copied wholesale.

| Page | Contents |
|---|---|
| **Hosts** | Connect field, search, saved hosts, favorites, tag filters, recent attempts. |
| **This Computer** | Host identity, FIPS readiness, sharing status, pending requests, active session, stop-sharing control. |
| **Access** | Host-specific remembered clients, exact capability scopes, revoke/edit actions. |
| **Identity** | Active signer, public identity, connection status, read-only shared Grotto identity panel where available. |
| **Relays** | Bootstrap relays, discovery provenance, DM delivery relays, storage/write relays, per-relay errors. |
| **Activity** | Local connection/approval outcomes; existing signed session-log integration remains separate. |

Use a persistent banner for unavailable signer/relay/host services, inline errors for invalid addresses, and toasts only for completed non-blocking actions. Approval requests remain in an inbox until resolved or expired; a notification opens that exact request and never approves it directly.

Grotto integration is deliberately narrow:

- Reuse `GrottoIdentityPanel` through the existing Control1 service adapter.
- Hide unavailable security callbacks; use `open_in_signer` for create/import/backup/replacement.
- Do not map desktop-session access into `GROTTO_PANEL_SET_PERMISSIONS`.
- Do not copy or link `approval-controller.c`: its requests and authority belong to the signer daemon.
- Connected Apps, Remote Signing, and signer Activity remain available through Grotto/Nostr Settings rather than becoming duplicate metadesk pages.

### 3.3 Per-feature Nostr mapping

| Feature | Primitive | Boundary / fallback |
|---|---|---|
| Public identity display/share | NIP-19 `npub`, `nprofile` | Internally use canonical 64-character lowercase hex keys. |
| Human-readable host lookup | NIP-05 | Maps a name to a key; never proves online status, authorization, or FIPS reachability. |
| Durable signing/encryption identity | NIP-46, NIP-55L, NIP-5F | No native key vault. Grotto handles key lifecycle on Linux. |
| Session negotiation | NIP-17 gift-wraps using NIP-44 | FIPS readiness remains a separate local-daemon concern. |
| Remembered client permissions | NIP-51 kind 30000 | Host-owned, private by default; capability metadata is a documented metadesk extension. |
| Address book, favorites, tags | Separate NIP-51 kind-30000 collections | Membership only; none grants access. |
| Labels, pinned lookup metadata, connection preferences | NIP-78 kind 30078 | NIP-44-encrypted to the document owner. |
| Relay discovery | NIP-65; NIP-17 DM relay metadata | Metadata informs explicit routing implemented by metadesk. |
| One-session approval | Local host memory | Ephemeral authorization should not require a durable relay publication. |
| Pending approvals / active sessions | Local host state + authenticated IPC | Never publish tokens or a live approval inbox. |
| FIPS socket, process status, window geometry | Local configuration/state | Machine-specific operational data is not useful cross-device app data. |
| Secret-store fallback | Existing `MdSecrets`, later opt-in integration | Not needed for the first management release; never pretend it is already wired. |

### 3.4 Identity and key custody

#### Supported behavior

1. GUI first run tries existing `md_signer_auto_detect()` order: NIP-5F, then NIP-55L.
2. Explicit backend selection never silently falls back to another identity.
3. NIP-46 remains an explicit pairing option. Pairing URI and resulting transport credentials stay in memory for this release; restarting requires re-pairing.
4. Create, import, backup, remove, and replace identity actions open the external signer.
5. Do not generate a durable identity in metadesk.
6. Preserve direct-key constructors for tests and explicitly configured secure integrations. Remove automatic ephemeral client identity creation from managed connections.
7. Identity switching disconnects active/pending client connections, cancels document operations, destroys the previous bridge, and loads an owner-separated cache.

An `nsec` pasted into a connect field is rejected without echoing it. Clear and securely erase application-owned input buffers; avoid putting such input into history or logs.

#### Signaling identity versus FIPS identity

Resolve the existing implicit assumption explicitly:

- **Host:** its signaling public key must equal the local FIPS daemon’s node public key for this release. Host addresses therefore identify both the signaling recipient and the FIPS destination. A mismatch disables managed hosting with a diagnostic; no unsupported mapping is guessed.
- **Client:** its signer identity is the access-control principal. Its local FIPS node identity may differ.
- Requests carry the client’s actual FIPS node `npub`, obtained from `show_status`, rather than putting the signer’s hex key into `fips_addr`.
- That client transport address is a reachability hint, not proof of ownership. Possession of the separately encrypted stream token binds the authorized signer to the stream.
- Host `--npub` retains its expected-client-principal restriction; readiness checks use the request’s transport identity, not the expected signer key.

FIPS key provisioning remains an operator/signer responsibility. metadesk never extracts a private key to configure FIPS.

#### `MdSigner` changes

Keep existing operation signatures. Add a live-check operation:

- Existing `md_signer_is_ready()` remains a cheap cached-key predicate.
- Existing `md_signer_probe()` retains its documented initialization behavior.
- Add `md_signer_check_connection(...)`: always calls the backend, verifies that the returned key matches the pinned identity, and returns a typed signer error.

Serialize backend operations with an internal signer mutex, including session-log calls. Never hold an allowlist/document/host-client lock across signer I/O. Signed results must be signature-validated and checked against the requested author and unsigned fields before publication.

### 3.5 Management types, interfaces, and execution model

These are C opaque components, consistent with existing `MdNostr`, `MdSigner`, and renderer conventions.

| Component / location | Owner and state | Interface contract |
|---|---|---|
| `MdManagement`, `src/core/management.c/.h` | Created by each process; owns signer, bridge, document store, routing state, controller thread. | Asynchronous commands return operation IDs; emits owned immutable snapshots/results through a drainable event channel. |
| `MdManagementConfig` | Deep-copied at creation. | Role (`manager`, `host`, `client`), pinned signer identity/descriptor, bootstrap URLs, local paths, FIPS endpoint. No frontend types. |
| `MdManagedStore`, `src/core/managed_store.c/.h` | Owned by management; encrypted document snapshots, pending writes, base event IDs. | Load/apply/edit/retry/discard operations; no relay I/O or permission decisions. |
| `MdHostAccess`, `src/core/host_access.c/.h` | Owned by host control loop; pending requests, active grant, authorization revision. | Receive request, decide, revoke, expire, bind stream; synchronous policy transitions, asynchronous signing/publication effects. |
| `MdClientConnection`, `src/core/client_connection.c/.h` | One per connection attempt; owns cancellation/generation, negotiation state, then the negotiated stream until handoff. | Start/cancel/drain; stream ownership transfers exactly once to the client thread. |
| `MdManagementServer`, `src/core/management_ipc.c/.h` | Host-owned listener and authenticated peers. | Closed, versioned local methods; uses existing IPC/JSON-RPC infrastructure. |
| GTK adapter, `src/manager/md-manager-model.c/.h` | GTK main-context GObject; owns `GListStore` models and management reference. | Converts C snapshots to immutable row objects; commands only enqueue work. |

No general plugin framework or second relay implementation is introduced.

#### Closed state sets

- **Document load:** `LOADING`, `READY`, `ABSENT`, `UNAVAILABLE`. A separately marked cached snapshot may exist in any state.
- **Document write:** `CLEAN`, `PENDING`, `PARTIAL`, `CONFLICT`, `ERROR`.
- **Pending approval:** `PENDING`, `SUBMITTING`, `ACCEPTED`, `DENIED`, `EXPIRED`, `CANCELLED`, `UNAVAILABLE`.
- **Client connection:** `IDLE`, `RESOLVING`, `CHECKING_SIGNER`, `CHECKING_FIPS`, `PUBLISHING_REQUEST`, `WAITING_APPROVAL`, `DIALING`, `BINDING`, `ACTIVE`, `RETRY_WAIT`, `FAILED`, `CANCELLED`.

Each result includes operation ID, process generation, typed error category, and non-secret diagnostic text. Unknown events or stale generations never mutate the current model.

#### Concurrency and lifecycle

- GTK and SDL stay on their respective main threads.
- A management controller serializes document/routing/policy mutations.
- Relay callbacks copy inputs into the controller’s queue; they do not mutate UI models or `HostCtx`.
- Blocking signer/HTTP work runs off UI and relay-receive threads.
- Limit DM unwrap work to four active workers and 64 queued events. Overflow drops untrusted work; it must not affect existing grants.
- Cancellation invalidates the operation generation immediately. If an underlying call cannot abort, discard its eventual result and join it during teardown.
- Shutdown order: stop accepting commands → stop relay producers/subscriptions → cancel/join workers → drain/free callback messages → free caches/channels → destroy signer.
- Capture only observes an authenticated active stream plus granted capability mask under `client_mu`; it never consults mutable approval objects.

### 3.6 Persistent data model

#### Common event rules

All documents are keyed by `(kind, author, d-tag)`. Validate event ID/signature, exact author, kind, identifier, and content limits before accepting them.

Apply Nostr replacement ordering:

1. Greater `created_at` wins.
2. Equal timestamp: lexicographically lower event ID wins.
3. Duplicate event IDs are no-ops.

Persist the winning signed event, not just its decoded projection. Never replace a newer cached event with an older relay response. Reject events more than five minutes in the future; surface clock problems rather than advancing authorization.

For local writes, choose `created_at = max(current_time, previous_created_at + 1)`, subject to the same future bound.

#### NIP-51 collections

All use kind **30000**. New documents expose only their `d` tag publicly. Their `content` is a NIP-44 ciphertext addressed to their own author.

| Collection | Exact `d` value | Decrypted entry shape | Authority |
|---|---|---|---|
| Host permissions | `metadesk-allowlist` | Standard `p` membership plus metadesk capability entries | Authorizes unattended access on that host only |
| Saved hosts | `metadesk-hosts` | `["p", host_hex]` | None |
| Favorites | `metadesk-favorites` | `["p", host_hex]` | None |
| Tag membership | `metadesk-tag:<uuid>` | `["p", host_hex]` | None |

Permission example, shown as **decrypted content**, not public tags:

```text
["p", client_hex]
["metadesk-caps", client_hex, "video,input"]
```

Rules:

- Canonical scope order is `video,agent,input`; comma-separated, no spaces.
- `p` means membership; `metadesk-caps` supplies its scope.
- A member without valid scope metadata grants **zero** capabilities.
- Unknown/duplicate scope metadata for a principal fails closed for that principal.
- Upsert is by public key; remove drops every matching membership and scope entry.
- Preserve unrelated unknown entries when editing.
- Do not put capabilities into the third position of a `p` tag: that position is a relay hint in standard list usage.
- Collection names and membership are never authorization evidence outside `metadesk-allowlist`.

The `d` tag reveals application usage and collection coordinates; encryption does not hide event author, timestamps, or approximate payload size.

#### Existing public allowlist compatibility

Read existing public `p` tags and recognize third-position capability strings only as the legacy metadesk encoding.

- Mark these records **“Legacy public permissions — review required.”**
- Do not automatically grant legacy entries access.
- Preserve the original event until explicit migration.
- Migration presents all entries and requires explicit scope confirmation.
- Publish a replacement with private entries and no public client `p` tags.
- Explain that removing public tags cannot erase copies already retained by relays or observers.

If a mixed public/private list arrives, treat it as requiring review; do not silently merge it into active authorization.

#### NIP-78 application data

Use kind **30078**, one coordinate per owner:

- `d = "metadesk:management:v1"`
- Public tags: only `["d", "metadesk:management:v1"]`
- `content`: NIP-44 encryption to the author of the application JSON.

The plaintext schema is:

| Field | Type / default |
|---|---|
| `schema_version` | Integer, exactly `1` |
| `hosts` | Map keyed by canonical host hex; default `{}` |
| `hosts[pk].label` | UTF-8 string, default `""`, ≤128 bytes |
| `hosts[pk].nip05` | Normalized identifier or `null`, default `null` |
| `hosts[pk].relay_hints` | Deduplicated URL array, default `[]`, ≤16 |
| `hosts[pk].requested_caps` | Array of known capability strings; default `["video"]` |
| `hosts[pk].tree_format` | `"json"` or `"compact"`; default `"compact"` |
| `hosts[pk].port` | Integer 1–65535; default `7700` |
| `tags` | Map keyed by UUID; default `{}` |
| `tags[id].label` | UTF-8 string, ≤64 bytes |
| `tags[id].deleted` | Boolean, default `false` |
| `preferences.default_requested_caps` | Known capability array; default `["video"]` |
| `preferences.default_tree_format` | `"json"` or `"compact"`; default `"compact"` |

There is no favorites boolean or tag-membership array here: NIP-51 owns those memberships.

Additional rules:

- The saved host’s key is the pin; `nip05` is an associated lookup name, not a mutable identity reference.
- Cap document content at 256 KiB ciphertext and 1,000 saved hosts.
- Unknown schema versions are read-only; never overwrite them with an empty v1 document.
- Preserve unknown JSON fields during supported-version edits.
- Deleting a tag sets its metadata tombstone and publishes an empty corresponding list.
- Removing a host removes favorites/tag memberships and metadata through separate document updates. Until all updates complete, the UI filters orphan memberships against `metadesk-hosts`; it does not imply atomic multi-event transactions.

#### Local configuration and state

For Linux:

- Configuration: `$XDG_CONFIG_HOME/metadesk/management.json`, defaulting to `~/.config/metadesk/management.json`.
- Cache/outbox: `$XDG_STATE_HOME/metadesk/`, defaulting to `~/.local/state/metadesk/`.
- Runtime socket directory: `$XDG_RUNTIME_DIR/metadesk/`.
- Directories mode `0700`; files/socket mode `0600`; atomic same-directory replacement.

Configuration schema version `1` contains:

- `bootstrap_relays: string[] = []`
- `signer_backend: "auto" | "nip5f" | "nip55l" | "nip46" = "auto"`
- `signer_socket: string | null = null`
- `expected_signer_pubkey: string | null = null`
- `fips_control_socket: string | null = null`
- `host_enabled: boolean = false`
- `unattended_enabled: boolean = false`
- Window geometry and last selected page.

Never persist bunker URIs, signer transport secrets, private keys, stream tokens, or plaintext decrypted address-book/app-data documents. Cache/outbox entries are signed events whose private content is already encrypted.

Recent connection times/outcomes remain local and retention-bounded to 100 records. They contain public-key references and error categories, not private labels or session credentials.

#### Sync and conflicts

Use whole-document replacement, not an invented CRDT.

1. Subscribe before presenting a live baseline.
2. Retain the cached document while processing relay backfill.
3. Record the winning event ID as the edit’s base revision.
4. Build/sign a replacement from that baseline.
5. Persist the signed event before publication.
6. Publish and record per-relay results.
7. Any acknowledgment makes the document “saved to at least one relay”; all required targets acknowledging makes it fully synchronized.

Contacts/preferences may show a clearly marked pending local projection. New unattended permissions become active only after successful signing, durable cache write, and at least one required storage-relay acknowledgment.

If another winning event differs from the edit’s base during an outstanding write, mark `CONFLICT`. Preserve both documents and require **Keep Remote** or **Review and Reapply Local Changes**. Do not automatically rebase permission grants or overwrite newer remote data.

Retry the same signed event after partial/unknown publication; do not generate a newer timestamp on every retry. Nostr has no compare-and-swap guarantee: concurrent offline writes can still lose updates, which must be disclosed.

### 3.7 Reusable nostrc extensions

#### NIP-78

Add:

- `nips/nip78/include/nostr/nip78/nip78.h`
- `nips/nip78/src/nip78.c`
- Component build/install/export metadata and tests.

Provide partial interface shapes:

- `nostr_nip78_build_event(event, author, identifier, content, created_at)`
- `nostr_nip78_parse_event(event, out_document)`
- Document destructor.

The owned parsed document contains author, identifier, content, timestamp, and event ID. The library treats content as opaque; it neither knows metadesk’s schema nor obtains a secret key.

Builders create unsigned events. `MdSigner` supplies encryption and signing. Add the kind to nostrc’s kind registry if absent, using one canonical registry constant.

#### NIP-51

Extend existing APIs with:

- Serialization of private entries to the standard plaintext tag-array JSON.
- Parsing that plaintext into existing `NostrListEntry` objects.
- Unsigned list-event construction with caller-supplied encrypted content.

Keep current raw-key APIs as compatibility wrappers. This avoids coupling nostrc to `MdSigner` while reusing its existing list semantics.

#### Relay pool and publication

Before adding management subscriptions:

- Give every subscription an independently owned filter set and explicit handle.
- Add subscription-scoped EOSE/CLOSED callbacks carrying relay identity.
- Add public relay enumeration/access instead of accessing pool internals.
- Provide bounded, cancellation-aware publish waits with per-relay outcomes.
- Existing subscription APIs must delegate to the corrected ownership implementation.

EOSE ends backfill, not the live subscription. CLOSED records failure and recovery state; authentication requirements are surfaced, not treated as successful empty results.

**Validation required:** the selected files do not include these pool implementations or build registries. Inspect their current APIs before implementation; adapt additive names to existing naming conventions while retaining these ownership and lifecycle contracts.

### 3.8 Relay strategy and discovery

Remove runtime use of `MD_DEFAULT_RELAY`. An empty configuration means **“No relays configured”**, not “use Sharegap.”

#### Routing categories

| Purpose | Targets |
|---|---|
| Bootstrap discovery | Explicit local bootstrap relays |
| Public relay-list discovery | Bootstrap relays plus validated address hints |
| Own collections/app-data storage | Own NIP-65 write/read-write relays; explicit bootstrap relays as fallback only when no write list exists |
| Fetch own stored documents | Bootstrap plus own NIP-65 read and write targets |
| Incoming NIP-17 DMs | Own NIP-17 kind-10050 DM relays |
| Outgoing NIP-17 DMs | Recipient kind-10050 DM relays |
| Compatibility DM fallback | Recipient NIP-65 read/read-write relays, then validated explicit hints/bootstrap targets |

NIP-65 write relays are **not** automatically the correct recipient DM inbox. Add kind-10050 metadata handling to the bridge; use the kind registry rather than hardcoded numbers in metadesk.

For first run without a DM list, the user explicitly confirms selected relay URLs for DM reception; publish kind 10050 through the signer. Until publication succeeds, display degraded discoverability and allow explicitly shared relay hints.

Additional routing rules:

- Use `nostr_nip65_parse_relay_list()` and read/write extraction functions.
- Validate author/signature/replacement ordering before applying relay metadata.
- Normalize/deduplicate using `nostr_nip65_normalize_url()`.
- Support at most 16 targets per operation and 32 active relay connections.
- Automatically discovered relays must be `wss://` and must not resolve to loopback, link-local, or private-network addresses. Explicit operator configuration can permit local `ws://` with a warning.
- Validate at connection time as well as discovery time to address DNS rebinding.
- Do not silently change FIPS’s separately configured discovery relays.
- Display target provenance: configured, NIP-05, nprofile, NIP-65, or DM list.
- When changing relay lists, publish replacements to old and new storage targets; retain failed old targets for retries, following the policy demonstrated in `nss-page-relays.c`.

Persistent connections and live subscriptions remain the normal model. A ten-second discovery deadline or publish deadline bounds the operation; it does not close otherwise healthy application subscriptions.

Publication status must distinguish `ACCEPTED`, `REJECTED`, `UNAVAILABLE`, `TIMED_OUT`, and `CANCELLED`. A timeout is an unknown delivery outcome, not proof that no relay stored the event.

### 3.9 Host authorization and approvals

#### Default policy

Managed hosting never has an implicit open mode.

| Situation | Behavior |
|---|---|
| Allowlist loading/unavailable | Never auto-accept; explicit attended approval remains possible |
| No list / explicitly empty list | Attended approval required |
| Valid remembered client | Auto-accept only when unattended access is locally enabled and policy backfill is ready |
| Unlisted client | Queue attended approval |
| No local approval UI | Deny with `approval_unavailable`; do not wait indefinitely |
| Host already active | Deny with `busy`; no takeover |
| Expected-client pin mismatch | Deny; never display as approvable |
| Signer unavailable | Preserve request/error state; never create a grant |
| Host sharing disabled | Deny with `host_disabled` |

A cached policy may be displayed while offline but cannot enable unattended authorization after startup until at least one configured storage relay has completed backfill. An older relay result cannot roll back a newer cached policy.

`ABSENT`, `READY` with zero entries, and `UNAVAILABLE` are distinct states. None means open access.

#### Approval presentation

Show:

- Host identity and requesting client’s full inspectable `npub`.
- Local label and verified identifier, explicitly secondary to the key.
- Requested capabilities.
- Separate toggles for **View screen**, **Read accessibility data**, **Control keyboard and pointer**.
- **Allow Once**, **Always Allow**, and **Deny**.
- Remaining request lifetime.

Defaults:

- Preselect only requested screen viewing.
- Never preselect input or accessibility access.
- Always Allow requires available policy storage and signer.
- Closing the dialog leaves the inbox item pending; it does not approve.
- At expiry, disable actions and remove approval authority.
- Positively observed desktop lock blocks attended decisions. Unattended access remains governed by its explicit configuration; the UI must not claim it can reliably infer lock state on every platform.

A maximum of 16 pending requests is allowed. Deduplicate by authenticated sender and request ID; allow one pending request per sender. Further distinct requests receive `busy`.

#### Exact capability enforcement

Effective grant:

```text
requested ∩ remembered-or-human-approved ∩ currently-supported
```

| Operation | Required bits |
|---|---|
| Video frames | `VIDEO` |
| Full/delta accessibility tree | `AGENT` |
| Raw pointer/keyboard input | `INPUT` |
| Agent mutation action | `AGENT` and `INPUT` |
| Agent screenshot request | `AGENT` and `VIDEO` |
| Keepalive/session bind | No capability bit; authenticated session still required |

A client can receive a reduced grant; display it before enabling controls. Zero effective capabilities produce denial.

The host must gate sends as well as receives. Do not create/send an initial accessibility tree or expose a stream to capture until binding succeeds. The input/backend readiness checks determine supported capabilities, not merely requested bits.

#### Revocation

- Local stop-sharing immediately stops new approvals and disconnects the active stream.
- Local revoke immediately disables unattended authorization and disconnects a session using the affected remembered grant.
- Persist a local coarse safety latch, `unattended_enabled=false`, before reporting revocation success.
- Then sign/publish the list removal; leave the latch disabled if signing/storage/publication fails.
- The user explicitly re-enables unattended access after policy synchronization.
- Incoming winning policy removal or scope reduction disconnects an active remembered session rather than silently continuing with stale privileges.
- An Allow Once grant is bound to its request/session and is not converted into remembered access by an unrelated list update.

This coarse latch permits safe local revocation even when the signer cannot encrypt a revised list. Cross-device revocation cannot be guaranteed during relay partitions; connected hosts enforce received updates immediately, and local revocation is immediate.

### 3.10 Session protocol and stream binding

Introduce **session signaling JSON version 2**. Keep the binary framing layout unchanged; its framing version and the JSON subprotocol version are distinct. Update the specification to state this explicitly.

Do not silently accept version-1 managed negotiations. The selected version-1 implementation does not provide adequate request correlation or stream binding.

#### Message contracts

| Message | Required fields |
|---|---|
| `session_request` | `v:2`, random 128-bit `request_id` as 32 lowercase hex characters, `issued_at`, `expires_at`, `capabilities`, `tree_format`, client FIPS `npub` in `fips_addr` |
| `session_accept` | `v:2`, matching `request_id`, non-secret `session_id`, `granted`, `tree_format`, random 256-bit `stream_token` as 64 lowercase hex characters, token `expires_at` |
| `session_deny` | `v:2`, matching `request_id`, `reason` |
| `session_cancel` | `v:2`, matching `request_id` |
| TCP `session_bind` | `v:2`, `request_id`, `session_id`, `stream_token` |
| TCP `session_bound` | `v:2`, `request_id`, `session_id`, authoritative `granted`, `tree_format` |

Closed denial reasons: `denied`, `expired`, `busy`, `approval_unavailable`, `host_disabled`, `not_authorized`, `unsupported_capabilities`, `unavailable`.

Limits and timing:

- Session JSON ≤8 KiB.
- Request validity: 180 seconds; reject future issuance beyond 30 seconds or longer lifetimes.
- Host attended-decision deadline: the earlier of request expiry or 120 seconds after receipt.
- Convert accepted deadlines to monotonic time locally.
- Stream token lifetime: 30 seconds from acceptance creation.
- Unauthenticated TCP bind deadline: five seconds.
- At most four unauthenticated TCP connections; none can block the host control loop.
- CSPRNG failure denies the request; remove the zero-filled random fallback.

#### Binding algorithm

1. Authenticate/decrypt/validate the request and enqueue it on the host controller.
2. Resolve its grant through policy or an exact attended decision.
3. Reserve a session ID and one-use token before publishing the acceptance.
4. Send acceptance only through the validated NIP-17 path.
5. Receive `session_bind` as the first application packet on a candidate stream.
6. Compare the token in constant time; require matching IDs, unexpired reservation, and current authorization revision.
7. Consume the token once and atomically install the active stream.
8. Send `session_bound`; only then enable video/tree/input/action handling.
9. On bind failure or expiry, destroy the candidate stream and reservation.

An acceptance may reach the client before its relay OK reaches the host. A correctly bound stream is proof that acceptance was delivered; an unknown publish outcome must not create a contradictory second grant.

The non-secret session ID may remain in session logs. The stream token must never enter logs, published monitoring events, persistent state, or companion snapshots.

#### Authentication and replay hardening

Before delivering any DM:

- Validate the gift-wrap signature and kind.
- Validate the seal signature and kind.
- Require the rumor author to equal the seal author.
- Validate rumor kind, canonical ID, recipient, and expected content shape.
- Use nostrc validation/hash primitives; do not require an unsigned rumor to carry a signature.

The current five-minute gift-wrap `since` filter is incompatible with NIP-59 timestamp randomization. Use a two-day lookback plus five minutes; apply freshness to the inner session message instead.

Maintain:

- Bounded outer-event deduplication before expensive unwrap.
- Request/result deduplication by `(sender, request_id)`.
- Client acceptance checks against the active generation, host key, request ID, requested capabilities, and expiry.

Exact duplicate requests do not reopen prompts. Cache terminal results through request expiry. Cancelled, expired, or already-bound requests cannot mint another token.

### 3.11 Local host control and client launch

#### Host endpoint

Use the existing Unix IPC and JSON-RPC seams, adding authenticated local framing where necessary.

- Socket: `$XDG_RUNTIME_DIR/metadesk/host-<host_hex>.sock`.
- Validate directory ownership and reject symlinks/unsafe stale paths.
- Linux: verify `SO_PEERCRED` UID equals host effective UID.
- Future macOS implementation: equivalent `getpeereid` check.
- No TCP listener and no remote management commands.

Same-user native processes are inside this trust boundary; this is not a sandbox against malicious code running as that user.

Closed protocol version-1 methods:

- `management.subscribe`
- `management.snapshot`
- `requests.decide`
- `access.upsert`
- `access.remove`
- `sessions.disconnect`
- `host.set_enabled`
- `host.set_unattended`

Mutations carry host generation and expected snapshot revision; request decisions additionally carry the exact request ID and selected capability bits. Stale commands return conflict without substitution.

Subscribe before requesting a snapshot. Events contain a host generation and increasing sequence. Ignore duplicates; on a sequence gap, reconcile from a snapshot. No raw signer commands, signed-event publication, key import, or arbitrary RPC forwarding is exposed.

#### Companion → video client

Launch without a shell, using an inherited socketpair:

- Add `--managed-session-fd N`.
- Channel carries a versioned launch configuration, progress, cancellation, and final outcome.
- Carry credential-bearing NIP-46 material only in memory through that private channel, never command-line arguments.
- The child pins the selected signer key and establishes its own signer connection.
- Only the intended child descriptor survives `exec`; all others are close-on-exec.
- Disconnecting the channel cancels an unfinished connection. An active session ends if its owning companion explicitly sends disconnect; ordinary companion window closure does not terminate a separate active video window.

The launch channel is private to the parent/child relationship and is not a second host-management listener.

### 3.12 First-run, connect, offline, and reconnect flows

#### First run

1. Open Hosts with an empty state; sharing is disabled.
2. Detect/select signer; show full public identity.
3. Offer Open Grotto for create/import/backup.
4. Require bootstrap relay selection; offer paste/import from an `nprofile`.
5. Discover own relay metadata and offer explicit DM-relay setup.
6. Load encrypted collections; show cached/loading/unavailable distinctly.
7. For hosting, check local FIPS identity match and platform capture/input/a11y readiness.
8. Enable sharing only through explicit user action; unattended remains off.

#### Connect

Accepted input: `npub`, `nprofile`, NIP-05 identifier, optionally prefixed with `nostr:`. Reject `nsec`, event IDs, and `naddr` in this field. Direct IP belongs in a separate advanced development path.

1. Normalize input using NIP-19/NIP-05 APIs.
2. Resolve a canonical key and relay hints.
3. If saved, compare NIP-05 resolution with the pinned key.
4. On mismatch, block connection and offer a separate review/replacement action. Never silently repin.
5. Show requested access; first-time host connections default to view-only.
6. Check signer, local FIPS daemon, then target FIPS readiness.
7. Publish request; show Waiting for Approval separately from Connecting.
8. Dial, bind, receive authoritative acknowledgment, then activate.
9. Offer Save Host independently of host permission decisions.

NIP-05 network work is bounded to ten seconds and HTTPS with verified TLS. Do not expose the library’s insecure-test option through the GUI. Validate DNS/redirect targets to avoid requests to local/private services.

#### No relay / offline

- No configured relay: no network activity; explain what must be configured.
- Cached contacts remain viewable; private data requires signer decryption.
- Offline edits become signed encrypted pending events only if the signer works.
- With no signer, permit viewing already-decrypted in-memory data and editing an in-memory draft; warn before discarding it.
- Managed remote connection and new unattended grants cannot proceed without signaling/storage requirements.
- Keep diagnostics separate: signer unavailable, relay unavailable, request rejected, FIPS unavailable, route not ready, or stream failure.
- Do not fall back from managed connection to direct TCP.

#### Reconnect and cancellation

- On unexpected stream loss, clear stream/token state and create a fresh request ID.
- Retry transient transport failures after 1, 2, 4, 8, 16, then 30 seconds.
- Never reuse a stream token or previous acceptance.
- Denied, revoked, identity-changed, and unsupported-capability outcomes stop automatic retry.
- Repeated attended reconnects require fresh approval; a remembered grant can auto-authorize after policy checks.
- Cancel sends best-effort `session_cancel`, invalidates the generation, and returns to Hosts/Disconnected.
- Host control-loop authorization continues throughout its lifetime; remove the one-time 60-second startup negotiation and direct-TCP fallback.

#### Overlay and event handling

- Create the video window before asynchronous connection work so progress/cancel remain responsive.
- Replace Disconnect’s synthetic `SDL_QUIT` with an explicit overlay action.
- Disconnect ends the session; window close exits the video process.
- Escape closes a local popup first, otherwise remains available to the remote session; it no longer unconditionally destroys the window.
- F1 is local-only and is never forwarded.
- Forward remote input only after local event consumption and only with `INPUT`.
- Preserve the single SDL event pump and single composite/present.
- Reset decoder/frame/tree state between sessions.
- Clear the old video texture when returning to disconnected state; do not leave a stale remote desktop visible indefinitely.

### 3.13 API changes and call-site requirements

| Existing interface | Planned interface / behavior | Call sites |
|---|---|---|
| `md_nostr_create(cfg,cbs)` | Keep; add versioned extended configuration/callback entrypoint for document, subscription, and relay-result events. | Host, client connection controller, manager, bridge tests |
| `on_dm(sender,content,userdata)` | Retain wrapper; extended callback adds validated rumor ID/metadata and owned message delivery. | Replace `host_on_dm`, `on_session_dm` shared-state mutation |
| `md_nostr_has_allowlist()` | Deprecate as an authorization predicate. Add snapshot/status + per-principal capability query. | Host policy and bridge tests |
| `md_nostr_allowlist_add/remove()` | Retain compatibility wrappers; new edit API takes expected revision and typed scope, returns operation ID. | Host IPC/access page |
| `md_nostr_send_session_request/accept()` | Retain transport helpers; add generic typed session-message send for deny/cancel. | Connection/host-access controllers |
| `MdSessionRequest`, `MdSessionAccept` | Add v2 correlation, validity, and binding data; separate bind/bound types. | `session.c`, both entry points/controllers, tests |
| `md_session_request_from_json()` / accept parser | Strict version/field/length validation; reject malformed/unsupported data. | All signaling and bind handlers |
| Renderer event callback | Add consumed-event result through an additive callback API; preserve old callback wrapper. | `render.c`, client event forwarding |
| Overlay rendering | Keep stats API; add explicit action retrieval and connection-phase presentation. | Client main loop |
| Signer operations | Existing signatures unchanged; add live connection check. | First-run and connection checks |

Because `libmetadesk` is shared, do not silently enlarge caller-allocated public configuration/callback structures. Extended entrypoints use structure size/version fields. The session structure/protocol changes require a coordinated library ABI version change and rebuild.

### 3.14 Verification requirements

Use deterministic fakes and a local scripted relay; do not make correctness depend on public relays, sleeps, desktop services, or optional “pool initialization skipped” tests.

| Area | Required cases |
|---|---|
| Documents | Exact schemas, encryption round-trip with an external-signer fake, missing/empty/private/public distinction, unknown versions/fields, malformed entries, size limits |
| Replacement/sync | Out-of-order events, equal-time tie break, duplicate delivery, stale base, partial publication, retry same event, crash after durable write |
| Authorization | Startup request before EOSE, no list, empty list, denied principal, invalid scope, unattended off, no approval UI, stale decision, expiry |
| Capability gates | No pre-bind video/tree/input/action; every capability combination; screenshot requires video; local backend unavailable |
| Binding | Wrong/expired/reused token, wrong IDs, duplicate bind, concurrent candidate streams, random-source failure |
| DMs | Invalid outer/seal signatures, rumor-author mismatch, wrong recipient, randomized outer timestamps, cross-relay duplicate |
| Routing | Correct NIP-65 read/write roles, kind-10050 priority, fallback provenance, no configured relay, CLOSED/AUTH, target limits, unsafe discovered URLs |
| Lifecycle | Callback during shutdown, signer blocks, cancel during lookup/publish/dial, producer-before-channel teardown, no leaked signer/stream/thread |
| UI | 360-pixel layout, keyboard navigation, plain-text rendering, exact notification target, no approval from notification, stale row reconciliation |
| Compatibility | v1 managed request rejected, no direct fallback, legacy policy review/migration, core/client build without GTK |

Integration gate: exercise the complete real entrypoint path—companion → host decision → DM acceptance → TCP bind → capability-gated stream—not only isolated policy helpers.

## 4. File-by-file impact

### 4.1 Spike changes only

| File | Change | Dependency |
|---|---|---|
| `docs/plans/management-ux-2026-11-14.md` | Replace open questions with this resolved design, retaining curated Background. | None |
| `docs/metadesk-spec.md` | Correct management milestone status; document planned session v2, identity/FIPS distinction, private policy schema, key custody, explicit relay setup, and unsupported legacy negotiation. Correct §2.4 C/C++ wording. | Design agreement; no claim that future implementation is complete |

All following files are **future implementation impacts**, not changes to make during the spike.

### 4.2 Metadesk core

| File | Planned changes and reason | Ordering |
|---|---|---|
| `src/core/management.c/.h` — new | Controller, owned snapshots, commands, shared bootstrap/signer setup. | nostrc seams and store types |
| `src/core/managed_store.c/.h` — new | Schemas, encrypted event cache/outbox, replacement ordering, conflict state. | NIP-51/NIP-78 extensions |
| `src/core/identity_resolver.c/.h` — new | NIP-19/NIP-05 normalization, pin checks, network limits and URL policy. | NIP-05/19 linkage |
| `src/core/relay_plan.c/.h` — new | Relay-role selection and provenance; no socket implementation. | Pool callbacks, NIP-65 linkage |
| `src/core/host_access.c/.h` — new | Host authorization state machine, deadlines, scoped grants, binding reservations. | Session v2 and policy snapshots |
| `src/core/client_connection.c/.h` — new | Cancellable FIPS/signaling/dial/bind controller used by CLI and GUI launches. | Session v2, routing |
| `src/core/management_ipc.c/.h` — new | Same-user host-control service/client, revisions, subscription snapshots. | Host-access state |
| `src/core/nostr.c` | Validate/deduplicate DMs; bounded unwrap; safe teardown; owned subscriptions; document delivery; per-target publication; remove pool-internal access. | Upstream relay-pool work |
| `src/core/nostr.h` | Extended versioned interfaces; deprecate empty-list/open-mode contract; remove runtime default relay. | Land with bridge callers |
| `src/core/signer.c/.h` | Serialize backend calls; live key-pinning check; validate signed results; secure cleanup on touched failure paths. | Before concurrent management use |
| `src/core/session.c/.h` | Strict v2 messages, deny/cancel/bind/bound parsing; keep streaming lifecycle separate from approval state. | Atomic with host/client v2 adoption |
| `src/core/stream.c/.h` | Candidate-stream bind gate support and explicit cancellation/close behavior; no video exposure before authentication. | Inspect current stream ownership first |
| `src/core/ipc.h`, `src/core/ipc_unix.c` | Reuse/extend framing and peer-credential support; no new remote transport. | IPC service |
| `src/core/agent.c/.h` | Apply per-session tree format and gate action classes/screenshots at the dispatch boundary. | Granted-scope model |
| `src/core/session_log.c/.h` | Record request/decision outcomes without tokens; serialize signer use; retain non-secret session IDs. | Signer/controller lifecycle |

`secrets.c/.h`, capture HALs, codec algorithms, and FIPS discovery implementation remain unchanged. Secure 1Password startup is a separately scoped follow-up.

The selected context does not include `stream`, `ipc`, `agent`, or session-log implementations. Validate their public ownership/threading contracts before editing; these are identified integration points, not assertions about unseen APIs.

### 4.3 Entry points and streaming UI

| File | Planned changes and reason | Ordering |
|---|---|---|
| `src/host/main.c` | Replace startup-only negotiation with lifetime control loop; authenticated candidate streams; capability gates; remove implicit fallback; own signer teardown consistently; retain capture pipeline. | Host-access, session v2 |
| `src/client/main.c` | Use connection controller; remove ephemeral fallback; add inherited launch FD; responsive progress/cancel; enforce grants locally; safe teardown/reconnect. | Client controller |
| `src/client/ui.h`, `ui.cpp` | Explicit actions instead of synthetic quit; phase/error display; restricted-access display; management action. | Client state model |
| `src/client/render.h`, `render.c` | Consumed-event callback, remove unconditional Escape quit, add texture reset; preserve HiDPI/single-present behavior. | Client event handling |

### 4.4 Linux companion — new files

| File | Responsibility |
|---|---|
| `src/manager/main.c` | `AdwApplication` startup and process lifetime |
| `src/manager/md-window.c/.h` | Adaptive navigation and service banners |
| `src/manager/md-manager-model.c/.h` | C snapshots → GTK list models; main-context delivery |
| `src/manager/md-page-hosts.c` | Connect, saved hosts, favorites, tags, pin review |
| `src/manager/md-page-host.c` | This Computer status, sharing, active session |
| `src/manager/md-page-access.c` | Remembered-client scope editing and revocation |
| `src/manager/md-page-identity.c` | Signer setup and shared identity panel |
| `src/manager/md-page-relays.c` | Relay-role/provenance/status UI; no copied systemd/session-relay controls |
| `src/manager/md-page-activity.c` | Local management outcomes |
| `src/manager/md-approval.c/.h` | Pending inbox, exact-request dialog and notification actions |
| `src/manager/md-grotto.c/.h` | Existing Control1 adapter integration, read-only panel host callbacks |
| `src/manager/md-client-launch.c/.h` | Child socketpair launch/status/cancellation |
| `data/metadesk-manager.desktop.in` | Application launch integration |
| `data/metadesk-manager.metainfo.xml.in` | Application metadata |

Use programmatic GTK construction consistently with the supplied Nostr Settings precedent; no new UI-template/resource system is required for the first release.

### 4.5 Build and tests

| File | Planned change |
|---|---|
| `meson_options.txt` | Add opt-in `manager` feature; require Linux when enabled. Existing `client=false` remains supported. |
| `meson.build` | Link NIP-05/19/65/78 and corrected nostrc APIs; add sources/tests; GTK/Grotto dependencies only for manager target; never consume arbitrary upstream `.o` files for new dependencies. |
| `tests/test_nostr.c` | Real authorization/document/DM cases; remove claims that NULL checks prove startup authorization. |
| `tests/test_signer.c` | Live check versus cache, pinned-key mismatch, serialized operations, denied/malformed signer responses. |
| `tests/test_session.c` | Strict v2 and binding payload validation. |
| `tests/test_stream.c` | Candidate connection and authentication lifecycle. |
| `tests/test_agent.c` | Scope gating and screenshot permissions. |
| `tests/test_managed_store.c` — new | Schemas, encryption, replacement, conflicts, crash recovery |
| `tests/test_host_access.c` — new | Decisions, deadlines, revocation, capability/token matrix |
| `tests/test_client_connection.c` — new | Cancellation, stale responses, reconnect and stream handoff |
| `tests/test_identity_resolver.c` — new | Encodings, NIP-05 mismatch, unsafe destinations |
| `tests/test_relay_plan.c` — new | Role-aware deterministic routing |
| `tests/test_management_ipc.c` — new | Peer admission, stale revisions, event gaps |
| `tests/test_manager.c` — new | Headless GTK smoke, inbox/actions, adaptive layout |
| `tests/fake_relay.c/.h` — new | Scripted local relay used by integration tests |

### 4.6 nostrc

| File / component | Planned change |
|---|---|
| `nips/nip78/include/nostr/nip78/nip78.h`, `src/nip78.c`, component build/export/pkg-config/test files — new | Reusable unsigned NIP-78 event construction and parsing |
| `nips/nip51/include/nostr/nip51/nip51.h`, `src/nip51.c`, component tests | Plaintext private-entry serialization/parsing and unsigned builder |
| `libnostr/include/nostr-simple-pool.h` and its implementation/tests | Subscription ownership, handles, EOSE/CLOSED, public relay access |
| `libnostr/include/nostr-relay.h` and its implementation/tests | Bounded cancellable publication if current APIs lack it |
| `libnostr/include/nostr-kinds.h` | Canonical missing kind constants only |
| `gnome/grotto-panels` | No widget/API change required |
| `gnome/nostr-settings` | Reference only; no metadesk coupling |
| Existing Grotto Control1 adapter and build files | Extract into a small installed companion client library only if it is not already installed; preserve existing consumers. |

The supplied panel build files show the adapter source used in tests, not an installed reusable adapter library. Verify packaging before choosing an existing dependency. If extraction is required, add `gnome/grotto-control-client/` with its own build/pkg-config files; do not copy the adapter into metadesk.

## 5. Risks and migration

### Protocol and library compatibility

- Session v2 is a coordinated host/client change. Older managed clients do not work with the new host.
- Bump the shared-library ABI for incompatible session structure changes; extended bridge interfaces are additive.
- Keep insecure direct TCP only behind explicit `--insecure-direct`, with loopback binding by default and a persistent warning. It is a development mode, not a management fallback.
- Rollback to the old host is unsafe: it ignores private permissions and lacks stream binding. Document rollback as stopping managed hosting, not reinstalling the old daemon against new policy.

### Privacy and synchronization

- Encrypting a replacement does not erase a previously public allowlist.
- Whole-document replacement can lose concurrent edits; conflict detection is useful but cannot create global transactional semantics.
- Relay EOSE establishes completion of that relay’s stored results, not proof of global freshness.
- Immediate revocation is guaranteed locally and on receipt, not across disconnected hosts.

### External dependencies

- Pool filter ownership and lifecycle fixes block dynamic management subscriptions.
- Signer operations may not expose usable cancellation in every backend; operation generations prevent stale results, but shutdown must still join bounded operations.
- Grotto’s installed panel library is usable; its service adapter’s installability must be verified.
- NIP-05’s test-oriented TLS bypass must not weaken production lookup.
- Host signaling/FIPS key equality is a deliberate first-release limitation, not a solved general identity-binding scheme.

### Platform scope

Linux management is the first release. macOS permissions remain a separate platform investigation; shared-core security improvements must not be described as solving Screen Recording or Accessibility onboarding.

## 6. Implementation order

Each work item below should be filed separately, linked to `md-70v`. Names are proposed issue titles, not pre-existing bead IDs.

### Phase 0 — Land the spike and corrected contracts

**Work item: “Specify management UX, authority boundaries, and session v2”**

- Update the two documentation files only.
- Preserve Background and mark specification/implementation discrepancies.
- Record schemas, protocol incompatibility, platform scope, and privacy warnings.

**Verification:** Design review traces one first connection, one unattended connection, one offline revoke, and one identity mismatch without unresolved authority or ownership decisions.

### Phase 1 — Upstream protocol building blocks

1. **“Fix nostrc subscription ownership and expose EOSE/CLOSED lifecycle”**
   - Independent filter lifetime, handles, public relay access, bounded publication.
   - Verify multiple live subscriptions, relay reconnect, CLOSED/AUTH, cancellation, and teardown under sanitizers.

2. **“Add signer-independent NIP-51 serialization and reusable NIP-78”**
   - Unsigned builders; private-entry plaintext seams; installed component metadata.
   - Verify raw-key compatibility wrappers and external-signer round-trips.

3. **“Make Grotto’s Control1 client adapter consumable by companion applications”**
   - Validate existing packaging first; extract only if necessary.
   - Verify Nostr Settings/Grotto consumers and standalone panel hosting still build.

These can land without changing metadesk runtime behavior.

### Phase 2 — Shared state, identity, and routing

4. **“Add encrypted management store and deterministic replacement/conflict handling”**
   - Schemas, owner-separated persistence, cache/outbox, safety latch.
   - Verify no private plaintext or credentials on disk and crash-safe retry.

5. **“Add canonical host resolution and NIP-65/NIP-17 relay planning”**
   - NIP-05/19/65 linkage, kind-10050 support, explicit bootstrap configuration.
   - Verify key pin mismatch, no-relay state, routing roles, and unsafe discovery rejection.

6. **“Harden signer/bridge concurrency, DM authentication, and teardown”**
   - Serialized signer calls, live identity check, bounded unwrap, validation/dedup.
   - Verify blocking signer cancellation, duplicate DMs, randomized gift-wrap timestamps, and producer-before-consumer shutdown.

**Phase gate:** Headless document load/edit/publish works through a fake external signer and local relay, with deterministic failure/recovery tests.

### Phase 3 — Secure host/client session control

7. **“Implement host approval policy and capability-scoped grants”**
   - `MdHostAccess`, load states, deadlines, local safety latch, no-UI denial.
   - Pure state-machine tests compile before entrypoint adoption.

8. **“Adopt correlated session v2 and one-use TCP binding”**
   - Strict messages, CSPRNG tokens, candidate-stream gate, client controller.
   - **Atomic:** host/client adoption, session parsers, startup fallback removal, and all send/receive capability gates.
   - Verify the real entrypoints never send video/tree or inject input before binding.

9. **“Expose same-user host management and private client launch channels”**
   - Closed IPC methods, revision/generation checks, subscriptions, inherited FD.
   - Verify unauthorized peers, stale decisions, lost events, child exit, and absence of credentials in process arguments.

**Phase gate:** Automated end-to-end attended approval, unattended authorization, denial, token replay rejection, and revocation.

### Phase 4 — Linux management UI

10. **“Build Linux management shell and signer onboarding”**
    - Adaptive navigation, Grotto identity integration, explicit relay setup.
    - Verify no GTK dependency in host/core/client-only builds.

11. **“Add host collections, connection flow, and scoped approval inbox”**
    - Hosts/favorites/tags, pin review, client launch, access editing, exact-request notifications.
    - Verify keyboard-only operation, 360-pixel layout, offline state, timeout, and stale-decision behavior.

12. **“Integrate responsive connection/reconnect controls into the SDL overlay”**
    - Async progress, explicit actions, input consumption, texture reset, grant display.
    - Verify F1/Escape handling, disconnect versus quit, and no stale input across reconnect.

**Phase gate:** Linux attended and unattended dogfood with real Grotto, operator-selected relays, and FIPS; run offline and signer-loss exercises before enabling unattended use.

### Phase 5 — Migration and release hardening

13. **“Migrate legacy public allowlists through explicit scope review”**
    - Preserve source event; encrypted replacement; explain historical disclosure.
    - Verify empty, duplicate, malformed, mixed-visibility, partial-publication, and rollback cases.

14. **“Add management release matrix and lifecycle/security regression gate”**
    - Linux manager, Linux/macOS CLI/overlay, `client=false`, signer feature combinations.
    - Exercise local relay failures, process restarts, partitions, and resource cleanup.

### Deferred, separately scoped work

- **macOS management frontend and permission onboarding:** reuse the same C controllers; do not introduce GTK as a macOS requirement.
- **Secure 1Password startup integration:** define non-environment bootstrap input and failure behavior before wiring `MdSecrets`.
- **Persistent NIP-46 pairing:** requires an audited credential-storage contract; do not store bunker credentials in NIP-78 or plaintext configuration.
- **Independent host signaling and FIPS identities:** requires an authenticated mapping/proof protocol, not a configurable unchecked key pair.
- **Remote approval and multiple active sessions:** separate authority and resource-allocation designs; neither is implied by signer approval.

### Final decision summary

- **Linux GTK companion now; shared C controllers; streaming overlay remains lightweight.**
- **External signer custody; Grotto reuse without granting signer panels desktop-approval authority.**
- **Private NIP-51 membership/permissions; private NIP-78 metadata; machine operations stay local.**
- **Explicit bootstrap plus role-aware relay routing; no hardcoded public fallback.**
- **Host-authoritative, scoped, correlated approvals and one-use stream binding before any access.**
- **Upstream nostrc ownership/NIP work first; protocol migration and enforcement land atomically.**

The numbered Phase 0–5 items are ready to file as spin-offs from `md-70v`; the spike itself ends with the reviewed design, not a claim that management UX has shipped. The spin-off work items are filed in bd as of 2026-11-21; see §8.1 for the item → bead map.

## 7. Design-review corrections and open decisions (2026-11-14)

Applied from `docs/reviews/management-ux-plan-critique-2026-11-14.md` (code-verified against metadesk + nostrc). These correct or extend the §1–§6 body above; where a correction contradicts body text, the correction wins.

### 7.1 Corrections the code requires
- **Kind constants already exist** — drop the §4.6 `nostr-kinds.h` "add kind if absent" row and §3.7's add-the-kind step: `NOSTR_KIND_DM_RELAY_LIST 10050` (`libnostr/include/nostr-kinds.h:120`) and `NOSTR_KIND_APPLICATION_SPECIFIC_DATA 30078` (`:160`) are defined. metadesk should use them instead of the hardcoded `30000`/`1059` at `nostr.c:394–425`.
- **Pool rescope (P1.1)** — `nostr_simple_pool_set_subscription_callback(...)` (EOSE/CLOSED with `NostrRelay*` identity, `nostr-simple-pool.h:230–234`) and `nostr_relay_publish_and_wait(...)` (default 30 s, `nostr-relay.h:159`) already exist. Only **per-subscription handles/scoping, public relay enumeration, and in-flight publish cancellation** are missing; use `nostr_simple_pool_set_recovery_window()` (`:235`) for reconnect `since` instead of the 300 s arithmetic at `nostr.c:394–399`.
- **Unsigned 10050 builder gap (P1.2/P2.5)** — `nostr_nip17_create_dm_relay_list()` takes `sk_hex` (`nips/nip17/.../nip17.h:192`), the same external-signer problem as NIP-51. Parsing already exists (`nostr_nip17_parse_dm_relay_list`, `get_dm_relays_from_event`, `:203/:227`); only routing + an unsigned builder (or metadesk-side assembly via `MdSigner`) are new.
- **NIP-78 unsigned precedent** — `libporthome/include/porthome/head.h:11,56` already builds an unsigned kind-30078 event; follow its shape for `nostr_nip78_build_event()`.
- **NIP-05 fetch ownership (§3.12)** — the library's TLS/timeout knobs are env vars (`NIP05_ALLOW_INSECURE`, `NIP05_TIMEOUT_MS`; `nip05.h:22–26`) with no redirect/destination hook; "don't expose the insecure option in the GUI" is misdirected. `identity_resolver` must either do its own HTTPS fetch + `nostr_nip05_resolve_from_json()` or a new nostrc API gains explicit timeout/validation; scrub both env vars in `metadesk-manager` and the launched client.
- **IPC is new, not reused (§4.2)** — `ipc_unix.c` has no `SO_PEERCRED`/`getpeereid` and lays out `$XDG_RUNTIME_DIR/metadesk-<name>.sock` (`:5–9,47–50`), not the plan's `metadesk/host-<hex>.sock`. Peer-cred verification is new code; decide whether to change the path builder or accept the flat layout. (JSON-RPC via `jsonrpc.c` does exist.)
- **Bind payload vs buffer (§3.10)** — the host copies `MD_PKT_SESSION_INFO` into `char json_buf[4096]` and truncates (`host/main.c:421–424`); `session_bind/bound` must size the buffer to the ≤8 KiB limit or set a smaller bound.
- **Cite constants, not literals** — `hosts[pk].port` default 7700 should cite `MD_STREAM_PORT` (`stream.h:35`); name the DM validation primitives `nostr_nip17_validate_gift_wrap()`, `nostr_nip17_validate_seal(seal, rumor)` (`nip17.h:156/168`), `nostr_nip59_get_recipient()` (`nip59.h:168`), `nostr_event_validate()` — today `dm_unwrap_thread` (`nostr.c:234–300`) does no wrap/seal validation and trusts the seal's `sender_pk`.
- **MCP action gate (§3.9)** — MCP stdio/HTTP also dispatch agent actions (`mcp_server.c`); apply the same capability gate there (or state MCP is local-only and unaffected).

### 7.2 Scope/ordering corrections
- **P3.7↔P3.8 cycle** resolved by splitting P3.8 into **P3.8a** (session-v2 types/parsers, prereq of `MdHostAccess`) and **P3.8b** (atomic entrypoint adoption). See the index.
- **Split P4.11** into client-side collections/connect (**ships on session v1**, P4.11a) and the approval inbox + scoped grants (P4.11b, needs P3.7/P3.8b/P3.9) — shortens the critical path to a usable address book.
- **Deliverable location** — `md-70v` asks for `docs/designs/`; this lives in `docs/plans/` (no `docs/designs/` exists). P0 records `docs/plans/` as the chosen home.
- **Cross-link** spin-offs to `md-3nv`, `md-d9y`, `md-aqf` with `bd dep` instead of duplicating.

### 7.3 Requirements newly surfaced (absent from the export)
- **Host identity model** (→ **resolved in §7.6**, 2026-11-16): host signaling key must equal the per-node FIPS key (§3.4) and every doc is keyed `(kind, author, d)` with fixed `d=metadesk-allowlist` — so a user cannot reuse one identity across machines without colliding on that coordinate. Decide: host identity = **per-machine** (companion is two-identity: user + this-machine) or = user identity (then FIPS provisioned with the user key, multi-host unsupported in v1). This changes `MdManagementConfig`, the Identity/This-Computer pages, and the cache layout.
- **Host process lifecycle/ownership:** nothing says who starts `metadesk-host`, nor behavior at logout/lock/reboot for unattended access. Decide a lifecycle (e.g. systemd user unit with `--managed`, autostart, restart-on-crash) and an IPC "host not running → start host" affordance, or declare it out of scope with a CLI path.
- **Headless/macOS admin path:** removing auto-grant leaves headless and macOS (no GTK in v1) hosts unable to remember clients — `src/host/main.c` has no allowlist flags today. P3.7 adds `metadesk-host access upsert/remove` over the same IPC, or the plan must say headless hosting is unsupported until P4 (contradicting §3.1).
- **NIP-42 AUTH on DM relays:** NIP-17 inboxes typically require AUTH for `#p` REQs; nostrc has `nostr_relay_set_auth_callback/auth(...)` (`nostr-relay.h:163–181`) but the sign callback is synchronous — bridging it to an async `MdSigner` off the reader thread is real design (add to P2.6 with a test).
- **Host identity rotation** invalidates all saved-host pins and (under the per-machine model) requires FIPS re-provisioning — specify an operator-driven "host key replaced" review path, no auto-migration.
- **Approval notification hygiene:** withdraw the desktop notification on expiry and on a received `session_cancel` (`g_application_withdraw_notification`); a received cancel moves the inbox item to `CANCELLED` and cannot be approved after.
- **GTK test fixture:** reuse grotto-panels' `tests/fake-service.c` (+ `GTK_A11Y=test`) and a fake host-IPC server for `test_manager.c`, else P4.10 verification is unfundable.

### 7.4 Deferred from the md-70v brief (explicitly out of first scope)
Address-book **import/export** and **QR scan/display** (Grotto has `sheet-qr-display.c`) fold into P4.11a; **clipboard**, **multi-monitor selection**, **quality settings (resolution/bitrate/fps)**, and an **in-overlay connection manager** are deferred (the overlay stays lightweight) — each to be filed as a separate later item, not silently dropped.

### 7.5 Open decisions (answers change design or order)
1. ~~Host identity model~~ — **resolved 2026-11-16 → §7.6** (user-assigned signer accounts per role).
2. **Resolved W17 / md-pw1q:** opt-in per-user graphical-session agent; no after-logout/pre-login desktop hosting. See [lifecycle contract](../host-lifecycle.md) for prompt-free constraints and platform status.
3. Is headless (macOS/agent-mode) hosting required in the first release? If yes, the CLI admin surface is in P3.7.
4. Are AUTH-required DM relays in scope for v1? If no, the first-run DM-relay picker filters them via NIP-11.
5. Can client-side management UI ship on session v1 (re-cut P4.11a)?
6. Where do the 10050/NIP-78/NIP-51 unsigned builders live — one P1.2 item spanning nip17/nip51/nip78, or metadesk-side assembly via the kind registry?
7. NIP-05 fetch ownership — metadesk HTTPS client + `resolve_from_json`, or a nostrc API change?
8. One `NostrSimplePool` per concern (DM inbox / own docs / discovery) to sidestep the shared-filter limit and let P2.5/P2.6 start before P1.1, at the cost of duplicate connections under the 32-connection cap?
9. `docs/designs/` vs `docs/plans/` as the canonical home for this deliverable.

### 7.6 Decision: signer accounts with user-assigned roles (2026-11-16)

**Direction (user):** metadesk supports a signer interface like Groundhog and GNostr and lets the user decide which key is used for what. This resolves §7.5 Q1 and **replaces** the single pinned identity in §3.4/§3.5 (`expected_signer_pubkey`, "pinned signer identity").

**Prior art in nostrc (verified 2026-11-16):**
- **Groundhog — the reference model.** `GhIdentityInfo` = npub + label + backend (`GROTTO`|`NIP46`) (`gnome/groundhog/src/identity/gh-identity.h:8–24`). The GTK-free `GhAccountController` selects by backend + npub, revokes the old generation on switch, and binds a `GhSigner` (`gh-account-controller.h:84–107, 145–160`; `.c:451–548, 953–1000`). The active pair is persisted as GSettings `current-backend` + `current-npub` (`data/org.nostr.Groundhog.gschema.xml:11–24`). NIP-46 client secret / remote pubkey / relays live in Secret Service (Linux) or Keychain (macOS) via `GhNip46CredentialStore`, keyed by user pubkey (`src/identity/gh-nip46-credentials.h:9–72`). Local keys stay in Grotto; removing an account deletes only remote credentials (`src/app/gh-account-ui.c:615–684`).
- **GNostr — weaker variant.** A `known-accounts` npub list plus one global `GnostrSignerService` (`NONE`|`NIP55L`|`NIP46`); NIP-46 secrets are stored in GSettings, not libsecret (`apps/gnostr/src/ipc/gnostr-signer-service.h:60–117`; `src/ui/gnostr-login.c:1486–1521`). Not the pattern to copy for credential storage.
- Neither app supports NIP-5F, and neither has per-purpose key choice (one active account). `nostr-gobject` only supplies the D-Bus proxy; there is no shared account library.

**Design:**
- **Account** = `(backend, pubkey, label)`. Backends: NIP-55L/Grotto, NIP-5F socket, NIP-46 (`bunker://`, `nostrconnect://`) — exactly what `MdSigner` already implements (`src/core/signer.h:127–163`), so metadesk is a superset of Groundhog's transports. Direct-key remains test/explicit-integration only (no native key vault — unchanged).
- **Roles**, each bound by the user to one account:
  - **client** — signs session requests; owns the address book, favorites, tags, and NIP-78 app data (the owner-separated cache is keyed by this account).
  - **host** (per machine) — the identity `metadesk-host` signs as; owns `metadesk-allowlist` and receives session requests.

  The same account may fill both roles. `MdManagement` holds up to two `MdSigner` instances, shared when both roles bind the same account; per-signer serialization (§3.4) is unchanged.
- **Constraints become validation, not mandates.** When hosting over FIPS, the host-role account must equal the local FIPS node pubkey reported by `show_status` (§3.4): the picker suggests that account and blocks managed FIPS hosting on mismatch with a diagnostic. Direct-IP hosting accepts any account. Because FIPS node keys are per node, the host identity is effectively per machine, so `metadesk-allowlist` coordinates do not collide across machines (reusing one FIPS node key on two machines is unsupported).
- **Persistence.** Role bindings are `(backend, pubkey)` pairs in local `management.json` (replacing `signer_backend` / `expected_signer_pubkey` in §3.6). NIP-46 credentials go in Secret Service / Keychain following `GhNip46CredentialStore`, keyed by account pubkey — never in NIP-78, config, argv, or plaintext. This **moves "Persistent NIP-46 pairing" (§6 Deferred) into scope.**
- **Switching / removing.** Rebinding a role revokes that role's previous generation (Groundhog pattern), cancels its sessions and document operations, then loads the new owner's cache. Removing an account deletes only metadesk-held remote credentials, never signer-held keys.
- **Headless hosts.** The same bindings are settable from the host CLI (e.g. `metadesk-host account bind host <backend> <pubkey>`), so headless/macOS hosts do not depend on the GTK companion (addresses part of §7.3 / §7.5 Q3).
- **Reuse strategy.** Mirror Groundhog's model in metadesk's plain-C core (`src/core/accounts.c/.h`, cross-platform; GObject/GSettings stay out of core) and use Groundhog's account UI and credential store as the GTK reference. *Alternative considered:* extract Groundhog's GTK-free controller + credential store into a shared nostrc library — more reuse, but GObject-based (Linux-only) and cross-repo; revisit if a third app needs it.

**Effects:** new work item **P2.7** (index); P3.7 consumes the host-role binding; P4.10 provides the account list and role picker; §4.2 gains `src/core/accounts.c/.h` and `tests/test_accounts.c`.



## 8. Spike close-out (2026-11-21)

This section completes the `md-70v` deliverable against every item in the bead and records what has shipped since §1–§7 were written. Where it conflicts with earlier sections, §8 wins (same rule as §7).

### 8.1 Status of the execution index

| Item | Status | Where |
|---|---|---|
| P0 spec | **Done** with this section | this document |
| P1.1 pool capabilities | Partly shipped (AUTH-OK recovery `md-1be`, CLOSED-aware backoff `md-odg`, per-callback info API); handles/enumeration/publish-cancel remain | **`md-b970`** |
| P1.2 unsigned builders | NIP-51 shipped upstream (`nostr_nip51_document_build_unsigned`, `nip51.h:102`); NIP-78 + 10050 remain | **`md-055m`** |
| P1.3 Grotto Control1 adapter | Open | **`md-crk7`** |
| P2.4 encrypted management store | Open | **`md-v3oh`** |
| P2.5 resolution + relay planning | Open; the binaries still fall back to built-in `MD_DEFAULT_RELAY` | **`md-2l87`** |
| P2.6 signer/bridge hardening | **Done** | `md-70v.1` |
| P2.7 signer accounts + roles | **Done**, incl. live adoption and persistent NIP-46 | `md-70v.2`, `md-70v.3`, `docs/accounts.md` |
| P3.7 host policy + headless CLI | Headless `metadesk-host access list/add/remove` + fail-closed ACL + per-client caps **shipped**; attended approval, pending requests, session list/kick remain | `md-pss`, `md-q6m`, `md-ikr`, `docs/access.md` → **`md-trex`** |
| P3.8a/b session v2 + binding | **Superseded/done** in a different shape: one-use stream binding (`md-mrw`, `docs/stream-binding.md`) + request correlation (`md-6ns`); NIP-42 AUTH (`md-xcy`) | — |
| P3.9 host IPC + client launch | Open (no peer-cred IPC yet) | **`md-trex`** (IPC), **`md-vb18`** (launch) |
| P4.10 manager shell | Open | **`md-fbg4`** |
| P4.11a collections + connect | Open | **`md-vb18`** |
| P4.11b approval inbox + access UI | Open | **`md-a9f2`** |
| P4.12 overlay connect/reconnect | Open; related `md-3nv` | **`md-hx89`** |
| P5.13 allowlist privacy migration | **Promoted to P2**: see §8.2 | **`md-bmiw`** |
| P5.14 release matrix | Folded into `md-5zi` (per-platform E2E) plus each bead's acceptance | `md-5zi` |
| §7.4 deferred parity items | Filed | `md-vex4` clipboard, `md-wvd2` quality, `md-qb6t` monitors, `md-l35t` overlay switcher |
| macOS frontend (deferred) | Filed | `md-pep3` |
| Host lifecycle (§7.5 Q2) | Filed, shared with packaging | `md-pw1q` |

### 8.2 Divergences between this design and what shipped

1. **The host allowlist is public.** `docs/access.md` documents the shipped list as a *public* NIP-51 kind-30000 `d=metadesk-allowlist` with `p` + `metadesk-caps` tags, so the npub of every authorized client and its capabilities are visible on relays. §3.6 specified NIP-44 self-encrypted private entries. The shipped list is therefore the "legacy public" case in §3.6, and migration is now a P2 item (`md-bmiw`). The MCP NIP-98 ACL (`md-ah9.3`) reuses the same list and must move with it.
2. **The default relay is still silent.** §3.1 says "no silent public relay fallback", but both binaries default to `MD_DEFAULT_RELAY`. That is acceptable for the CLI. The manager must require explicit first-run relays (`md-2l87`).
3. **Ephemeral client identities remain** on the legacy no-role path (`md-70v.3`). That is fine for the CLI. Managed connections must require a bound client role (note added to `md-fbg4`).
4. **Persistent NIP-46 pairing shipped** (Keychain/Secret Service credential store, `md-70v.2`). The §3.4 item 3 "in memory only" statement is obsolete.

### 8.3 Bead checklist → design coverage

Every item in the `md-70v` description, with where it is designed and which bead builds it. The *new design* rows fill the gaps left by §1–§7.

| Bead item | Design | Build |
|---|---|---|
| Generate / import keys | Delegated to the signer (Grotto/Groundhog/bunker); metadesk never creates a durable key (§3.4). The manager's Identity page offers "Create/Import in signer" (`open_in_signer`) and "Add account" (§7.6) | `md-fbg4` |
| **Rotate keys** *(new)* | Two cases. **Client rotation:** bind the new account to the client role (§7.6). Hosts know the old npub, so the old host grants are orphaned. The manager lists the saved hosts to re-request from, and host admins see "replace client key" (atomic remove-old/add-new with the same caps). **Host rotation:** requires FIPS re-provisioning (§3.4). Every client holding the old pin hits the *host key changed* block (below), and there is no auto-migration (§7.3) | `md-a9f2`, `md-vb18` |
| Secure at-rest storage | Keys stay in the signer. NIP-46 transport secrets live in Keychain/Secret Service (shipped). `op://` (`MdSecrets`) remains for CLI operational secrets such as MCP tokens and is **not** a key store; 1Password as a signer backend is out of scope | shipped / — |
| **Unlock flow** *(new)* | metadesk has no vault, so there is **no metadesk password**. "Locked" is a signer state, surfaced as a persistent banner with one action. **Grotto/NIP-55L locked:** "Unlock in signer". **NIP-46 bunker unreachable or awaiting approval:** pending, with Cancel. **NIP-5F socket missing:** "Start signer". **OS credential store locked:** the OS prompt appears on first credential read; a refusal shows a typed error and leaves the role unbound. A headless host with a locked/unavailable signer fails closed at startup with a diagnostic (shipped) | `md-fbg4` |
| **Display npub / fingerprint** *(new)* | Everywhere a key appears (This Computer, TOFU review, Access list, `metadesk-host account`/`access list`), show the NIP-19 npub plus a **verbal fingerprint**: the first 16 hex chars of the pubkey in four 4-char groups (e.g. `3bf0 c63f cb93 4634`). Also show an `nprofile` QR on the host. One shared formatter in core (`md_key_fingerprint`), so CLI and GUI match | `md-vb18`, `md-fbg4` |
| Signer selection (46 / 55L / 5F / local) | Signer accounts + roles (§7.6). Local raw key is test-only | shipped |
| **Enter / scan npub, NIP-05, or IP** *(new rules)* | Accepted inputs: `npub`, `nprofile`, 64-hex, `name@domain`, or a QR of `nprofile`. **npub / nprofile / hex:** the user supplied the key; show the fingerprint for confirmation and pin it. **`name@domain`:** a NIP-05 lookup produces a candidate key, and the first save is a TOFU review (fingerprint, domain, verified-at time). Later lookups that disagree with the pin are a hard block. **Raw IP/hostname is a route hint, never an identity.** Since `md-mrw` there is no unauthenticated direct mode, and the TCP peer cannot be identified without its key. An IP entry must be paired with a key (paste or scan from the host's This Computer page). Rejected alternative: TOFU on a key announced over raw TCP, because an on-path attacker could present any key | `md-2l87`, `md-vb18` |
| Trust-on-first-use prompt | As above. The saved host's key **is** the pin (§3.6). The *host key changed* screen shows old and new fingerprints and offers only **Keep old (cancel)** or **Review and replace**, which requires typing/confirming the new fingerprint. No "connect anyway" | `md-vb18` |
| Saved credentials | Nothing secret is stored per host: authentication is the client-role key plus the host's grant. "Saved" means pinned key + label + route hints + requested caps (NIP-78) | `md-v3oh` |
| **Connection status / errors** *(new taxonomy)* | One typed phase/error enum in core, rendered by the overlay, the manager and CLI stderr. The phases are: **resolve** (NIP-05 fetch failed / mismatch); **relay** (connect failed, `AUTH` required/failed, `CLOSED` reason, publish `OK false`); **signer** (locked, denied, timeout); **negotiation** (no reply before deadline, or explicit deny); **FIPS** (daemon missing, not ready, route to npub not found); **bind** (token rejected / expired); **media** (decoder, display). "No reply" deliberately does not distinguish "host offline" from "not allowlisted", to avoid an authorization oracle | `md-hx89`, `md-vb18` |
| Named saved hosts | NIP-51 `metadesk-hosts` + NIP-78 `label` (§3.6) | `md-v3oh` |
| **Last-connected** *(new)* | **Local only**: retention-bounded recent records (§3.6), not synced. Syncing would publish a usage timeline (even encrypted, `created_at` and replacement cadence leak activity). Shown as "last connected on this device" | `md-v3oh` |
| Tags, favorites | NIP-51 `metadesk-tag:<uuid>`, `metadesk-favorites` + NIP-78 tag labels | `md-v3oh` |
| Per-host capabilities | NIP-78 `requested_caps` (client side) and host grants (host side, `md-ikr`) | `md-v3oh`, shipped |
| **Per-host notes** *(new field)* | Add `hosts[pk].notes`: UTF-8, ≤ 2 KiB, default `""`, inside the NIP-44-encrypted NIP-78 document (§3.6 schema, version stays 1 since unknown fields are preserved) | `md-v3oh` |
| Quick-connect | Manager Hosts page: favorites first, then recents. In the overlay, a switcher only (`md-l35t`) | `md-vb18`, `md-l35t` |
| **Import / export** *(new format)* | Versioned JSON `{"schema":"metadesk-hosts/1","hosts":[{"pubkey","label","nip05","relay_hints","requested_caps","tags","notes"}]}`. No secrets and no grants. Import puts each new key through the TOFU review in bulk (list with fingerprints, select all after review). Export warns that the labels and notes in the file are plaintext | `md-vb18` |
| Host: approve / deny incoming | Attended flow: pending request → approve once / approve and remember with exact caps / deny. Bounded queue, deadlines, no-UI default deny (§3.9) | `md-trex`, `md-a9f2` |
| NIP-51 allowlist management UI | CLI shipped. GUI Access page; private-entry migration first | `md-pss`, `md-bmiw`, `md-a9f2` |
| **Session list / kick** *(new)* | Host IPC methods `session.list` / `session.kick(session_id)` and CLI `metadesk-host session list|kick`. Kick closes the bound stream, burns its token and cancels pending input. "Kick and revoke" also removes the allowlist entry | `md-trex`, `md-a9f2` |
| Per-client granted capabilities | Shipped (`md-ikr`); UI shows the effective grant (`requested ∧ granted ∧ local restrictions`) | shipped / `md-a9f2` |
| Settings (resolution / bitrate / fps) | Client preferences in the session request, clamped by host policy, persisted per host in NIP-78 | `md-wvd2` |
| Reconnect UX | Typed phases above, explicit retry/cancel, no stale input | `md-hx89` (+ `md-3nv`) |
| Clipboard | New `clipboard` capability, off by default, text-first | `md-vex4` |
| Multi-monitor selection | Host output list in session info + capability-gated switch | `md-qb6t` |
| In-overlay connection manager | **Decision:** a quick switcher only. Editing stays in the manager, so the overlay stays lightweight (§3.1) | `md-l35t` |
| Where it lives | GTK4/libadwaita manager on Linux; SDL/ImGui overlay for in-session; CLI for headless; native macOS later (§3.1, `md-pep3`). Packaging consequences: Flatpak-first manager, no GTK in the macOS/Windows bundles (`docs/plans/packaging-2026-11-21.md` §2) | — |
| Survey 2–3 tools | §3.2 + parity matrix below | — |
| Nostr primitive per feature, relay strategy, offline/first-run | §3.3, §3.8, §3.12 + this table | — |

### 8.4 Parity matrix and adopted patterns

Columns summarize **publicly documented** behavior (product docs as cited in §3.2), not a fresh hands-on evaluation; edition-specific details vary.

| Capability | RustDesk | Parsec | AnyDesk | Moonlight/Sunshine | metadesk today | metadesk plan |
|---|---|---|---|---|---|---|
| Identity | numeric ID + password / one-time password | account login | numeric address/alias + account | host PIN pairing | Nostr key via signer roles | same; fingerprint + QR (`md-vb18`) |
| First-contact trust | ID + password | account/friend | address + accept prompt | 4-digit PIN pairing | key pin by construction | TOFU review for NIP-05; key changed = hard block |
| Attended accept with permission toggles | yes | yes (keyboard/mouse/gamepad) | yes | n/a | no (allowlist-only) | `md-trex`, `md-a9f2` |
| Unattended access | permanent password | yes (owner's computers) | unattended password | paired clients | allowlist + caps (shipped) | private allowlist (`md-bmiw`) |
| Address book / favorites / tags | yes | computer list | yes (address books, tags) | host list | none | `md-v3oh`, `md-vb18` (synced, encrypted) |
| Session list / kick | yes | yes | yes | yes | no | `md-trex` |
| Clipboard | yes | yes | yes | limited | no | `md-vex4` |
| Monitor switch | yes | yes | yes | per app/display | host-side only | `md-qb6t` |
| Quality settings | client-side | client-side | client-side | client-side | host CLI only | `md-wvd2` |
| Reconnect | auto | auto | auto | manual | basic (`md-d9y`); reliability `md-3nv` | `md-hx89` |
| Installable packages | all platforms | all platforms | all platforms | all platforms | source only | `md-kb7` follow-ups |

**Adopted patterns:**
- RustDesk/AnyDesk: separate quick connect, saved hosts and access configuration.
- Parsec: permission toggles on the accept prompt.
- Moonlight: an explicit pairing step, via fingerprint compare/QR instead of a PIN.
- AnyDesk: naming kept separate from authorization.

**Rejected patterns:**
- Passwords or one-time codes. A shared secret is weaker than key-based allowlists and needs a server or rendezvous to be meaningful.
- Central accounts and IDs. Nostr keys and NIP-05 replace them (§3.2).

### 8.5 Recommendation

1. Keep the architecture of §1/§3.1, with signer accounts and roles per §7.6, both already shipped.
2. Fix the security/privacy gap next: private allowlist migration (`md-bmiw`) and attended approval + session control over authenticated IPC (`md-trex`). Headless hosts become complete before any GUI exists.
3. Then build the client-side data plane (`md-055m` → `md-v3oh`, `md-b970` → `md-2l87`) and ship the manager's address book/connect flow (`md-fbg4` → `md-vb18`) on the current session protocol.
4. Add the host-side GUI (`md-a9f2`) and the overlay work (`md-hx89`) last. Parity features (`md-vex4`, `md-wvd2`, `md-qb6t`, `md-l35t`) are independent and can interleave.

### 8.6 Remaining phases

| Phase | Beads | Gate |
|---|---|---|
| A — security/privacy on the shipped host | `md-bmiw`, `md-trex` | private relay + real signer tests; Nostr Protocol Review; headless approve/deny/kick E2E |
| B — upstream building blocks | `md-b970`, `md-055m`, `md-crk7` | nostrc sanitizer-clean tests |
| C — client data plane | `md-v3oh`, `md-2l87` | headless load/edit/publish via fake signer + private relay; NIP-05 pin-mismatch tests |
| D — Linux manager | `md-fbg4`, `md-vb18`, then `md-a9f2` | GTK smoke with fake Control1 + fake host IPC; keyboard/360px; offline |
| E — in-session UX + parity | `md-hx89`, `md-vex4`, `md-wvd2`, `md-qb6t`, `md-l35t` | per-bead; `md-5zi` matrix |
| F — platform | `md-pep3` (macOS frontend), `md-pw1q` (lifecycle) | packaging (`md-kb7`) |

### 8.7 Open decisions (updated)

| # | Question | Status |
|---|---|---|
| 1 | Host identity model | **Resolved** (§7.6) |
| 2 | Host process lifetime / unattended after logout | **Open** → `md-pw1q` (shared with packaging) |
| 3 | Headless hosting in first release | **Resolved:** yes, CLI shipped (`md-pss`); approvals/kick added in `md-trex` |
| 4 | AUTH-required DM relays in v1 | **Resolved:** in scope (`md-xcy`, `md-1be`) |
| 5 | Client UI on the current session protocol | **Resolved:** yes (`md-vb18`) |
| 6 | Where unsigned builders live | **Resolved:** upstream nostrc (NIP-51 done; `md-055m`) |
| 7 | NIP-05 fetch ownership | **Recommended:** metadesk-owned HTTPS + `nostr_nip05_resolve_from_json` (`md-2l87`); revisit if nostrc adds an explicit-options API |
| 8 | One pool per concern vs per-subscription handles | **Open**; default to waiting for `md-b970`, fall back to separate pools only if it slips |
| 9 | `docs/designs/` vs `docs/plans/` | **Resolved:** `docs/plans/` |
| 10 | Allowlist migration ordering: does the MCP NIP-98 ACL (`md-ah9.3`) migrate in the same change? | **Open**; recommend the same change (`md-bmiw`) |
| 11 | Sync last-connected across devices? | **Recommended no** (privacy; §8.3) |
| 12 | IP-only connect without a key | **Recommended no** (no TOFU from raw TCP; §8.3) |
| 13 | macOS management frontend timing relative to the notarized DMG | **Open** → `md-pep3` / `md-wdi3` |

With §8, the spike deliverable is complete: a concrete recommendation (§8.5), a phased plan with filed beads (§8.1, §8.6), and the open decisions (§8.7).

### 8.8 Status update (2026-11-24, release epic `md-w9sc`)

The items §8.1 listed as open were implemented under release epic `md-w9sc` (target tag `v0.1.0-alpha.2`). Where this table conflicts with §8.1, this table wins.

| Item | Bead | Status | Where | Carried forward |
|---|---|---|---|---|
| P2.4 encrypted management store | `md-v3oh` | Done | `src/core/managed_store.*`, `docs/managed-store.md` | — |
| P2.5 resolution + relay planning | `md-2l87` | Done, narrowed | `src/core/identity_resolver.*`, `relay_plan.*`, `discovery_http.c` | `md-dc2m`: connect-time destination enforcement needs an upstream nostrc dial policy; discovered relays are validated at selection and pool hand-off only |
| P4.11a collections + connect | `md-vb18` | Done | `src/manager/hosts-page.c`, `src/core/managed_store_controller.*`, `managed_launch.*`, `docs/manager.md` | `md-7c86`: remaining keyboard matrix, real-client hand-off test, camera QR scan, Connect gated by a stalled storage write |
| Overlay quick-connect switcher | `md-l35t` | Done | `src/client/ui.cpp`, `src/client/main.c` | `md-zf1y`: overlay switches do not record recents |
| macOS frontend (deferred item) | `md-pep3` | Prototype done | `docs/plans/management-macos.md`, `src/macos_manager/` (`-Dmacos_manager=true`) | `md-qgxe`: live sync/edit, connect launch, approvals, role binding; never launched on screen |
| P5.14 release matrix | `md-5zi` | In progress | `docs/testing/results/` | Full host↔client matrix (Omarchy, FIPS, attended visual checks) remains incomplete |

Decisions taken while implementing:

- **§8.7 #8 (pool per concern):** moot; per-subscription handles (`md-b970`) are used.
- **§3.8 connection-time validation:** not achievable at the pinned nostrc revision without an upstream change; tracked in `md-dc2m`, and the code and docs state the remaining time-of-check/time-of-use window.
- **New linked nostrc components:** `nip05`, `nip65`, `nip78` at the unchanged pin.
- **Overlay host list:** the overlay reads the encrypted cache read-only through its own client-role binding on every platform, rather than over the Linux-only launch channel.
