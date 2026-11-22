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

## Transport encryption (md-lp7i)

The client advertises `stream_tls: "tls13-sha256"` in its encrypted request.
The host creates an in-memory ephemeral P-256 certificate at listener startup
and includes `tls_cert_sha256` (64 lowercase hex, SHA-256 of the DER leaf
certificate) in every encrypted `session_accept`. The identity-pinned NIP-17 /
NIP-44 negotiation authenticates this pin, request ID, session ID, token and
grant together. The client completes TLS 1.3 and compares the exact certificate
digest **before any application packet**, including `session_bind`. Web PKI,
DNS, the transport IP and the self-signed certificate's CN are not identity
authorities. No certificate private key or TLS traffic key leaves the host.

**Threat model:** a passive observer cannot read the token, ACK or media. An
active endpoint-substituting MITM has a different certificate and is rejected
before the token is sent. A transparent TCP forwarder can relay encrypted
records, but cannot decrypt or change them. TLS record/transcript replay cannot
authenticate a new channel; a decrypted proof replay by an authorized client
also fails the existing one-use token gate. Reconnects negotiate fresh request
IDs, session IDs and tokens; the listener certificate is reused only for that
host process and is reauthenticated in each acceptance. TLS early data is not
enabled ([OpenSSL's default](https://docs.openssl.org/master/man3/SSL_read_early_data/)).
Endpoint/signing-key compromise, traffic analysis and denial of service
are outside this protection; silent TLS candidates have a bounded handshake.

**Compatibility: fail closed.** Upgrade both ends. Hosts refuse requests without
the exact TLS capability before minting a token (`tls_required`); clients refuse
an acceptance without a valid pin, and report missing-pin or TLS/pin failures.
There is no plaintext fallback, TOFU prompt, insecure flag or automatic downgrade.
Old clients may display only a generic refusal or disconnect.

FIPS retains its daemon/peer-readiness checks and identity-derived TUN routing.
TLS is deliberately layered over its existing encryption: one listener and one
binding policy cannot accidentally authorize a plaintext direct connection by
trusting a claimed route or source address. The cost is redundant encryption,
not a change to FIPS identity, authorization or routing semantics.

## Local regression gates

`meson test -C BUILD 'NIP-5F host session quoted seal'` (client and NIP-5F enabled)
also runs `tests/direct_ip_session.py` against the real host/client binaries,
private throwaway signers/relay and native 127.0.0.1 TCP. It covers identity-free
host refusal, unauthorized client refusal, allowlisted binding/ACK, a replaying
second TCP peer, and same-process disconnect/reconnect with fresh secrets.
The in-memory TCP proxy is a transparent ciphertext capture fixture for forced disconnect/replay observation,
not a substitute for production Nostr negotiation or native sockets. The private
client signer observes the decrypted acceptance only to assert that its token
never appears in captured TCP bytes; the proxy does not possess TLS keys.
`test_stream` also checks correct pins, a substituted certificate, malformed
pins, silent TLS candidates and one-use proof replay over fresh pinned channels.
No public
relays, real credentials, FIPS daemon or capture/input are used. The host
may initialize the native accessibility backend (this is not a video test).
`host_binding`, `session JSON + state machine`, and `session correlation
retaining private relay` cover malformed/expired proofs, history/decoy answers
and duplicate delivery. The startup gate checks identity-free client refusal
and invisible GUI failure before any connection.
