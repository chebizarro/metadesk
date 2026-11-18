# Signer accounts (P2.7)

An account is `(backend, pubkey, label)`. Public keys are lowercase, 64-character
hex. Backends are `direct`, `nip46`, `nip55l`, and `nip5f`. Host and client are
independent user-assigned roles; the same account can fill both.

## Headless CLI

```
metadesk-host account add nip55l PUBKEY "My signer"
metadesk-host account bind-role client nip55l PUBKEY
metadesk-host account bind-role host nip55l PUBKEY --fips-node NODE_PUBKEY
metadesk-host account list
metadesk-host account label nip55l PUBKEY "New label"
metadesk-host account check-fips-host NODE_PUBKEY
metadesk-host account unbind-role host
metadesk-host account remove nip55l PUBKEY
```

Adding metadata does not contact a signer. Binding requires the backend to be
available and verifies its live public key against the selected account.
`nip55l` and `nip5f` use the signer's default local endpoint. They never delete
signer-held keys. Real remote/local transports require the corresponding build
options; direct-key and custom-backend core tests do not.

Direct keys are explicit-integration/test only: `bind-role ... direct PUBKEY
--secret-stdin` reads a hex key piped from a trusted source. No native direct-key
vault is provided, so the key must be supplied again when activating a saved
selection. Never put secrets in argv, shell history, metadata labels, or files.

For an **already paired** NIP-46 account:

1. `account add nip46 USER_PUBKEY "Remote signer"`
2. Pipe the saved pairing JSON from a trusted source into
   `account credentials-stdin USER_PUBKEY`.
3. `account bind-role client nip46 USER_PUBKEY` restores the saved transport
   session, checks the remote user's key, and persists the selection.

The JSON has `version: 1`, `user_pubkey`, `remote_pubkey`, `client_secret` (the
transport key, not the user's private key), and `relays` (an array of ws/wss
URLs). Pairing UI/URI generation is not implemented by this CLI. Credentials go
only to Secret Service on Linux or Keychain on macOS. There is no plaintext
fallback. Headless operations do not prompt to unlock a locked OS store; unlock
it externally and retry. Duplicate Secret Service items fail closed; remove the
remote account and re-import its pairing to recover. Removal deletes only
Metadesk's remote credentials, not the remote signer's signing key.

## Core ownership and persistence

`MdAccounts` is a single-owner control-plane object. Its metadata pointers are
borrowed until mutation/destruction. `bind_role` retains a verified `MdSigner`;
the two roles share that signer for one account, not their cancellation tokens.
Acquire a ref-counted `MdAccountBinding` before handing work to another thread.
Use its cancellable operations, and check `is_current` before committing an
asynchronous side effect. Switching/removal/controller destruction revokes old
leases; in-flight non-abortable results are discarded without shutting down the
other role's signer. Release leases when work drains.

`management.json` stores only versioned account metadata and role pairs. Default
location: `$XDG_CONFIG_HOME/metadesk` (or `~/.config/metadesk`) on Linux;
`~/Library/Application Support/metadesk` on macOS. A private, owned 0700 directory
and 0600 regular, single-link files are required; symlink path components are
refused. `account --config-dir ABSOLUTE_DIRECTORY ...` overrides the location.
An advisory lock prevents concurrent controller/CLI writers. Saves use a 0600
exclusive temporary file, fsync, rename, and directory fsync. The C API requires
an explicit `md_accounts_save` after edits; failed saves are reported, not rolled
back. OS credential deletion and metadata saving are not a cross-store atomic
transaction: if saving after removal fails, repair storage and rerun removal.
Windows persistence is explicitly unsupported (no insecure fallback).

## Live startup and precedence

Host startup restores the saved **host** role; client startup restores the saved
**client** role. Restoration checks the live signer against the saved account
before acquiring a role lease. The host lease owns allowlist reads/updates and
session acceptance; the client lease owns outbound session requests. A bridge
cannot use a client lease for host operations (or a host lease for requests).

Precedence, in order:

1. Any explicit signer option bypasses saved accounts and the OS credential
   store: `--bunker`, then `--dbus-signer`, then `--socket-signer` (default local
   endpoint when no path is supplied), with `--auto-signer` as the socket/auto
   branch's fallback. This preserves the existing ordering if flags are mixed.
   An explicitly requested backend that fails is fatal, not an ephemeral fallback.
2. Otherwise, restore the application's saved role. Missing credentials, locked
   stores, unavailable backends, direct keys without an explicit integration,
   malformed metadata and wrong-owner replies fail closed. There is no fallback
   to another account, an environment-detected signer or an ephemeral key.
3. With **no selected role**, retain legacy behavior: host direct-IP mode needs
   no signer; client `--npub` uses an ephemeral signer; direct-IP client needs no
   signer. Auto-detection (including its environment inputs) runs only with
   `--auto-signer`. `--npub` selects a peer, not a signer override; `--relay`
   changes signaling relays, not the account or saved bunker's private relays.
   MCP startup remains independent of video/signing accounts.

FIPS hosting (`--npub`) additionally decodes the **actual daemon's** `show_status`
`npub` and compares it to the checked host identity. A missing/malformed node
identity or a different key is fatal, including with explicit signer overrides.
The expected client key is not the local node key. Direct-IP hosting omits this
FIPS equality check. Saved-role hosting never downgrades to unauthenticated TCP
when signaling fails or no authorized request arrives.

Runtime controllers load a read-only metadata snapshot under the advisory lock,
then release that lock so host, client and CLI can coexist. CLI edits affect the
**next startup**, not an already-running process; there is no file polling.
In-process role switches/removal revoke the old generation synchronously: bridge
admission closes, bridge-local signer cancellation fires, cached allowlist reads
fail closed, and the owning application's negotiation/session loop is stopped.
The client drops its document/tree state on normal shutdown. Non-abortable signer
results are discarded; a signer shared by the other role stays usable. A new
identity requires a fresh bridge/session, never reuse of an old owner's cache.
Revocation watchers must be removed before their callback data is destroyed.

## Security limits

Signer/role verification is not proof of end-to-end desktop authorization.
Inherited host stream/session binding and allowlist-readiness defects are tracked
as **md-mrw** and **md-q6m**; do not expose the host to untrusted networks until
those are resolved. Protocol review also tracks AUTH transport (**md-xcy**),
allowlist replacement ordering (**md-5w9**) and failed-publication/local-ACL
consistency (**md-30w**). The private restore fixture does not qualify relay AUTH,
CLOSED/rejection/reconnect handling or the full desktop session boundary.

## Tests

`meson test -C build accounts account_cli` runs the core/memory store and CLI
contracts. `account startup roles + private relay restore` runs owner-separated
bridge cancellation and process startup/override contracts in a temporary HOME.
With `-Dsigner_nip46=true` it also starts a private loopback WebSocket relay and
real nostrc bunker/client transports, restores saved pairing material from an
in-memory credential store, validates a signed event and refuses a wrong-owner
restore. The fixture uses only deterministic throwaway keys, observes REQ/EOSE
and OK, and removes all files/connections; it never reads an OS keyring. macOS also runs `account_keychain` against a private throwaway
Keychain (restoring the original search list). Linux registers
`account_secret_service` when `dbus-run-session` and `gnome-keyring-daemon` are
installed; otherwise Meson explicitly reports the unavailable fixture. It uses
a private HOME, XDG directories, runtime directory, D-Bus session, and daemon,
then removes all fixture files. It tests replacement across a default-collection
change and rejects duplicate items. No live user keyring is used by the tests.
