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

Loaded role selections are **not live bindings**. Startup/live-session adoption
is deliberately deferred. A future integration must supply checked signers,
acquire role leases, cancel owner-specific work on switching, and call
`md_accounts_check_fips_host` with the actual node pubkey from `show_status`
before FIPS hosting. Direct-IP hosting does not require that key equality.
The CLI does not change current session startup or existing signer flags.

## Tests

`meson test -C build accounts account_cli` runs the core/memory store and CLI
contracts. macOS also runs `account_keychain` against a private throwaway
Keychain (restoring the original search list). Linux registers
`account_secret_service` when `dbus-run-session` and `gnome-keyring-daemon` are
installed; otherwise Meson explicitly reports the unavailable fixture. It uses
a private HOME, XDG directories, runtime directory, D-Bus session, and daemon,
then removes all fixture files. It tests replacement across a default-collection
change and rejects duplicate items. No live user keyring is used by the tests.
