# Headless session access

`metadesk-host access` manages the saved **host role** account's public NIP-51
kind-30000 list, `d=metadesk-allowlist`. It does not use the client role, a signer
override, a new identity, or a local allowlist file.

```sh
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] list
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] add CLIENT_NPUB
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] remove CLIENT_NPUB
```

Options precede the subcommand. Repeat `--relay` for multiple relays; without it,
the same built-in default relay as host startup is used. Use the same relays for
administration and hosting. `add`/`remove` accept checksummed NIP-19 **npub** keys;
`list` prints lowercase **hex** public keys, one per line (like `account list`).
Empty successful output means a loaded, empty list, not pending/failed history.
Invalid syntax exits 2; unavailable accounts/history/signers, failed publication,
and concurrent replacements exit 1. Success exits 0. Listing is a coherent copy
of the loaded membership, not a promise that a later relay update cannot change it.

The commands subscribe to history, awaiting real EOSE/CLOSED/connection failure;
they do not sleep, poll, or infer completeness from elapsed time. A relay that
never supplies a history boundary can leave a command waiting; interrupt it to
abort. Edits preserve other members and use the core's staged publish: success
requires a positive relay OK and an unchanged base version. Existing membership
and absent removals are idempotent no-ops. On a failure, reread the relay list
before retrying: a relay may have stored an event even if its acknowledgement was
lost, or another administrator may have replaced it. As in the core, one positive
relay acknowledgement suffices; this is not multi-relay quorum/atomic storage.

## Capabilities and secure defaults

**Membership is full admission, not a restricted capability grant.** The current
host intersects the request with `VIDEO | AGENT | INPUT`; it does not interpret
per-entry capability text. Video requires VIDEO; tree requires AGENT; actions
require AGENT + INPUT; raw input requires INPUT on the negotiated stream.
The CLI therefore rejects `[caps]` rather than publishing an unenforced promise.
Per-client capability policy is tracked by **md-ikr**. Existing opaque tag extras
are preserved when editing other members but are not security restrictions.
Removing membership controls future admission; it does not revoke an active
stream. Dynamic revocation/renegotiation remains P3.8 work.

Unknown, failed and loaded-empty lists deny admission. **No open/dev mode was
added.** A loopback-only, loudly warned opt-in can be revisited on request; it is
not a fallback or an option in these commands. TCP still requires the one-use
acceptance-DM proof described in [stream binding](stream-binding.md).

## Fresh host/client walkthrough

Prerequisites: upgraded host/client binaries, an available signer on each side,
a shared reachable relay, and working FIPS routing to the host's identity.
Metadesk does not generate signer identities, run a relay/FIPS daemon, or perform
new NIP-46 pairing. Those transport/signer prerequisites are separate from this
walkthrough. No external Nostr tool is needed to edit/publish access policy.
See [accounts](accounts.md) for supported backends and private credential input.

Starting with no Metadesk account metadata, the validated NIP-46 route below
assumes each side's trusted signer integration supplies its already-paired JSON
on a private pipe at file descriptor 3 (not a terminal, argv or plaintext file).
Unlock the OS credential store first. On the **host**:

```sh
metadesk-host account add nip46 "$HOST_HEX" "Desktop host"
metadesk-host account credentials-stdin "$HOST_HEX" <&3
metadesk-host account bind-role host nip46 "$HOST_HEX" --fips-node "$HOST_HEX"
metadesk-host account list
metadesk-host access --relay "$RELAY" list
metadesk-host access --relay "$RELAY" add "$CLIENT_NPUB"
metadesk-host access --relay "$RELAY" list
```

On the **client**, in its own account directory/home:

```sh
metadesk-host account add nip46 "$CLIENT_HEX" "Desktop client"
metadesk-host account credentials-stdin "$CLIENT_HEX" <&3
metadesk-host account bind-role client nip46 "$CLIENT_HEX"
metadesk-host account list
```

For an available local signer, use `nip55l` or `nip5f` instead of `nip46` in
`add`/`bind-role` and omit `credentials-stdin`; backend-specific readiness is a
prerequisite. `HOST_HEX` must match the host FIPS daemon's node identity. The CLI verifies the
live signer owner when binding and again when restoring it. For an **already
paired NIP-46 signer**, substitute `nip46` in the account commands and import
its saved pairing with `account credentials-stdin PUBKEY` from a trusted pipe
before `bind-role`; never put credentials/private keys in argv or metadata.
Neither `access` nor startup has a plaintext credential fallback.

Start the host, then connect from the client:

```sh
# Host: waits for an allowlisted Nostr request, then publishes acceptance.
metadesk-host --npub "$CLIENT_NPUB" --relay "$RELAY" --port 7400
# Client: Nostr negotiation, FIPS TCP, then token-bearing session_bind and ACK.
metadesk-client --npub "$HOST_NPUB" --relay "$RELAY" --port 7400
```

Use `--no-capture --no-input` on a test host and `--no-display` on a headless
client to validate authorization/binding without a desktop. Do not replace the
client's `--npub` with a direct IP: the inherited direct-IP client path does not
negotiate the required session proof. A new host negotiation/startup is needed
for a new stream after disconnect. Direct unauthenticated TCP remains disabled.

## Private validation

`access_cli arguments` checks syntax/missing host role in every build.
`access_cli private relay` (requires `-Dsigner_nip5f=true`) exercises the installed
CLI dispatch, real signer IPC/cryptography and real WebSocket transport with
throwaway accounts. It explicitly gates EOSE and OK, rejects publication, sends
CLOSED, checks host/client separation and wrong-owner refusal, and verifies
membership preservation, empty history and idempotent operations. No sleeps or
public services, OS keyring or desktop are used.

On Linux, the opt-in full-process check additionally uses real NIP-46 bunker
transports and a private unlocked Secret Service in a temporary HOME/D-Bus
session. It performs all account/import/binding/access operations with Metadesk
commands, starts the real host and client, and waits for authenticated stream
binding to complete. It needs signer_nip46, client support, `sudo -n`, `unshare`,
iproute2, `dbus-run-session`, `gnome-keyring-daemon` and `stdbuf`:

```sh
MD_ACCESS_SESSION_E2E=1 meson test -C build 'access session e2e' --print-errorlogs
```

The entire fixture runs in a disposable network namespace, assigning the host's
derived ULA only to that namespace's loopback interface. FIPS health/readiness
is a fixture control endpoint: this is **real Nostr + host/client TCP session
proof**, not qualification of a FIPS daemon, encrypted transport, video,
accessibility or input. Nothing in the VM's main routing namespace is changed.
Without opt-in this gate reports an explicit skip. Children, the private
keyring/bus, namespaces, sockets and account directories are removed on exit.
The root supervisor terminates the fixture process group before removing files;
a private PID namespace and `--kill-child` contain descendants on timeout.

## Diff-scoped Nostr protocol review

Reviewed the CLI, additive snapshot, dispatch/build hooks and private fixtures
against `nostr-protocol-smells` A–J. No new protocol mismatch was found: history
uses EOSE/CLOSED/connection boundaries and a condition variable, not polling or
elapsed-time success. Edits reuse staged positive-OK publication and version
checks; the CLI never fabricates relay frames. Filters remain host-author,
kind-30000, exact `#d` and `limit:1`; membership edits are idempotent. Tests gate
EOSE and OK explicitly and exercise rejection/CLOSED and owner mismatch.
AUTH/NIP-11 and reconnection remain in the unchanged core, not reimplemented by
the CLI. Fixture process deadlines are failure/cleanup bounds, not relay-history
completion. Review's fixture timeout-cleanup recommendation was applied.
