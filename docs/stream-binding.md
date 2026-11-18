# Host stream binding (md-mrw)

This is a minimal binding extension to existing **v1 session signaling**, not
an implementation of the full P3.8/session-v2 management protocol. Binary
framing remains version 1. Both host and client must be upgraded together.

1. The host records the authenticated Nostr request and its authoritative grant.
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
Request IDs, request/result deduplication, attended approvals, dynamic grant
revocation and renegotiation remain the planned P3.8 work. The token deadline
starts at acceptance creation, after capture/backend startup.

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
substitutes for relay events. Successful binding is one-use and replay-safe;
request/result correlation remains explicitly deferred to P3.8.

`host_binding` uses private ephemeral IPv6 loopback sockets, the real host
candidate/capture/dispatch functions, and instrumented desktop boundaries. It
never creates a capture session, input backend or accessibility bus connection.
