# W8 — private host allowlist (md-bmiw)

## Design and migration

Worktree: `md-w8-private-allowlist`, branch `md/w8-private-allowlist`, base
`0e91e463b2baff1fca2ff6daca1262bdaa1d5858`. No host admission/main.c or nostrc
source changes. Uses existing `MdSigner` NIP-44 operations for local and external
signers; self peer is the saved host-role public key. The public envelope has
only `d=metadesk-allowlist`; NIP-51 private items contain membership and v1 scopes.
Unknown private string-array items survive edits, encrypted and inert. Public
unknown/event-level metadata is not copied into the replacement.

All legacy public v1 grants and mixed lists are review-required, with zero
session/MCP authority. `access list` labels every member and `add`/`remove` refuse
implicit conversion. `access migrate [NPUB CAPS ...]` requires every unique source
member exactly once with an explicit canonical scope, including `none`. It
normalizes duplicate membership/scope records only after that explicit review.
Unreadable private content cannot be mistaken for an empty list or migrated.

The shared staged edit reserves the base under the ACL mutex, encrypts/signs
outside it, and commits only after real positive OK and an unchanged version.
Negative OK, crypto/signer failure, cancellation or concurrent replacement does
not commit the candidate locally. A reflected candidate EVENT before OK is not
authority. One positive OK is sufficient, not quorum storage. A relay may have
stored an event despite a lost OK/conflict; reread before retrying.

Historical public relay/observer copies **cannot be erased**. Migration protects
future list contents, not earlier disclosure. NIP-44 also leaves the author,
address, timestamps and approximate size public, without forward secrecy.
Older clients fail closed on private grants; upgrade the MCP consumer too.

`md-ah9.3` was read and checked: the MCP ACL restores the same host role and
queries core grants, so private `agent`/`input` updates remain live. Static MCP
flag/file grants remain independent and additive. No alternate MCP decoder.

## Tests and review corrections

- Encode/decode: real nostrc NIP-44 and external NIP-5F, strict JSON, malformed
  scopes/arity/duplicates, foreign first-d address, private-only wire envelope,
  unknown private item preservation, newer unreadable policy denies without
  stale fallback, re-signed ciphertext MAC tamper and foreign-key ciphertext.
- Escaped U+0000 in private keys/scopes is rejected before cJSON values cross
  into C-string policy APIs; a literal escaped backslash remains valid metadata.
- Migration: complete/duplicate/partial confirmations, empty migration, signer
  and encryption rejection, pre-OK echo, source preserved before OK/after reject,
  concurrent replacement during encrypt/sign/publish, cancellation during
  encryption, and late OK after stop.
- Real host/client processes on this Mac: throwaway saved NIP-5F accounts,
  `access add` private publication, NIP-17 request/accept, authenticated loopback
  TCP session_bind/ACK. No capture/input, user keys, public relay, FIPS daemon,
  routing or desktop-device changes. Reproducible Meson gate and standalone
  `tests/access_private_session.py`.
- Oracle review was limited to supplied source/design excerpts because the
  worktree artifact-selection route failed. Its escaped-NUL and transaction
  failure-coverage findings were addressed; no second review was requested.
- Initial broad tests exposed a public-policy seed in `direct_ip_session.py`,
  updated to real encrypted content. A failed crypto operation in the NIP-5F
  fixture originally dropped the transport rather than sending an RPC error;
  it now sends explicit rejection. The larger CLI coverage is split into normal
  access and migration gates, with the original 120-second bounds unchanged.
- The first VM transfer excluded `build*` at every depth and omitted the tracked
  FFmpeg `build-minimal-lgpl.sh`. The exclusion was corrected to root build
  directories; no packaging source change was made to fix the resulting gate.

## Nostr Protocol Audit — W8 diff, A–J

Top risks: no remaining introduced protocol mismatch found. Independent source
review covered the changed decoder/builder/edit/CLI, private relay controllers,
capability/MCP/direct-IP seeds, and the new real-process fixture; it also traced
the existing publish, subscription, history and AUTH paths that they reuse.

### Findings

No remaining verified A–J finding in this diff. Parser and transaction-test
findings above were fixed rather than waived. No claim of whole-nostrc audit.

### Good patterns

- **A/B/D/I:** persistent REQ, real EOSE/CLOSED/connection-state readiness,
  condition variables; no new polling, sleeps or timeout-based success/CLOSE.
- **C:** `publish_all` checks every per-relay `publish_and_wait` result. Existing
  NIP-42 session handles challenges/acceptance/rejection. Migration reuses this
  path, with positive OK and base-version fences, not websocket-send success.
- **E:** unchanged filter is exactly kinds:[30000], authors:[host],
  #d:[metadesk-allowlist], limit:1; raw author/signature/address checks precede
  decryption. The first public d is authoritative; private d items are inert.
- **F:** timestamp/lower-ID replacement, duplicate/older replay no-op for policy,
  unconfirmed-ID echo suppression, idempotent ordinary upserts/removals. Newer
  authenticated unreadable content revokes authority instead of old-key fallback.
- **G/H:** no relay queue facade or new relay assumptions; NIP-11 and reconnect
  remain in the existing bridge/nostrc path. One-OK, not quorum, is explicit.
- **J:** tests hold real EOSE/OK and signer phases using semantic hooks/events.
  Process/condition deadlines are failure/cleanup bounds. `p.poll()` is a single
  assertion at a held boundary, not a polling loop. No desktop input test opt-in.

### Suggested order

No remaining W8 protocol remediation. Keep the unrelated baseline macOS clock
binding failure separate; do not mask it or change sibling build/linker work.

## Validation receipts

Final production-code gates:

| Gate | Result |
|---|---|
| macOS full suite, all signer backends | 54 passed, 1 explicit live AX/video skip, 0 failed (55 total) |
| Linux VM full suite, all signers, `-j2` | 52 passed, 1 explicit opt-in access-session skip, 0 failed (53 total) |
| macOS ASan, touched Nostr/access/MCP/process gates | 9 passed, 0 failed |
| macOS TSan, same gates | 9 passed, 0 failed |
| Real local Mac host/client authenticated loopback | passed standalone and as a suite gate |
| `git diff --check` | clean |

Sanitizer gate set: `nostr NIP-44`, `nostr ACL and AUTH private relays`,
`access_cli arguments`, `access_cli private relay`, `access migration private
relay`, `capability grants private relay`, `mcp-proxy NIP-98 e2e private relay`,
`private allowlist real host/client loopback`, `NIP-5F host session quoted seal`.
All use real private WebSocket fixtures and throwaway keys. A final guard in the
crypto fixture explicitly refuses to turn a sanitizer exit/report into an
expected crypto rejection; both CLI sanitizer gates were also checked with it.

No public-relay tests, live AX/video qualification, real FIPS transport or VM
uinput device creation is claimed. Linux `MD_INPUT_LIVE_TESTS` was unset and the
input gate explicitly skipped platform device creation. The host process
fixtures use `--no-input --no-capture`.

Dependencies: dedicated nostrc master `cfcb86296`, unchanged source. macOS normal
build uses its `_build`; sanitizers use W8-only CMake trees with matching C/C++
and linker sanitizer flags, normal libgo options, and vendored **instrumented
nsync** (`NOSTR_USE_SYSTEM_NSYNC=OFF`), not the uninstrumented system copy. No
suppressions. Linux uses `/home/bizarro/nostrc-master/_build` at `cfcb86296`.
Every Linux compile/test invocation is `-j2` in `~/md-w8` on `192.168.64.3`.

Baseline check: a clean `git archive 0e91e46` built separately under
`/private/tmp/md-w8-baseline-*` reproduces exactly
`clock_gettime binds to libSystem`: `libmetadesk.0.dylib: clock_gettime is not
imported from libSystem` (0 pass / 1 fail). `nm` shows that baseline importing
`_clock_gettime` from libnsync. The final W8 binary imports it from libSystem and
the unchanged clock-binding gate passes; no clock-specific fix was made or is
claimed by this lane. No main checkout build was used.

Context triage used Jev `find-lines`/`filter-search`: 28 request windows,
219121 input tokens, approximately $0.009203. It narrowed large source/docs
reads; claims were checked in actual W8 source. The diff-smell search was only
partial relevance, so the actual publisher/subscriber paths were read directly.


Specification references: [NIP-51 private items](https://github.com/nostr-protocol/nips/blob/master/51.md)
and [NIP-44](https://github.com/nostr-protocol/nips/blob/master/44.md).


Cleanup receipt: W8-only normal/sanitizer/dependency/baseline build trees, edit
scripts and scratch logs under `/private/tmp/md-w8-*` were removed and absence
verified. VM `~/md-w8` and W8 `/tmp/md-w8-*` logs were removed and absence verified.
The real-process fixtures reaped children, closed private servers and removed
their temporary homes. No task-created stashes, routes or desktop devices.
The assigned worktree/branch remains for landing; nothing was pushed or merged,
no beads were closed, and `.beads/` is excluded from the commit.
