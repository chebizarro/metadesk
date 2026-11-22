# Authenticated direct TCP client

Use a host identity and a transport endpoint independently:

```sh
metadesk-host --bind 127.0.0.1 --port 7700 --relay ws://127.0.0.1:8080 \
  --socket-signer /private/host-signer.sock
metadesk-client --host 127.0.0.1 --port 7700 --npub HOST_NPUB \
  --relay ws://127.0.0.1:8080 --socket-signer /private/client-signer.sock
```

Replace HOST_NPUB with the host signer's bech32 npub. Configure the host's
allowlist/capability grants for the client signer's identity (`metadesk-host
access add CLIENT_NPUB video`). The access command uses the saved **host role**;
use `account` to configure it and pass the same private relay to `access`.
Ephemeral clients are not automatically authorized by a closed allowlist.
`--no-display` selects decode-only operation, not an authentication bypass.

`ADDRESS --npub HOST_NPUB` is a positional alias for `--host ADDRESS`.
The port is always a separate `--port PORT` option (not `ADDRESS:PORT`);
an IPv6 literal can be passed directly as ADDRESS. `--npub HOST_NPUB` without
an address retains FIPS daemon/peer readiness and mesh routing. Direct TCP
never checks or requires a FIPS daemon/TUN. An address without `--npub` fails
before signer startup or TCP. Specify `--relay` explicitly for local testing:
the product's configured/default relay selection is otherwise unchanged.

## Admission and stream binding

Both routes share the same NIP-17 request/accept exchange. The client pins the
expected host public key **before callbacks start**, creates a fresh random
request ID for every attempt, and accepts only a matching authenticated answer
inside its validity window. Relay history cannot grant an earlier session.
The host requires live, fresh, allowlisted requests and rechecks the current
capability policy before recording a grant. Its fail-closed NEGOTIATING gate
remains in place; opening TCP does not authorize a stream.

After the listener/backends are ready, the host sends a random session ID and
256-bit one-use bearer token only in the encrypted acceptance DM addressed to
the authenticated client. The first TCP packet must contain that exact session
ID/token (`session_bind`, at most 8 KiB). The host uses a constant-time token
comparison, checks the monotonic expiry, consumes/clears the token, and sends
a token-free authoritative ACK. Only then is the stream published to capture
and dispatch, with stream-owned host-granted capabilities. The client validates
the ACK's session ID, grants and tree format before becoming active. Wrong,
legacy, expired or replayed proofs and second peers cannot inherit a grant.

On disconnect, the host rearms Nostr admission and the client reconnect path
runs a fresh negotiation. The previous request ID is fenced, and a new session
ID/token replaces the consumed reservation. Policy is rechecked; a removed
client cannot reuse its old token. There is no unauthenticated reconnect.

**Threat boundary:** this binds a TCP stream to possession of the DM secret,
not to a TCP source address or a cryptographic transport identity. Native TCP
is plaintext: an on-path observer or a substituted endpoint can steal/race the
bearer proof, and the ACK does not authenticate the TCP server independently.
Use direct TCP only on a trusted network or inside a separately authenticated,
encrypted tunnel. FIPS remains the encrypted mesh transport. This change does
not claim TLS, channel binding or protection against an on-path attacker.

## Local regression gates

`meson test -C BUILD 'NIP-5F host session quoted seal'` (client and NIP-5F enabled)
also runs `tests/direct_ip_session.py` against the real host/client binaries,
private throwaway signers/relay and native 127.0.0.1 TCP. It covers identity-free
host refusal, unauthorized client refusal, allowlisted binding/ACK, a replaying
second TCP peer, and same-process disconnect/reconnect with fresh secrets.
The in-memory TCP proxy is a fixture for forced disconnect/replay observation,
not a substitute for production Nostr negotiation or native sockets. No public
relays, real credentials, FIPS daemon or capture/input are used. The host
may initialize the native accessibility backend (this is not a video test).
`host_binding`, `session JSON + state machine`, and `session correlation
retaining private relay` cover malformed/expired proofs, history/decoy answers
and duplicate delivery. The startup gate checks identity-free client refusal
and invisible GUI failure before any connection.
