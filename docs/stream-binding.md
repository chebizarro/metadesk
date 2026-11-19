# Host stream binding (md-mrw)

This is a minimal binding extension to existing **v1 session signaling**, not
an implementation of the full P3.8/session-v2 management protocol. Binary
framing remains version 1. Both host and client must be upgraded together.

1. The host records the authenticated Nostr request and its authoritative grant.
   At acceptance it rechecks the current per-client allowlist policy and computes
   `requested & entry_grant` (also applying local `--no-input`). Zero is refused.
   Legacy/malformed grants default to zero; see [grant format v1](access.md).
   This is a snapshot for the next stream, not dynamic active-stream revocation.
   Once listener/capture/backend startup is complete, it reserves a CSPRNG
   256-bit token for 30 seconds on the monotonic clock and publishes acceptance.
   Startup cannot spend the client's bind/ACK budget. Entropy failure denies
   access; there is no fallback.
2. The existing NIP-17 `session_accept` DM additionally carries `stream_token`
   (64 lowercase hex characters). It is ephemeral: never log or persist it.
3. A candidate TCP connection's first application packet must be
   `MD_PKT_SESSION_INFO` with exactly
   `{"type":"session_bind","v":1,"session_id":"...","stream_token":"..."}`.
   The entire JSON is bounded to 8 KiB, with a five-second total read deadline.
   Invalid, expired, duplicate and replayed proofs close the candidate.
4. The host compares the negotiated ID and token (constant-time token compare),
   consumes the token once, and replies with a **token-free** `session_accept`
   containing the host's grant and tree format. The client validates that ACK.
   Only then does the host publish the connection to capture/tree/dispatch.
5. The grant belongs to that stream, not to a later accepted socket. Video
   requires `VIDEO`, tree requires `AGENT`, actions require `AGENT | INPUT`, and
   raw input requires `INPUT`. Denied dispatch and duplicate binding close the
   stream. An additional client is closed without disturbing the active one.

Legacy clients that echo `session_accept` as their stream handshake are
incompatible and fail closed. Legacy hosts without tokens are not accepted by
the updated managed client. Direct unauthenticated TCP hosting is disabled,
including fallback after absent/failed signaling. FIPS provides transport
confidentiality for the bearer proof; this is not a new public-Internet TLS
identity protocol.

The host still has a single startup negotiation: a disconnect does not restore
or remint a consumed grant. A new host negotiation/startup is required for a new
stream. Wrong candidates do not consume an otherwise unexpired reservation;
only successful binding consumes it. Admission handles one candidate at a time
with a bounded wait, not P3.8's future asynchronous four-candidate controller.
Request IDs (as correlation only, see below), request/result deduplication,
attended approvals, dynamic grant revocation and renegotiation remain the
planned P3.8 work. The token deadline
starts at acceptance creation, after capture/backend startup.

## Request correlation (md-6ns)

NIP-59 randomizes gift-wrap `created_at` up to two days in the past, so the
DM subscription's `since` must cover that window and cannot separate a live
negotiation from relay history. A relay that retains history replays earlier
requests to a new host and earlier acceptances to a new client. Correlation is
therefore semantic, inside the encrypted, seal-authenticated rumor.

Wire change (still `"v":1`; field names and shapes are those of session-v2
§3.10, so P3.8a can adopt them unchanged):

- `session_request` adds `request_id` (fresh CSPRNG 128-bit value, 32 lowercase
  hex characters, per negotiation and per reconnect), `issued_at` and
  `expires_at` (integral Unix seconds). The client sets the window to its own
  wait for an answer, capped at 180 seconds. Entropy failure aborts the request.
- `session_accept` (the DM) echoes the request's `request_id`. The token-free
  stream ACK and `session_bind` are unchanged.

Host rules, before the allowlist and before latching its single negotiation:

1. Only **live** requests are answered: the gift wrap must arrive after its
   relay subscription's EOSE, in that subscription's ordered delivery
   (`nostr_simple_pool_get_callback_live_since`). Stored history and
   post-reconnect catch-up are ignored. The host announces
   `waiting for session requests` only after every relay reached EOSE or a
   terminal state (bounded 10 s readiness wait; subscriptions stay open).
2. The request must carry `request_id`/`issued_at`/`expires_at`, with
   `0 < expires_at - issued_at <= 180`, `issued_at <= now + 30`,
   `now <= expires_at + 30`, and the rumor's own (non-randomized) `created_at`
   within 30 seconds of `issued_at`.
3. Ignored requests are logged and do not latch, so the client's live request
   can still be answered. The acceptance is refused (no token minted) if the
   pending request has no `request_id`.

Client rule: only an acceptance from the pinned host, addressed to the client
(NIP-17 recipient validation), whose `request_id` equals the current pending
request, carrying a token, `session_id` and a non-zero grant within the
requested capabilities, and whose rumor is not older than `issued_at - 30`, is
taken. It is matched at most once. Replays of earlier sessions' acceptances,
answers to another request, and uncorrelated acceptances are ignored and
logged without the token. Late answers after the client's deadline are ignored.
Other clients' acceptances are addressed and encrypted to them, so they are
never delivered. Event-id deduplication across relays/reconnects remains in
`nostr.c` (md-70v.1), with one refinement. A wrap or rumor id seen only as
history (for example during one relay's post-reconnect catch-up) is delivered
again, exactly once, when its first live copy arrives from any relay.
Otherwise a history copy from one relay would suppress the live copy from
another, dropping a legitimate request. Every other repeat is a duplicate, and
the handlers are idempotent on replay.

Compatibility: as with md-mrw, host and client must be upgraded together. A
legacy client's request (no `request_id`) is ignored by an updated host as
`uncorrelated`, and the client times out. An updated client ignores a legacy
host's acceptance (no `request_id`) and times out. Both fail closed, without
granting a stream. The host requires the client to request after the host is
listening. A request published before the host's subscription reached EOSE is
history. Clock skew beyond 30 seconds fails closed.

Coverage: `session correlation retaining private relay` runs three host/client
bridge negotiations on one retaining private relay. It uses the real
`host_on_dm`/acceptance path and the client's acceptance rule, plus live decoy
acceptances for another request and an uncorrelated one. It also runs a
two-relay case: the request is history on relay 1, and the identical wrap
arrives live on relay 2 and is answered once. The opt-in Linux
`access session e2e` runs a second real host/client process pair on the same
relay.

Protocol review (`nostr-protocol-smells`, A–J, diff-scoped):

- Liveness is the ordered EOSE boundary, not a timer or `since`.
- The host's 10-second history wait is a readiness bound. It closes no
  subscription.
- The client's wait is the request's own deadline, woken by DM events.
- Publishes still require relay `OK`.
- Filters are unchanged.
- The tests wait on callbacks, with deadlock bounds only.

The one finding (history-first dedup suppressing a later live copy) is fixed
above.

Standalone MCP stdio/HTTP mode takes a separate startup branch, with its own
local/HTTP authentication and backend-readiness capability checks; it cannot be
reached through a TCP session packet. This change does not turn an MCP HTTP
credential into a Nostr session or change standalone MCP authorization policy.
TCP action dispatch is gated before calling the shared agent action handler.

## Diff-scoped Nostr protocol review

Reviewed the changed host/client signaling calls, session/stream helpers and
private tests against `nostr-protocol-smells` A–J. No new relay polling,
timeout-based subscription closure, ignored relay frames, broad filters,
queue wrappers, capability assumptions or sleep-based relay tests were added.
Acceptance publication still checks the existing Nostr API's success result;
relay OK/EOSE/CLOSED/AUTH handling stays in the unchanged Nostr layer. Token
expiry and TCP admission deadlines are authorization/transport limits, not
substitutes for relay events. Successful binding is one-use and replay-safe.
Request correlation is the md-6ns section above. Its own protocol review is
recorded there.

`host_binding` uses private ephemeral IPv6 loopback sockets, the real host
candidate/capture/dispatch functions, and instrumented desktop boundaries. It
never creates a capture session, input backend or accessibility bus connection.
