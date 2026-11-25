# Client management store

`src/core/managed_store.h` is a headless API. Open it with an active client-role
binding and an owner-only application state directory (on Linux,
`$XDG_STATE_HOME/metadesk` or `~/.local/state/metadesk`). It holds an exclusive
lock for that Nostr author. Serialize calls and relay callbacks on the owner
of the store; on role change cancel the old generation's relay work, close the
store, and open the new owner's store.

## Using the store

1. Run a persistent REQ with `md_managed_store_filter()` per needed coordinate
   and the caller's selected read routes. Feed verified-path relay EVENT JSON
   into `md_managed_store_receive()`; it independently validates the envelope.
   Keep cached events visible through catch-up. Mark backfill complete only
   after the required EOSE barrier; CLOSED, AUTH/recovery and reconnect must
   not imply empty history.
2. Read the document and its revision. Missing optional v1 metadata defaults
   to empty label/notes/relay hints, null NIP-05, video, compact tree, and port
   7700. Tag tombstones default to false. The public key is the identity pin;
   derive any displayed fingerprint from it, not from a lookup or a second
   mutable field. Requested capabilities do not confer host permission.
3. Stage a whole-document edit with the read revision and caller-selected
   write targets. Do not send a locally constructed event directly. Retrieve
   the durable signed event from `outgoing`, send it with cancellable transport
   tied to the same binding, and record the correlated per-relay outcome. The
   handoff is durably recorded as possibly sent before transport gets the
   event. Only positive OK counts; even a relay echo of the pending event does
   not commit it. Do not mint a new replacement on retry. Raw relay reason strings are
   transport diagnostics, not durable state.
4. One positive OK commits the winner; remaining targets keep the document
   partial. In a conflict the remote winner and signed local draft are kept
   separately. Keep Remote drops a draft only if it was never handed off or
   echoed and has no outstanding superseded events. Otherwise it stages the
   exact remote content at a timestamp that beats the discarded draft, using
   the original targets and normal positive-OK gating. Replayed copies of the
   discarded draft cannot displace the remote baseline while this correction
   is pending. To keep reviewed local changes instead, call `reapply` against
   the current remote revision. Nostr has no compare-and-swap:
   another device may already have published or accepted a competing draft.

Tag deletion is an app-data tombstone plus an empty tag membership document.
Host deletion is a hosts-list edit plus independent favorite/tag/metadata
edits. They are not atomic transactions; filter orphan memberships against
`metadesk-hosts` while these edits are in progress. An unreadable winner takes
precedence over draft status. A decrypted but schema-invalid winner returns
its revision and no plaintext. After explicit user review, `replace_invalid`
can stage a complete replacement against that exact revision. It cannot
replace an unknown schema version or bypass a signer/decryption failure.
Unknown fields on retained v1 records and unrelated list extensions are kept.

Documents are limited to 256 KiB of ciphertext and 1,000 host entries. The
pinned NIP-44 v2 implementation also limits encrypted plaintext to 65,535 bytes;
a collection can hit that byte limit before it reaches the count ceiling. The
store returns a size error rather than truncating or sharding a document.

## Live controller

Native frontends can use `src/core/managed_store_controller.h` instead of
owning relay subscriptions. Serialize calls on a frontend worker; the change
callback is only a wakeup hint and must not call back into the controller.
Open the cache first, then start a plan with explicit bootstrap relays. The
controller watches hosts, favorites and app data; add exact tag coordinates
as they become known. An offline read can still return cache. Writes require
an EOSE boundary on every admitted fetch route in its current connection
generation. A local admission failure needs a fresh scoped replay rather than
trusting the relay's reconnect cursor. AUTH and retryable CLOSED recovery
remain owned by the bridge and nostrc. Use `flush` to send an already-staged
edit; retry sends its original signed event, not a new replacement.

## SDL quick connect

The SDL client reads the committed cache using its own current client-role
binding, whether started from the Linux manager or from the CLI. Quick connect
shows saved favorites, then deduplicated local recents. A row needs both saved
membership and a metadata record. A pending Hosts, metadata or Favorites
edit can hide an existing pin immediately: while the draft exists a key must
be present in both the confirmed and draft versions of that document. A draft
cannot add a selectable pin or change the connection data used for a pin. Opening the list and choosing a host
read the cache on a worker without opening a management relay subscription or
acquiring the writer lock. A signer that can decrypt the local cache is still
required; offline cache access is not offline session establishment.

The list is memory-only, and no new plaintext file or launch-channel payload
is needed. Linux and macOS use `$XDG_STATE_HOME/metadesk` (or
`~/.local/state/metadesk`); Windows uses `%LOCALAPPDATA%/metadesk`. The macOS
manager uses that same default when its cache path field is empty. Saved aliases
must still resolve to their pinned key. The overlay cannot add, update, or
replace a pin. Switching discards the old session and starts a new pinned
session using the original explicitly configured bootstrap relays. Without
those relays the list remains viewable, but switching is disabled rather than
using the CLI's implicit default. A one-off direct endpoint supplied for the
initial connection is not inherited. The Linux
manager's launch status ends with the session it originally launched. The
read-only overlay does not record switch outcomes, so switches do not update
recents; the manager records its own launch outcomes.

## Local data and compatibility

Cache and outbox are one atomic encrypted-event journal per coordinate, with
relay target fingerprints and finite result codes. Fingerprints use the same
URL normalization as nostrc (scheme/host case and trailing slashes; path case
is retained). Decoded JSON trees are scrubbed before release. Last-connected
records are local public-key/time/outcome tuples, retaining the newest 100
insertions. Malformed recents are discarded; a corrupt event journal instead
fails closed and is never treated as empty. The local unattended latch defaults to disabled; it is not host authorization.
Host policy callers must hold real authorization separately and must not
report revocation success if the local disable cannot be persisted.

The private host ACL and standalone CLI quality preferences reuse the unsigned
signer-backed event builder. Host ACL remains host-owned and keeps its positive
OK and legacy-public review policy. V1 management host metadata also accepts
the existing quality codec, and retains quality through unrelated edits. Set
`"quality": null` in an edit to clear that host's quality preference. The
standalone CLI's existing quality coordinate (`metadesk-session-preferences`)
remains compatible; it is not silently migrated into the client address book.

Import validates `metadesk-hosts/1` and returns candidates without modifying
state; present them for TOFU review. Export returns an allowlisted projection of
candidate records. Neither function opens a file or serializes grants. The UI
must warn that an explicitly exported file has plaintext labels and notes.
