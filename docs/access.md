# Headless session access

`metadesk-host access` manages the saved **host role** account's private NIP-51
kind-30000 list, `d=metadesk-allowlist`. It does not use the client role, a signer
override, a new identity, or a local allowlist file.

```sh
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] list
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] add CLIENT_NPUB [CAPS]
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] remove CLIENT_NPUB
metadesk-host access [--config-dir /absolute/private/directory] [--relay ws://relay ...] migrate [CLIENT_NPUB CAPS ...]
```

Options precede the subcommand. Repeat `--relay` for multiple relays; without it,
the same built-in default relay as host startup is used. Use the same relays for
administration and hosting. `add`/`remove` accept checksummed NIP-19 **npub** keys;
`list` prints lowercase **hex** public keys and effective caps, tab-separated, one
member per line. Legacy public or mixed lists add a third column,
`legacy-public-review-required`, and report `none` for every member.
`none` also includes invalid grant metadata; it never
reports an opaque relay hint as authorization.
Empty successful output means a loaded, empty list, not pending/failed history.
Invalid syntax exits 2; unavailable accounts/history/signers, failed publication,
and concurrent replacements exit 1. Success exits 0. Listing is a coherent copy
of the loaded membership, not a promise that a later relay update cannot change it.

The commands subscribe to history, awaiting real EOSE/CLOSED/connection failure;
they do not sleep, poll, or infer completeness from elapsed time. A relay that
never supplies a history boundary can leave a command waiting; interrupt it to
abort. Edits preserve other members and use the core's staged publish: success
requires a positive relay OK and an unchanged base version. Omitting caps preserves existing membership/policy; explicit caps upserts by
public key, replacing all of that principal's membership/scope records. Identical
valid grants and absent removals are idempotent no-ops. On a failure, reread the relay list
before retrying: a relay may have stored an event even if its acknowledgement was
lost, or another administrator may have replaced it. As in the core, one positive
relay acknowledgement suffices; this is not multi-relay quorum/atomic storage.

## Capabilities and secure defaults

At session acceptance, the host atomically reads membership and scope from the
current loaded policy and grants **requested & entry_grant**, then applies local
restrictions such as `--no-input`. A zero result refuses the session before a
stream token is reserved. The grant flows through the acceptance DM, one-use
binding and authoritative TCP ACK into that stream. Video requires VIDEO; tree
requires AGENT; actions require AGENT + INPUT; raw input requires INPUT.

`access add NPUB [CAPS]` accepts `none` or any nonempty subset in canonical
`video,agent,input,monitor,clipboard` order, comma-separated without whitespace. For example:

```sh
metadesk-host access add "$CLIENT_NPUB" video
metadesk-host access add "$CLIENT_NPUB" video,agent,input
metadesk-host access add "$CLIENT_NPUB" none  # retain membership, revoke all grants
```

Omitted CAPS gives a **new** member an explicit video-only grant; it does not
change an existing member (including legacy/denied members). Explicit CAPS is
required to change an existing private grant. Public entries require the
explicit `migrate` command below; ordinary add/remove never converts them. No name implies other bits: `agent` permits
trees but not actions; `input` permits raw input but not actions.

### Private allowlist grant format v1 (md-bmiw / md-ikr)

The host-author-signed kind-30000 event exposes **only**
`["d", "metadesk-allowlist"]` in its public tags. `content` contains a NIP-44
v2 self-encrypted JSON array of NIP-51 private items, for example after decryption:

```json
[
  ["p", "<64 lowercase hex client pubkey>"],
  ["metadesk-caps", "<same pubkey>", "v1:video,agent,input"]
]
```

`metadesk-caps` is a Metadesk extension, not a standard NIP-51 scope. Each record
has **exactly three strings**. The third string is `v1:` followed by the same
canonical CAPS grammar as the CLI (`v1:none` means zero). Exactly one membership
and exactly one valid grant record are required per principal. Missing, duplicate,
unknown-version, unknown-token, empty, repeated-token, noncanonical-order or
wrong-arity scope records grant **zero**, as do duplicate memberships and orphan
grant records. Invalid records cannot combine with a valid duplicate to escalate
privileges. No numeric masks or implicit capability dependencies are accepted.

Metadesk decodes public tags and decrypted private policy items with the same
raw-tag rules (md-jqnz). Unknown private items are retained, inert and encrypted,
when editing; unknown public event-level tags are never copied to a replacement. It does not use nostrc's
generic NIP-51 parser, because that parser's tag policy depends on the nostrc
version:

- Before nostrc 5d1283a7c it drops application tags.
- From 5d1283a7c it keeps every tag, and rejects the whole list if any tag has
  more than three fields.

Metadesk's rules are fixed:

- **Address.** The first `d` tag is the address, as in NIP-01. If the first `d`
  is not `metadesk-allowlist`, the event is ignored, even when a later `d` tag
  matches.
- **Membership.** A `p` tag counts whatever its length. The third field is an
  opaque relay hint. Later fields, such as a NIP-02 petname, are ignored.
- **Grants.** A `metadesk-caps` record that does not have exactly three fields
  is an invalid grant for its principal, so that principal gets zero. The record
  is kept rather than dropped, so a valid duplicate cannot override it.
- **Other public tags.** No other tag affects access, and edits do not republish
  them. Unknown private arrays of strings are preserved inside content only. Edits publish only the public `d` tag, with `p` and `metadesk-caps`
  encrypted in content. This keeps event-level
  tags from a stale event, such as NIP-40 `expiration` or NIP-70 `-`, out of
  the replacement. Older metadesk builds carried forward `e`, `t`, `word`, `a`
  and `r` tags.

A malformed or unknown tag never rejects or opens the list as a whole. At most,
it denies the one principal it names.

Encryption and decryption go through the saved host role's `MdSigner`, using
the host public key as the peer: local keys use nostrc's NIP-44 API and external
signers use their NIP-44 operations (including NIP-46 `nip44_encrypt` and
`nip44_decrypt`). No private key export, signer override, NIP-04 fallback, or
plaintext fallback is used. Signature/author/address checks precede decryption.
Ciphertext and plaintext are bounded to 128 KiB; signer/crypto/JSON failures on
a newer valid envelope deny all access and edits, rather than retaining older
authority or treating unreadable content as an empty list. A later valid newer
policy can recover it. Events more than five minutes ahead of the local clock
are ignored; local edits cannot advance beyond that bound.

NIP-44 does not hide the author, address, timestamps or approximate list size,
and does not offer forward secrecy if the host key is later compromised.

### Legacy public permissions — review required

**Every public membership or capability record requires review**, including
valid public v1 grants from previous releases. List output labels these records
`legacy-public-review-required`; sessions and MCP grant them zero capabilities.
A mixed public/private list is review-only **as a whole**, not a silent union.
Third-field legacy capability strings remain opaque relay hints, never policy.
Ordinary `add`/`remove` fail on review-required lists to avoid silently migrating
other principals.

First list and review every member, then confirm the **complete** membership with
explicit canonical scopes (use `none` for members that should stay denied):

```sh
metadesk-host access --relay "$RELAY" list
metadesk-host access --relay "$RELAY" migrate "$CLIENT_NPUB" video "$OTHER_NPUB" none
metadesk-host access --relay "$RELAY" list
```

Each distinct source member must appear exactly once. Partial confirmations,
new principals, duplicate confirmations, missing scopes and unknown scopes
fail. Duplicate source membership/scope records are normalized only by this
explicit review; orphan grants and non-policy public metadata are discarded. Unknown private
items are preserved, without granting authority. The CLI
prints the confirmed keys/scopes and disclosure warning to stderr. An empty
source can be explicitly migrated with `access migrate` (encrypted `[]`).
Unreadable private content cannot be migrated, because its membership cannot
be reviewed.

Migration stages and encrypts a replacement with **no public client tags**.
The source remains the enforced/cache policy until positive relay `OK` and an
unchanged-base version check; `OK false`, signer failure, stop or conflict
reports failure without committing the candidate locally. NIP-42 AUTH and
CLOSED handling remain in the existing relay session path. One positive OK
suffices, not a relay quorum. A lost OK or concurrent edit may still have
reached a relay; reread before retrying.

**Historical public copies cannot be erased.** Relays, backups and observers
may retain every earlier public list. An encrypted replacement only protects
future list contents; it cannot undo earlier membership/capability disclosure.
No deletion event or erasure promise is made. Older Metadesk builds cannot
read the private grants and fail closed; upgrade every consumer, including MCP.

Private edits retain other members and their parsed grant metadata; removing a
principal drops all its membership and scope records. Wrong-arity grants remain
invalid so unrelated edits cannot normalize them into authorization. Newest
replaceable ordering, fail-closed readiness and positive-OK staged publication
apply to private grants exactly as before.

Grant changes/removal affect the **next session acceptance**, not an active
stream. Dynamic revocation/renegotiation remains P3.8 work.

The same list can also authorize NIP-98 MCP HTTP agents: run the host with
`--mcp-http --mcp-allow-host-list`, and an `agent` grant admits that npub live,
without a restart. MCP restores the same host signer to decrypt private items;
legacy/mixed/unreadable lists fail closed through the same grant API. As in sessions, MCP action tools also need `input`: an
`agent`-only MCP principal gets the read tools, and its action calls are
refused with an error naming `input`. Unlike sessions, a grant change applies to the
next MCP request. See [MCP HTTP authentication](mcp-auth.md#per-principal-tool-capabilities-md-5km).

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
metadesk-host access --relay "$RELAY" add "$CLIENT_NPUB" video
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
negotiate the required session proof. A fresh host negotiation is needed for every new stream; the host rearms its
existing subscription after disconnect. Direct unauthenticated TCP remains disabled.

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

`capability grants private relay` exercises signed v1, legacy, revoked, removed,
orphan, duplicate and malformed policy over real private WebSocket history. Its
view-only path negotiates/binds real loopback streams and calls the production
host dispatch: input/actions close the stream before instrumented desktop calls;
trees are suppressed. `nostr ACL and AUTH private relays` additionally checks
live grant edits and rejected revocation with positive/negative OK barriers.
The production-acceptance fixture revokes on the same live bridge and refuses
its next negotiation before token reservation. For each denied snapshot it also
edits an unrelated principal, then re-REQs the published event to prove malformed
and duplicate grant metadata cannot become authority through serialization.
`access_cli private relay` checks explicit scope upsert, omitted-scope preservation,
`none`, listing and rejected edits. These are desktop-free boundary tests, not
live input or accessibility qualification.

## Diff-scoped Nostr protocol review

Reviewed the CLI, additive snapshot, dispatch/build hooks and private fixtures
against `nostr-protocol-smells` A–J. No new protocol mismatch was found: history
uses EOSE/CLOSED/connection boundaries and a condition variable, not polling or
elapsed-time success. Edits reuse staged positive-OK publication and version
checks; the CLI never fabricates relay frames. Filters remain host-author,
kind-30000, exact `#d` and `limit:1`; membership edits are idempotent. Tests gate
EOSE and OK explicitly and exercise rejection/CLOSED and owner mismatch.
AUTH/NIP-11 and reconnection remain in the unchanged core, not reimplemented by
the CLI. The md-ikr diff-scoped A–J review likewise found no introduced protocol
mismatch: scope edits reuse the same acknowledged write/version fences; strict
scope parsing adds no wire loops, filters, timers or alternate publish path.
Review recommendations added a platform guard and stateful revocation/republish
coverage. Existing single-startup negotiation is not dynamic P3.8 revocation. Fixture process deadlines are failure/cleanup bounds, not relay-history
completion. Review's fixture timeout-cleanup recommendation was applied.

### Private-list validation (W8)

`nostr ACL and AUTH private relays` exercises self-encrypted decoding, legacy and
mixed review-only policy, complete migration, rejection, reflected pre-OK events,
MAC tamper, foreign-key ciphertext and stale replay denial. The CLI fixture
checks migration's source preservation and private-only wire output. Capability,
MCP proxy and direct-IP fixtures now seed private policies, not public grants.

`private allowlist real host/client loopback` launches the real binaries with
throwaway NIP-5F signers and accounts, uses `access add`, verifies private-only
relay policy, negotiates NIP-17 and completes authenticated loopback TCP binding.
It uses no desktop capture/input, user credentials, public relay, FIPS daemon or
routing changes. It is a local authorization/binding gate, not FIPS or desktop
qualification. Run directly with:

```sh
python3 tests/access_private_session.py BUILD/metadesk-host BUILD/metadesk-client BUILD/access_signer_fixture
```

## Attended approval and session management

Explicit `--attended` adds bounded local approve-once / approve-and-remember /
deny decisions without opening the default policy. `metadesk-host requests` and
`metadesk-host session` use same-user, credential-verified management IPC; they
never export stream tokens. Remember reuses the private acknowledged writer
described above. See [host management](host-management.md) for startup, CLI,
request expiry/cancellation, kick completion, protocol version 1 for md-a9f2,
platform support, and validation boundaries. Windows attended management fails
unsupported pending md-wmo6; there is no insecure fallback.

## Session parity grants: monitors and clipboard

The complete canonical vocabulary is now `video,agent,input,monitor,clipboard`
(in that order; any subset, or `none`). Old grants still mean exactly their old
capabilities. Default new grants remain **video only**, never clipboard/monitor.
The same exact vocabulary is used by private NIP-44 `metadesk-caps` entries,
`access add`, migration and attended `requests approve [--remember]`. Approval
cannot add capabilities outside the client's request or the host's supported
mask. W16 Access consumers should render the capability strings from IPC rather
than assuming three bits. No public grant tags or new authorization bypass exist.

Examples (replace an existing grant only after review):

```sh
metadesk-host access add "$CLIENT_NPUB" video,input,monitor
metadesk-host access add "$CLIENT_NPUB" video,clipboard
metadesk-client --host ADDRESS --npub "$HOST_NPUB" --monitor-control
metadesk-client --host ADDRESS --npub "$HOST_NPUB" --clipboard both
```

Clipboard is opt-in at the client (`--clipboard send|receive|both`, default off)
and requires an explicit host grant. The host can further restrict local
clipboard directions with `--clipboard off|send|receive|both`. Send/receive are
local to the endpoint; signaling confirms their intersection from the **client's**
perspective. A grant without negotiated directions grants no clipboard access.
Neither endpoint opens/reads the clipboard before the bound stream grants it.
Receive-only endpoints do not read clipboard contents or query change metadata.
Kicking/disconnecting the session destroys clipboard state and stops sync.

Only valid UTF-8 plain text, including empty text, is sent: at most **64 KiB**,
no NUL, over the TLS-pinned bound stream, not in relay events/DM contents.
Oversized packets are rejected before payload allocation. Incoming writes require
both the clipboard grant and the receive direction. Native revision checks run at
the existing stream/UI cadence; only a changed text value is sent, and echoed or
replayed content is idempotent. Logs contain byte counts, never clipboard text.
This permission exposes whatever text the local user copies during the session;
do not grant it to a peer that should not receive passwords or other copied data.

macOS uses NSPasteboard; Windows uses CF_UNICODETEXT with strict UTF-8 conversion
and bounded native allocation. A busy/unavailable Windows clipboard reports an
error rather than claiming delivery. Linux Wayland/X11 clipboard is **not
implemented in this build**: client opt-in fails explicitly and the host removes
clipboard from supported/granted capabilities. There is no shell-command,
`wl-paste`, XWayland or portal-consent fallback. Linux screen capture still works;
monitor changes require a new local portal selection, not a remote consent bypass.

Quality uses `--max-size WxH --bitrate BPS --fps FPS`, clamped by host policy.
F1 exposes session quality and granted monitor switching. `--save-quality` stores
per-host preferences in a self-encrypted NIP-78 document; it stores no grants.
See [W15 design and verification](reviews/w15-session-features.md) for boundaries.
