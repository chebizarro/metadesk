# Critique: `docs/plans/management-ux-2026-11-14.md` vs. the context_builder export (md-70v)

**Date:** 2026-11-14 · **Spike:** `md-70v` · **Reviewer scope:** plan vs. export baseline, spot-checked against metadesk + nostrc source.

## Context / scope

- **Baseline:** the generated plan in `prompt-exports/oracle-plan-2026-11-14-142627-build-run-metadesk-c-fd96.md`, lines 113–1148 (after `## Generated Plan` / `## Chat Send`). The composed prompt and selected-file list (lines 1–104) were read as context only.
- **Subject:** `docs/plans/management-ux-2026-11-14.md` (1061 lines). Both documents were read in full.
- **Diff result:** the plan is the export's plan **verbatim** except for (a) a reworded three-line header, (b) a new `## 1.1 Execution index` table (plan lines 11–32), and (c) one added final-summary bullet plus closing sentence (plan lines 1057–1061). Nothing from the export was removed, weakened or generalized. Category 1 below is therefore about the *added* §1.1 and whether it faithfully compresses §4/§6.
- **Spot-checks performed** (load-bearing seams only): `src/core/nostr.c` (`host_on_dm` path, allowlist cache, subscribe filter, destroy order), `src/host/main.c` (grant path, CSPRNG fallback, accept loop, `MD_PKT_SESSION_INFO`), `src/core/ipc_unix.c`, `src/core/session.c`, nostrc `nostr-kinds.h`, `nostr-simple-pool.h`, `nostr-relay.h`, `nip17.h`, `nip05.h`, `nip65.h`, `nip19.h`, `grotto-panels.h`, and the Control1 adapter build wiring.

Severity key: **H** changes a decision or implementation order · **M** must be fixed before filing the work item · **L** wording/precision.

---

## 1. Export content missing, weakened or generalized in the plan

**None.** `diff` of the two bodies shows only additions. Two observations on the additions:

1.1 **(L) §1.1 path shorthand disagrees with §4.6.** P1.1 "Key files" says `libnostr/nostr-simple-pool.h`, `nostr-relay.h`; the real paths (and §4.6) are `libnostr/include/nostr-simple-pool.h` and `libnostr/include/nostr-relay.h`. Use the §4.6 paths in the index so the row is copy-pasteable into a bead.

1.2 **(M) §1.1 dependency graph contradicts §4.2 ordering.** §1.1 has **P3.8 (session v2) depends on P3.7 (host_access)**, while §4.2 says `host_access.c/.h` is ordered after "Session v2 and policy snapshots" — i.e. P3.7 depends on P3.8. Both can't be true. Resolution: split "session v2 **types/parsers** in `session.c/.h`" (a prerequisite of `MdHostAccess`, since the state machine carries `request_id`/`stream_token`/deadlines) from "session v2 **entrypoint adoption**" (the atomic host/client switch). P3.7 then depends on the former; P3.8 is only the latter. Same fix resolves P3.9's `client/main.c` edits (inherited FD) that otherwise land before the client controller exists.

---

## 2. Under-specified seams, unresolved decisions, contradictions, wrong references, missing dependencies

2.1 **(H) Kind-10050 publishing has the same external-signer problem as NIP-51, and it is not in P1.2.** The plan says "publish kind 10050 through the signer" (§3.8) and "Add kind-10050 metadata handling to the bridge." nostrc already has `nostr_nip17_create_dm_relay_list(const char **relays, const char *sk_hex)` and `nostr_nip17_parse_dm_relay_list()` (`nips/nip17/include/nostr/nip17/nip17.h:192–203`). The builder takes a raw secret key, exactly the shape the plan rejects for NIP-51 (Background §2 item 6). Either P1.2 must grow an unsigned 10050 builder in `nip17`, or P2.5 must build the event itself with `NOSTR_KIND_DM_RELAY_LIST` and sign via `MdSigner`. Pick one and record it; today P2.5 has an unstated upstream dependency.

2.2 **(H) NIP-05 lookup cannot meet the stated bounds/validation with the current API.** §3.12 requires "bounded to ten seconds and HTTPS with verified TLS … Validate DNS/redirect targets to avoid requests to local/private services." `nip05.h:22–26` shows the library's knobs are **environment variables** (`NIP05_TIMEOUT_MS`, `NIP05_ALLOW_INSECURE=1`) and `nostr_nip05_lookup()` exposes no redirect/destination hook. The plan's "do not expose the library's insecure-test option through the GUI" is misdirected: the real risk is a child process *inheriting* `NIP05_ALLOW_INSECURE` from the user environment. Required decision: (a) have `identity_resolver` perform its own HTTPS fetch with destination policy and call `nostr_nip05_resolve_from_json()` (no network in nostrc), or (b) add a nostrc API with explicit timeout/validation and make it a P1 item. Also: scrub both env vars in `metadesk-manager` and the launched client.

2.3 **(M) IPC claims "reuse" of infrastructure that does not exist yet.** §4.2 says `ipc.h/ipc_unix.c`: "Reuse/extend framing and peer-credential support." `src/core/ipc_unix.c` has no `SO_PEERCRED`/`getpeereid` code and lays sockets out flat as `$XDG_RUNTIME_DIR/metadesk-<name>.sock` (`ipc_unix.c:5–9, 47–50`), not the plan's `$XDG_RUNTIME_DIR/metadesk/host-<host_hex>.sock`. Peer-cred verification is **new** code, and the proposed directory layout requires either changing `md_ipc_listen()`'s path builder or accepting the existing flat layout. JSON-RPC does exist (`src/core/jsonrpc.c/.h`, used by MCP), so that half of the claim holds.

2.4 **(M) Bind payload size vs. existing receive buffer.** §3.10 sets "Session JSON ≤ 8 KiB", but the host's `MD_PKT_SESSION_INFO` handler copies into `char json_buf[4096]` and silently truncates (`src/host/main.c:421–424`). The `session_bind`/`session_bound` handlers must either define a smaller bound or size the buffer to the limit; state which.

2.5 **(M) `hosts[pk].port` default 7700 is right but should cite `MD_STREAM_PORT` (`src/core/stream.h:35`)** rather than a literal; otherwise the schema default and the stream default can drift.

2.6 **(M) Deliverable location contradicts the task.** `md-70v` says "Deliverable: a design doc (**docs/designs/**)". The plan lives in `docs/plans/` and §4.1 says it stays there. Either move it or record in P0 why `docs/plans/` is the chosen home (there is currently no `docs/designs/` directory either).

2.7 **(M) Missing cross-links to existing beads that overlap spin-offs.** Open/known issues `md-3nv` (host streaming unreliable across client reconnects), `md-d9y` (client reconnection logic), `md-aqf` (test-suite fake-behavior overhaul) overlap P3.8/P4.12 and §3.14's "do not rely on 'pool initialization skipped' tests." The §6 items should be filed as `bd dep` relations to those, not as fresh duplicates.

2.8 **(L) `src/core/agent.c/.h` "screenshot requires AGENT and VIDEO".** `action.h:42` has a screenshot region, so the operation exists; fine. But §3.9 omits the MCP HTTP/stdio path (`src/core/mcp_*.c`), which also dispatches agent actions and therefore needs the same capability gate. Add `mcp_server.c` to §4.2 or state that MCP is local-only and unaffected.

2.9 **(L) "Use nostrc validation/hash primitives" (§3.10) should name them:** `nostr_nip17_validate_gift_wrap()`, `nostr_nip17_validate_seal(seal, rumor)` (`nip17.h:156, 168`), `nostr_nip59_get_recipient()` (`nip59.h:168`), and `nostr_event_validate()` (already used for the allowlist at `nostr.c:358`). Today's `dm_unwrap_thread` (`nostr.c:234–300`) performs **no** signature validation on wrap or seal and takes `sender_pk` from the seal unverified — the finding is correct; the fix is one call each.

---

## 3. Details disproved by code, not required by md-70v, or replaced by a simpler named design

3.1 **(H → remove work) Kind constants already exist; §4.6's `nostr-kinds.h` row and §3.7's "add the kind … if absent" are no-ops.** `libnostr/include/nostr-kinds.h:120` defines `NOSTR_KIND_DM_RELAY_LIST 10050` and `:160` defines `NOSTR_KIND_APPLICATION_SPECIFIC_DATA 30078`. Drop the row; have metadesk use these constants (it currently hardcodes `30000`/`1059` in `nostr.c:394–425`).

3.2 **(H → rescope P1.1) The pool already has an EOSE/CLOSED observer with relay identity and a bounded publish.** `nostr_simple_pool_set_subscription_callback(pool, cb(NostrRelay*, state, reason, ud), ud)` (`nostr-simple-pool.h:230–234`) and `nostr_relay_publish_and_wait(relay, event, timeout_ms, err)` (`nostr-relay.h:159–160`, default 30 s) exist. What is genuinely missing is **per-subscription scoping/handles** (the pool keeps one shared filter set — `subscribe_async` doc at `:250–258`), public relay enumeration, and *cancellation* of an in-flight publish. Rewrite P1.1's "done when" to those three; otherwise the upstream item is over-scoped by roughly half. Also note `nostr_simple_pool_set_recovery_window()` (`:235–240`) as the correct knob for the reconnect `since`, instead of the metadesk-side 300 s arithmetic at `nostr.c:394–399`.

3.3 **(M) "No implementation/header exists" for NIP-78 is true as a NIP library but nostrc has an in-tree precedent.** `libporthome/include/porthome/head.h:11, 56` builds an **unsigned** kind-30078 event. Cite it as the shape to follow for `nostr_nip78_build_event()`; it also confirms the "builders create unsigned events" decision is consistent with existing nostrc practice.

3.4 **(M) "Add kind-10050 metadata handling to the bridge" overstates new work.** Parsing exists (`nostr_nip17_parse_dm_relay_list`, `nostr_nip17_get_dm_relays_from_event`, `nip17.h:203, 227`). Only routing + the unsigned builder (see 2.1) are new. Reword P2.5 to "link nostrc nip17 DM-relay-list API; add unsigned builder."

3.5 **(M) Session v2 + one-use stream binding + capability enforcement is scope md-70v does not ask for — justified, but it should be filed as its own hardening epic, not as a UX spin-off that gates UX.** The task's host-side bullets are "approve/deny incoming clients, NIP-51 allowlist UI, session list/kick, per-client granted capabilities." The plan's finding #3 (`MD_PKT_INPUT`/`ACTION`/video not gated; confirmed: initial tree is sent at `host/main.c:1083–1084` immediately after `accept`, before any `SESSION_INFO`, and the `SESSION_INFO` handler at `:416–437` never compares `session_id`) correctly shows an approval dialog alone would be theatre. But §1.1 makes **P4.11 (Hosts/favorites/tags/connect flow) depend on P3.8 (L-sized protocol change)**. The client-side pages (Hosts, Identity, Relays, saved-host collections) depend only on P2.x and can ship against v1 negotiation; only the **approval inbox** and the **grant display** need P3.7/P3.8/P3.9. Split P4.11 into "collections + connect flow" (depends P2.*, P4.10) and "approval inbox + scoped grants UI" (depends P3.7, P3.9). This shortens the critical path to a usable address book by one L item.

3.6 **(L) Correct, keep:** `MD_DEFAULT_RELAY` at `nostr.h:45`; `host_on_dm` at `host/main.c:113–147`; `session_req_ch` created at `:739` after `md_nostr_create()` at `:725`; 60 s `go_select_timeout` at `:745`; zero-fill CSPRNG fallback at `:787, :792`; auto-grant `VIDEO|AGENT|INPUT` at `:803`; direct-TCP acceptance when no `--npub` at `:844`; `md_nostr_destroy()` frees allowlist (`nostr.c:569–570`) and destroys `allowlist_mu` (`:580`) **before** `go_wait_group_wait(&n->dm_wg)` (`:583`); `md_nostr_allowlist_add()` mutates the cache before signing/publishing (`:845–866`); client puts the signer hex into `fips_addr` (`client/main.c:235`) while `show_status` returns an `npub` (`:190`); ephemeral client signer at `:653–667`; spec `:192` says "C++ core" vs `:12` C17; spec `:615` marks 2.6 ✅; `GrottoIdentityPanel`, `open_in_signer`, `GROTTO_PANEL_SET_PERMISSIONS` all exist (`grotto-panels.h:294–299, 241, 100`); the Control1 adapter is compiled by relative path into nostr-settings (`gnome/nostr-settings/meson.build:22`, `CMakeLists.txt:76`) and into grotto-panels tests (`gnome/grotto-panels/meson.build:67–68`), confirming P1.3. `nostr_nip65_normalize_url` (`nip65.h:259`) and `nostr_nip19_decode_nprofile` (`nip19.h:80`) exist.

---

## 4. Requirements, edge cases, dependencies or architectural problems absent from both export and plan

4.1 **(H) Which identity does the host use, and can one user run two hosts?** §3.4 requires host signaling key == local FIPS node key, and §3.6 keys every document by `(kind, author, d)` with a fixed `d = metadesk-allowlist`. Consequences the documents never draw: (a) **each hosting machine needs its own Nostr identity** (FIPS keys are per node), so "the user's Grotto identity" cannot be the host identity on more than one machine; (b) if two hosts *did* share an author, their permission lists collide on one coordinate; (c) the companion's single `expected_signer_pubkey`, single `GrottoIdentityPanel`, and single "owner-separated cache" assume **one** identity per companion, yet the same companion is supposed to show "This Computer" (host identity) and "Hosts" (user identity). Decide: host identity = per-machine identity (companion is two-identity: user + this-machine), or host identity = user identity (then FIPS must be provisioned with the user key and multi-host is unsupported in v1). This changes `MdManagementConfig`, the Identity page, and the cache layout.

4.2 **(H) Host process lifecycle and ownership are unspecified.** The companion has `host.set_enabled` / `host.set_unattended` over IPC, but nothing says who **starts** `metadesk-host`, whether it is a systemd user service, what happens at logout/lock/reboot for unattended access, or how the companion behaves when the host is not running (banner only). Unattended access that disappears on logout is not unattended. Add a lifecycle decision (systemd user unit with `--managed`, autostart policy, restart-on-crash) and an IPC "host not running" → "start host" affordance or an explicit "not in scope" with a CLI instruction.

4.3 **(H) Headless hosts have no administration path.** §3.9 says "No local approval UI → deny `approval_unavailable`"; spec §2.4 agent mode and the macOS host (no GTK in v1) are headless. `src/host/main.c` has **no** `--allow`/allowlist CLI flags today, and the plan removes auto-grant. Result: a headless/macOS host can serve only remembered clients it has no way to remember. P3.7 needs a CLI admin surface (`metadesk-host access upsert <npub> <caps>` via the same IPC, or flags) or the plan must state that headless hosting is unsupported until P4 — which contradicts §3.1 "macOS retains CLI."

4.4 **(H) NIP-42 AUTH on DM relays.** NIP-17 recommends DM relays require AUTH for `#p`-filtered gift-wrap REQs. The plan only says "CLOSED/AUTH surfaced." nostrc has the hooks (`nostr_relay_set_auth_callback`, `nostr_relay_get_auth_challenge`, `nostr_relay_auth(relay, sign_cb)` — `nostr-relay.h:163–181, 409–421`) but the sign callback is synchronous; bridging it to an external `MdSigner` (D-Bus/socket round-trip, user approval in Grotto) off the relay reader thread is real design work. Without it, incoming session requests on an AUTH-required inbox never arrive and the connect flow fails with a misleading "waiting for approval." Add to P2.5/P2.6 with its own test case.

4.5 **(M) Host identity rotation breaks every client's pin.** §3.12 pins the saved host key and blocks on NIP-05 mismatch. Rotating/replacing a host identity (the task asks for "rotate") invalidates all saved-host entries and, under 4.1, requires FIPS re-provisioning. Specify a "host key replaced" review path (the mismatch dialog is a start) and state that rotation is operator-driven with no automatic migration.

4.6 **(M) Task items silently dropped rather than explicitly deferred.** md-70v lists address-book **import/export**, **scan** an npub (QR), and "general parity: settings (resolution/bitrate/fps), reconnect UX, clipboard, multi-monitor selection, in-overlay connection manager." The plan covers reconnect and declines the in-overlay manager implicitly (overlay stays lightweight) but says nothing about import/export (relay sync is not an export), QR display/scan (Grotto already has `sheet-qr-display.c`), clipboard, multi-monitor, or quality settings. Add a "Deferred" bullet per item so the spike visibly answers its own brief.

4.7 **(M) Approval UX when the host user is away: expiry vs. notification persistence.** Request lifetime is 180 s and attended deadline 120 s. A desktop notification that outlives the request (GNOME keeps them) will open an expired item; §3.9 says "at expiry disable actions" but does not require withdrawing the notification (`g_application_withdraw_notification`). Add it.

4.8 **(M) Testability gap for the GTK layer.** `tests/test_manager.c` "headless GTK smoke" needs a fake `GrottoPanelsService` and a fake host IPC server; grotto-panels already ships `tests/fake-service.c` and runs with `GTK_A11Y=test`, `G_DEBUG=fatal-warnings`. Name reuse of that fake (or extraction with P1.3) as a dependency, or P4.10's verification is unfundable.

4.9 **(L) Cancellation of a pending approval from the client side has no host-side UX rule.** `session_cancel` exists in the protocol; §3.9's inbox should state that a received cancel moves the item to `CANCELLED`, withdraws the notification, and cannot be approved afterwards (the binding rules imply it, the UI section doesn't).

---

## 5. Questions whose answers change the design or order

1. **Host identity model (4.1):** per-machine identity or user identity? Is multi-host-per-user a v1 requirement?
2. **Who owns host process lifetime (4.2):** systemd user service shipped with the manager, or operator-run CLI? Is unattended-after-logout required?
3. **Is headless (macOS/agent-mode) hosting required to keep working in the first release (4.3)?** If yes, the CLI admin surface moves into P3.7 and the spike must specify it.
4. **Are AUTH-required DM relays in scope for v1 (4.4)?** If yes, the external-signer AUTH bridge is a P2 dependency; if no, the first-run DM-relay picker must filter them out via NIP-11 and say so.
5. **Can client-side management UI ship on session v1 (3.5)?** If yes, re-cut P4.11 and the critical path shortens materially.
6. **Where does 10050/NIP-78/NIP-51 unsigned-builder work live (2.1):** one P1.2 item spanning nip17/nip51/nip78, or metadesk-side event assembly using the kind registry? The latter removes an upstream blocker from P2.5.
7. **NIP-05 fetch ownership (2.2):** metadesk HTTPS client + `nostr_nip05_resolve_from_json`, or a nostrc API change?
8. **Single pool vs. multiple pools:** would one `NostrSimplePool` per subscription concern (DM inbox, own documents, discovery) sidestep the shared-filter problem well enough to let P2.5/P2.6 start before P1.1 lands, at the cost of duplicate relay connections within the 32-connection cap? The plan asserts the upstream fix is a hard blocker without weighing this.
9. **`docs/designs/` vs `docs/plans/` (2.6):** which is canonical for md-70v's deliverable?

---

## Recommendations (in order)

1. Resolve Q1–Q3 before filing P3.x; they alter `MdManagementConfig`, the Identity/This-Computer pages and P3.7's surface.
2. Fix §1.1: path shorthand (1.1), P3.7/P3.8 cycle (1.2), split P4.11 (3.5), add bead cross-links (2.7).
3. Rescope P1.1 to the three genuinely missing pool capabilities (3.2); delete the `nostr-kinds.h` row (3.1); fold the 10050 unsigned builder into P1.2 or P2.5 (2.1).
4. Add NIP-42 AUTH, NIP-05 fetch ownership, IPC peer-cred (new, not reuse), notification withdrawal and the headless admin CLI to §3/§4 text with one line each.
5. Add an explicit "Deferred from md-70v brief" list (4.6) so the spike's coverage of its own task is auditable.
