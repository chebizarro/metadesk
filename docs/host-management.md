# Host approval and local management (md-trex / W13)

## Policy and ownership

Default hosting remains **allowlist-only, fail closed**. A missing, empty,
unreadable, legacy/public, or unavailable policy never opens the host. Explicit
`--attended` additionally permits an operator to approve unlisted requests through
local IPC. Starting a management client, connecting a socket, or opening a GUI
never enables attended admission implicitly. Listed clients retain existing
private-policy auto-admission; `--attended` is not a new unattended safety latch.
Use `session kick` to end an active session without changing policy. W16's
local `access.set` / `access.remove` also end that principal's existing admission
authority after the private policy write succeeds. Dynamic relay-driven active revocation and the
plan's persistent unattended latch are not introduced here.

`MdHostAccess` owns a mutex-serialized inbox of at most **16** authenticated,
live, freshness-checked requests, one per sender and request ID. The host's
existing non-evicting replay fence retains every considered fresh ID through
its authenticated lifetime plus clock skew, including refused requests.
Expected-client pin mismatches and relay history never reach the approval queue.
Distinct requests at the bound or while a session is reserved/active are refused.
Multiple pending senders may coexist, but only one may be approved/remembering
or occupy the host at once. Other pending items retain their original deadlines.

Transitions:

- Pending → approved once → reserved session → authenticated one-use bind.
- Pending → remembering → approved only after private publication succeeds.
- Remember publication failure → pending (no grant); explicit operator action
  is required to retry. The operator must reconcile relay policy first.
- Pending/remembering/approved → removed on authenticated `session_cancel`, deny,
  or strict `expires_at` expiry (no skew grace for a local approval).
- Cancel or kick of a reservation/session → revoked → main-loop close and reset.
- Every mutation advances a generation-scoped revision. Host restart changes the
  unpredictable generation. Stale decisions never substitute another request.

Approval requires an explicit nonzero **exact** selection within requested known
bits and local restrictions (`--no-input` removes INPUT). There is no implicit
input/agent grant or default selection in the CLI. Core stream gates remain:
video needs VIDEO, tree needs AGENT, actions need AGENT+INPUT, raw input needs
INPUT. The acceptance DM and bound TCP ACK carry the effective grant. Capture
failure still refuses before creating a usable stream proof. Only the host
thread mutates sessions/tokens; management and Nostr callbacks only change
admission authority and wake it. Approve-once never edits policy.

Remember uses `md_nostr_allowlist_add` with the selected canonical scope: the
saved host signer self-encrypts the NIP-44 private list, and success requires a
positive relay OK plus the unchanged-base check. Legacy/mixed policy requires
explicit `access migrate`, not implicit conversion. Signer/history/write failure
never becomes approval. If cancellation/expiry wins while publication is in
flight, that request cannot be admitted, **but an acknowledged remembered policy
can already be durable**. The response says `cancelled_or_expired_after_publish`;
reread `access list` before deciding whether to remove the grant. A lost OK or
conflicting replacement can likewise already have reached storage. One OK is
not a multi-relay quorum. No automatic retries are performed.

## Process owner

See [host lifecycle](host-lifecycle.md) for `--managed`, session boundaries,
status fields, supervisor registration and exit/restart policy. Managed Unix
startup can recover a checked, refused stale endpoint under a lifetime lock;
interactive startup retains the no-unlink behavior described below.

On macOS, the read-only v1 `host.permissions` request takes no parameters and
returns the same snapshot as `management.snapshot`, plus `permissions` with
boolean `screen_recording` and `accessibility` fields. These are prompt-free
preflights of the responding host process only. No capture, AX tree query or
input test runs. They do not advance the revision, authorize a session, or
indicate capture/input readiness. Recheck explicitly; this is not a permission
subscription. See the [native companion](plans/management-macos.md).

## Startup and CLI

```sh
# Host: choose a short absolute socket path under a canonical, owner-only dir.
# /private/tmp is canonical on macOS; /tmp on Linux.
mkdir -m 700 /private/tmp/my-metadesk-host
metadesk-host --attended --management-socket /private/tmp/my-metadesk-host/control.sock \
  --relay "$RELAY"

metadesk-host requests --socket /private/tmp/my-metadesk-host/control.sock list
metadesk-host requests --socket /private/tmp/my-metadesk-host/control.sock \
  approve REQUEST_ID GENERATION REVISION video
metadesk-host requests --socket /private/tmp/my-metadesk-host/control.sock \
  approve REQUEST_ID GENERATION REVISION video,agent,input --remember
metadesk-host requests --socket /private/tmp/my-metadesk-host/control.sock \
  deny REQUEST_ID GENERATION REVISION
metadesk-host session --socket /private/tmp/my-metadesk-host/control.sock list
metadesk-host session --socket /private/tmp/my-metadesk-host/control.sock \
  kick SESSION_ID GENERATION REVISION
```

Read generation/revision/IDs from the JSON list output. Mutations require those
**exact** values; conflict is an instruction to reconcile, never to blindly retry.
Endpoint options precede the verb. Instead of `--socket PATH`, use `--host-key
LOWERCASE_64_HEX` to select the default endpoint; no account or signer override is
needed to manage a running host. Syntax errors exit 2, operational/conflict errors
exit 1, success exits 0. List output is JSON, including the coherent requests and
session snapshot, not a plaintext token or a promise against later mutations.

The Unix host attempts a management listener even in allowlist-only mode, so
sessions can be listed/kicked. If the default runtime endpoint is unsafe or
unavailable, allowlist-only hosting warns and continues without management;
`--attended` or explicit `--management-socket` instead makes this startup error
fatal. No attended inbox is exposed without a working authenticated endpoint.

### Endpoint and peer trust

- Linux: `$XDG_RUNTIME_DIR/metadesk/host-<host_hex>.sock`; the runtime directory
  must be canonical, user-owned and owner-only. Server verifies `SO_PEERCRED.uid`
  equals its effective UID on every accepted connection.
- macOS: same XDG path if explicitly configured; otherwise
  `/private/tmp/metadesk-<effective_uid>/host-<host_hex>.sock`. Darwin's long
  per-user temp directory plus a full host key exceeds `sockaddr_un.sun_path`,
  hence this short, owner-checked private directory. Server verifies `getpeereid`.
- An explicit path must be absolute and fit the platform's Unix socket bound;
  its canonical parent must be user-owned/owner-only, without symlink aliases.
  Socket mode is 0600. Client verifies directory/socket ownership, type and
  permissions **and** server kernel credentials before sending a command.
- Existing endpoints of **any** type are never stolen or automatically unlinked.
  For crash recovery, independently establish that the old host is gone before
  removing its stale socket. Normal shutdown removes only the bound inode.
- No TCP listener, remote management command, key import, signer operation,
  arbitrary event publishing, or generic RPC forwarding exists here. Same-user
  native processes are inside this trust boundary, not sandboxed from each other.
- Windows: `session`/`requests` CLI and `--attended`/`--management-socket` return
  clear unsupported errors. Allowlist-only hosting is unchanged. Owner-only
  named-pipe DACL and client-SID verification are tracked in **md-wmo6**; no
  permissive pipe or TCP fallback is provided.

## Version-1 wire protocol (for md-a9f2)

AF_UNIX stream, UTF-8 JSON terminated by one LF. One command per connection;
ordinary commands receive one response then EOF. Requests and responses are
bounded to 8192 bytes. Invalid, duplicate/unknown fields, unsupported versions,
nonintegral/negative/unsafe revisions, invalid exact capability masks and
unsupported methods fail. At most eight peers run concurrently; incomplete reads
and blocked writes have five-second failure deadlines. These deadlines never
mean successful relay history or successful publication.

### Read and subscribe

```json
{"v":1,"method":"management.snapshot"}
{"v":1,"method":"requests.list"}
{"v":1,"method":"session.list"}
{"v":1,"method":"management.subscribe"}
```

All return full coherent snapshots of the same shape:

```json
{"v":1,"ok":true,"generation":"<32 hex>","revision":4,"requests":[{"request_id":"<32 hex>","peer":"<64 hex>","requested":7,"granted":0,"expires_at":1795350000,"state":"pending"}],"session":null}
```

`state` is `pending`, `remembering`, or `approved`. Bits are VIDEO=1, AGENT=2,
INPUT=4. `session`, if reserved/active, contains the same request fields plus
`session_id` and `revoked`. An assigned session ID includes the pre-bind
reservation: listing does not itself prove TCP binding. `revoked=true` means
teardown was requested; it is not an assertion that OS cleanup already finished.
After teardown the session becomes null. No stream token is exposed.

Subscribe installs its observer before taking the first snapshot, then sends a
new full snapshot whenever revision changes or a pending request expires. It is
server-output-only: client EOF, half-close or extra input ends it. A private
notification pipe wakes the worker; it does not poll state or relays. Idle peer
EOF releases the worker without needing another mutation. Revisions strictly
increase within a generation, but can skip because intermediate states are
coalesced. Every event is already a **complete reconciliation snapshot**, not a
delta; consumers need not reconstruct skipped changes. A new connection starts
with a full snapshot, so no subscribe/snapshot race is required. If a separate
snapshot is desired, subscribe first. Host generation changes require discarding
old rows/decisions. W16 adds the bounded private-policy operations below; the plan's host-latch API
is still not implemented.

### Mutate

```json
{"v":1,"method":"requests.decide","generation":"<32 hex>","revision":4,"request_id":"<32 hex>","decision":"approve","caps":1,"remember":false}
{"v":1,"method":"requests.decide","generation":"<32 hex>","revision":4,"request_id":"<32 hex>","decision":"deny"}
{"v":1,"method":"session.kick","generation":"<32 hex>","revision":9,"session_id":"<exact session id>"}
```

Approve requires both `caps` and `remember`; deny/kick reject those extra fields.
Every command returns the latest full snapshot with `ok`, and on failure `error`:
`invalid`, `unsupported_method`, `conflict`, `policy_unavailable`, `publish_failed`,
`cancelled_or_expired_after_publish`, `deny_delivery_failed`, or
`subscription_unavailable`. A decision linearizes under the access mutex before
network/signer work; its response snapshot can include subsequent independent
changes. A failed denial delivery does not restore approval authority.

Kick revokes authority synchronously and wakes the host; subsequent capture,
tree sends, packet dispatch and candidate binding reject that reservation. The
host thread closes the stream, burns the session/token, and then releases the
slot. Wait for the subscription's null session to observe completed teardown.
It does not remove a remembered grant, so a future fresh request can still be
authorized. It cannot undo already dispatched native input; there is no queued
input producer in this host loop.

Nostr cancellation is an authenticated live DM payload:
`{"type":"session_cancel","v":1,"request_id":"<32 lowercase hex>"}`. Only the
matching sender and exact request/reservation may cancel. Old or foreign IDs
never revoke a different session. This adds host-side cancellation handling,
not a client UI or new client cancellation/reconnect policy.

## Validation scope and protocol review

`host attended access state machine` tests queue bounds, strict deadline expiry,
exact capabilities, stale generations/revisions, barrier-synchronized approve/deny
races, failed/expired/cancelled remember, one session and kick. `management IPC
peer credentials` uses real kernel credentials on socketpair, checks matching
and deliberately mismatched expected UID (without changing process privileges),
invalid descriptors, private paths, active-path/symlink/unsafe-parent rejection,
idle subscription EOF reclamation and socket cleanup. It is not an actual
cross-UID login test or Windows SID qualification.

`attended real host client private relay` launches real binaries on this Mac
(and Linux), throwaway host/client roles, NIP-5F signers with real cryptography,
a private WebSocket relay and authenticated loopback TCP. It covers approve-once,
deny, exact video-only grant, stale decision, list/kick, absence of once-only
persistence, private-only remembered wire policy, fresh-host policy restoration
and auto-admission. It does **not** qualify screen capture, accessibility, native
input, FIPS encrypted routing, external signers, or hosted deployment. Every
step waits for a protocol/state/process transition; deadlines only fail/clean up.

Diff-scoped `nostr-protocol-smells` A-J review: no new relay polling or
elapsed-time relay completion, filters, REQ lifecycle, or reconnect logic; existing
EOSE/CLOSED/AUTH/NIP-11 handling is retained. New accept/deny sends check the
existing NIP-17 publisher's result; remember checks the existing private writer's
positive OK and unchanged-base result. Queue/request deadlines, IPC framing
bounds and process watchdogs are not relay completion heuristics. Replay IDs,
authenticated sender correlation and one-use binding remain authoritative.
The second-opinion review found no concrete defect in the supplied lifecycle and
protocol excerpts, but could not access the full diff artifact; it is explicitly
**not whole-patch approval**. The source-backed diff review and tests above are
the validation evidence, not that excerpt review alone.

### W13 local qualification results (2026-11-22)

| Gate | Result |
|---|---|
| macOS full configured suite (NIP-5F enabled) | 60 passed, 1 native AX live test skipped, 0 failures |
| macOS ASan + UBSan selected tests | 5/5 passed, including real host/client approval fixture |
| macOS TSan selected tests | 5/5 passed, including real host/client approval fixture; no suppressions |
| Linux ARM64 VM `192.168.64.3`, `~/md-w13`, build `-j2` | Full compile; 58 passed, 1 opt-in first-key desktop test skipped, 0 failures |
| Windows ARM64 VM `192.168.64.4`, `C:\Users\bizarro\md-w13`, build `-j2` | Full compile; session/state/binding tests 3/3 passed; management CLI exit 1 with unsupported diagnostic |

Sanitizer selection: host access state machine, management IPC, host binding,
retaining two-relay correlation, and attended real host/client private relay.
TSan initially exposed an inherited fixture's unlocked `session_requested` read;
it now uses the production request mutex, with no assertion weakening or timing
workaround. The initial IPC shutdown test exposed Darwin listener shutdown not
waking poll; the explicit stop pipe fixes that lifecycle defect. The older
negative fixture now asserts correlated refusal instead of waiting for a timeout.
Linux live-input opt-in was disabled; no GNOME desktop devices or Windows console
lock/disconnect were used. Windows reused read-only prebuilt dependency prefixes
from W10, without editing/building its checkout. VM directories and local scratch
builds/logs are removed after recording these results; the tests are reproducible
from this branch, not dependent on retained scratch artifacts.

Source discovery used five Jev find-lines calls (27 requests, 212188 input tokens,
approximately $0.008912). Partial client-lifecycle hits were widened to confirm
that kick closes the stream but the inherited client may reconnect; the fixture
now waits for actual connection loss and explicitly stops its test client.

### W12 TLS integration (master `8f10366`, including W12 `15aa94c`)

The master merge keeps both admission paths behind the pinned-TLS requirement.
An attended decision cannot authorize a legacy non-TLS client. Once approved,
the host publishes the **actual listener certificate SHA-256 pin** in the same
correlated, encrypted `session_accept` DM as the one-use token. W12's client
verifies TLS 1.3 and that exact pin before sending the token; missing/wrong pins
never fall back to plaintext. See [stream binding](stream-binding.md).

The initial admission loop keeps W13's rearming behavior, including cancellation
and stale decisions, with W12's `tls_required` refusal for legacy peers. The TLS
listener and its pin persist across rearming. Kick/cancel only revoke admission
from callback/IPC threads; the host thread gates stream use, closes it, joins
per-session work, destroys TLS, then resets the session and token. No management
protocol change or cross-thread SSL teardown was introduced.

`host_binding` now exercises a real human-approved capability subset with a real
TLS listener certificate, checks the emitted acceptance pin against that
listener, performs pinned TLS before binding, then checks both kick and live
`session_cancel` close the TLS peer and clear session/token/slot state. It also
checks a human-approved request without TLS support is refused before a token
or negotiating session exists. The real host/client approval fixture explicitly
requires the TLS 1.3 connection transition before stream activation and observes
connection loss after kick.

The direct-IP negative fixture retains its short 1500-ms watchdog, but no longer
confuses refusal **delivery latency** with fail-closed authorization. It requires
the host's correlated refusal, a terminal client failure, no TCP dial transition,
and unchanged proxy traffic. A refusal response or the client's watchdog may
win while retained signer history drains under TSan; neither is treated as a
successful stream or as proof of delivered denial. Delivered denial remains a
strict separate assertion in the real attended fixture and client refusal tests.
No timeout was increased, no retry added, and no sanitizer suppression used.

Diff-scoped Nostr A-J integration review found no introduced relay mismatch:
publication still checks positive OK, acceptance keeps request correlation,
replay fences and one-use binding, and TLS negotiation adds no relay polling,
filter broadening or elapsed-time history completion. The second-opinion merge
review was limited to supplied source excerpts and found no concrete defect;
it is not a whole-patch approval.

Merged-tree reruns: macOS full suite **60 passed, 1 native AX live skip**;
ASan+UBSan **9/9 passed**; TSan **9/9 passed**, without suppressions; Linux ARM64
`~/md-w13` compile at `-j2` and full suite **58 passed, 1 first-key desktop opt-in
skip**. Sanitizers covered stream pinning, session/client refusal, host binding
(including attended TLS kick/cancel), state machine, IPC, retained correlation,
NIP-5F direct-IP integration and real attended host/client flow. The Mac real
process fixture passed TLS approve-once/kick, strict delivered denial, private
remember and restart. No capture, input or FIPS routing qualification is implied.

Master advanced during validation only in Windows packaging and Beads metadata.
The final merge includes that newer tip; Git object IDs for `src/`, `tests/`,
`meson.build` and `meson_options.txt` were checked identical to the tested tree.
Inherited master Beads metadata is unchanged; no lane-authored Beads file edit
was included. Linux validation and local scratch directories were removed after
recording these results. Windows was not requalified by this merge-only run.

## Manager private-policy operations (W16 / md-a9f2)

Version 1 additionally supports:

```json
{"v":1,"method":"access.list"}
{"v":1,"method":"access.set","generation":"<32 hex>","revision":4,"policy_version":8,"peer":"<64 lowercase hex>","caps":1}
{"v":1,"method":"access.remove","generation":"<32 hex>","revision":4,"policy_version":8,"peer":"<64 lowercase hex>"}
```

`access.list` and policy mutations return the ordinary full snapshot plus
`policy_available`, `policy_version`, and `clients` (each has `peer`, canonical
`caps`, and `review_required`). Unloaded/failed history is **unavailable**, not
an empty policy. Legacy/mixed entries remain review-only; migrate with the CLI.
Oversized policy lists return `policy_too_large`, unavailable and no partial
clients; the 8192-byte framing bound is unchanged. Normal and subscription
snapshots also expose `supported`, the host's locally restricted capability mask.

Set replaces the exact known mask (zero means `none`); remove deletes the
principal. Both use the host's existing private, self-encrypted acknowledged
writer, never GUI relay publication. Generation/access revision are checked and
`policy_version` is compared under the writer's ACL lock **before** staging.
A stale policy yields `conflict`; writer/publication failures yield
`publish_failed`. A positive result requires positive relay OK and an unchanged
base. On any uncertain outcome, explicitly reconcile before retrying; a relay
may already have stored the replacement. A successful set/remove cancels that
principal's pending/remembering requests and revokes its active/pre-bind
reservation, then wakes the host for cleanup. It does not silently change a
running stream's grant. Subscription null-session is the cleanup receipt.

Policy listing is on connect, after decisions, and on explicit Refresh Private
Policy. External relay policy changes are not a new policy subscription here:
refresh explicitly before editing; the conditional writer rejects a stale view.
Requests/sessions themselves use the long-lived management subscription, without
polling or blind retries. No unattended latch, remote management, public-policy
migration, arbitrary signer method, or generic relay RPC is exposed.
