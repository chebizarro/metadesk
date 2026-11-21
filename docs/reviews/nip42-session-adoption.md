# NIP-42 session adoption qualification

Scope: md-xcy, md-quhj, md-ebm, md-d9tj. Local-only delivery; no push or merge.
Baseline metadesk `bf1577a`; nostrc master
`cfcb86296068c1e4090bc5ce8a486392dc74eb11` (includes `7d8b55724`).
The VM snapshot's relay/session/pool sources, NIP-5F client and public session
headers match that checkout by SHA-256; it has no Git metadata.

## Design

- Each bridge registers its pinned `MdSigner` and owner generation per relay
  with `nostr_relay_set_auth_signer_full(ON_CHALLENGE)`. nostrc owns templates,
  nonce, validation, AUTH transport, OK correlation and reason-specific recovery.
- Signing runs on nostrc's attempt thread, not the reader. The bridge-local
  cancellation token discards non-abortable signer results on stop/revocation;
  nostrc cancels superseded connection/challenge/identity attempts as well.
  Clearing registrations cancels them. Their destroy notifications retain the
  bridge until every attempt has returned; destroy drains before freeing it.
  The shared signer is never shut down by a bridge.
- Results are observed through the AUTH-result callback and checked against
  identity/connection/AUTH generations. Ordinary publication still observes OK.
- Construction installs the owner watch only after the immutable snapshot,
  registrations and subscription exist. A role revoked during construction is
  detected synchronously when installing the watch and construction unwinds.
- Explicit build-tree libraries now beat ABI-compatible installed NIP/signing
  libraries. The NIP-5F regression reproduced with the old installed adapter
  until this preference was fixed. Sanitized linker probes include the runtime.

## md-quhj

Pool callbacks consume `NostrPoolCallbackInfo`. Old-connection boundaries are
ignored, boundaries are ordered by sequence, and AUTH_REFUSED is terminal for
that AUTH generation. A new challenge or reconnect can reopen the path.

AWAIT_AUTH is pending even before the challenge arrives: CLOSED may precede
AUTH on the wire. It denies access without spuriously closing held SSE streams.
Rejected/failed signing and post-accepted auth-required CLOSED yield known deny;
when all relays refuse, readiness is FAILED and held MCP streams close.
Rate-limited/error remain transient backoff; other terminal reasons are held
until reconnect. No blind re-REQ remains in the bridge or its fixtures.

Private tests force pre-AUTH CLOSED delivery after reader OK, delay a CLOSED
across connection loss, test post-AUTH denial, and exercise both SSE-preserving
AUTH reconnect and stream-closing post-AUTH refusal. Constructor revocation is
barrier-driven at the pinned-key read. No sleeps or weakened timeouts were added.

## md-ebm

`NIP-5F host session quoted seal` uses a private Unix signer, private retaining
relay and throwaway keys. It runs real host admission, request correlation,
acceptance publication and stream-token matching. The signer fixture verifies
that decrypt results include quoted JSON for both kind-13 seal and kind-14
rumor. Successful negotiation therefore requires the upstream JSON-unescaping
fix; merely signing a CLI command would not satisfy the test.

## Gates

| Platform/configuration | Result |
|---|---|
| macOS default, master tree | 37 passed, 0 failed |
| macOS all three signers, master tree | 43 passed, 0 failed |
| Linux default, parallel `-j2` and serial `-j1` | 36 passed each, 0 failed |
| Linux all signers, parallel `-j2` and serial `-j1` | 42 passed + 1 skipped each, 0 failed |
| Linux ASan, Nostr + MCP + JSON-RPC + NIP-5F session | 9 passed, 0 failed; no sanitizer reports |
| Linux TSan, both test_nostr modes repeated 20 times | 40 passed, 0 failed; no sanitizer reports |
| Linux TSan, MCP + JSON-RPC + NIP-5F session | 7 passed, 0 failed; no sanitizer reports |
| Old shared trees, all-signers configuration only | macOS and Linux configure successfully; not built |

The Linux skip is the existing opt-in `access session e2e` test
(`MD_ACCESS_SESSION_E2E` not enabled), not the new NIP-5F session regression.
Full suites were also run with `NOSTRC_TEST_ISOLATION=1` and a nonexistent
session-bus address. No live desktop qualification is claimed.

md-d9tj uses private out-of-tree TSan nostrc and nsync builds, with ordinary
libgo channel options (`GO_ENABLE_TSAN=OFF`), `setarch -R`, and
`TSAN_OPTIONS=halt_on_error=1`. No suppressions file or option is present.
`ldd` confirms the instrumented nsync is loaded. Positive control with system
uninstrumented nsync exits 66 with a TSan report, demonstrating the detector
is active (not evidence of a production race). See
[private sanitizer recipe](../testing-sanitizers.md).

ASan additionally exposed an existing 42-byte response leak in the JSON-RPC
NULL-safety test; the test now frees its response. Leak detection remains on.

## Nostr Protocol Audit — adoption diff (A–J)

Top risks: none remaining verified in the diff.

Oracle review covered all changed files via explicit diff/source attachments.
Its one verified introduced finding was the construction/revocation race above;
the watch ordering was corrected and constructor cancellation tested. This
source-derived race was not claimed as a reproduced TSan finding. No second
Oracle approval is claimed after the correction.

- A/B/I: no introduced polling or timeout-based subscription closes; backoff is
  reason-specific. Fixture deadlines are deadlock bounds, not delivery policy.
- C/D: AUTH results, OK, CLOSED and EOSE are observed; stamped frames settle
  catch-up/refusal. AUTH acceptance is never treated as an authorization grant.
- E/F/G: tight merged filters, deduplication and bounded DM workers are retained;
  no new relay queue abstraction is introduced.
- H: challenge-driven library sessions, not hardcoded relay authentication.
- J: barriers and protocol boundaries replace blind retry/count assumptions.

No remaining verified findings or new beads. Raw gate logs and source manifest
are retained locally under `build-adopt-all/adoption-evidence/`; private VM trees
are removed after collecting them. Shared nostrc source/build trees and the
VM's live GNOME session are not modified.
