# W15 master integration — 2026-11-22

## Integration target and design

Merged local master **e03f4de** into `md/w15-session-features` at **ecad4dc**,
retaining W17 managed lifecycle, W16 Access/Activity and fenced access IPC, and
W14 packaging. Work stayed in the assigned W15 worktree; main was not edited or
built. No rebase, push, upstream merge or bead closure is part of this delivery.

Conflict resolutions:

- `src/core/management_ipc.c`: retain W16's bounded private-policy snapshot,
  `access.list/set/remove`, generation/revision and policy-version fencing,
  acknowledged publish, and peer revocation. Use **64-byte** capability buffers
  in both attended approvals and `access.set`; the previous 32-byte local buffer
  cannot hold all five canonical permissions.
- `src/host/main.c`: retain W15 quality-error refusals, capture controls,
  negotiated clipboard/output state and cleanup alongside W17's `host_running()`,
  managed startup/config exit policy, lifecycle status, endpoint ownership and
  stopping cleanup. Source replacement copies the capture config, preserving
  `no_interaction`. Acceptance rechecks lifecycle, role and grant validity after
  the bounded capture/quality wait; it cannot publish after a stop observed there.
- `tests/test_host_access.c`: run both clipboard exact-capability checks and W16's
  pending/remembered/active peer-revocation checks.

Manager integration:

- Display and edit **Video, Accessibility tree, Input, Monitor selection,
  Clipboard**. Requests default to no permissions; only requested and supported
  known permissions can be selected. Old hosts without `supported` retain the
  legacy three-bit default, never inferred new capabilities.
- Future numeric bits remain visible as `Unknown permissions (0x...)` with no
  grant checkbox. Unknown private-policy tokens disable Save but keep Revoke.
  Known exact policy masks survive editing rather than being silently truncated.
- Keep W16's captured generation/revision/policy-version, dialog invalidation,
  cancellation-first consent and exact remembered-grant behavior. No new grant
  defaults, desktop bus access or clipboard reads were introduced.

## Regression evidence

The real private-relay attended host/client fixture now saves all **31** known
bits through production `access.set`, reads back exactly
`video,agent,input,monitor,clipboard`, rejects the stale prior policy version and
unknown bit 32, then restores its video-only policy before existing restart,
auto-admission and kick checks. This tests serialization, encrypted policy
publication and version fencing together, not merely a formatter mock.

Manager tests request known and future bits while declaring Input unsupported.
They assert new controls are off by default, selectively grant exactly **25**,
save exactly **31** with policy version 8, preserve unknown-bit display, and keep
unknown-token editing fail-closed. Production Access widgets run under a private
Xvfb/session bus, not the GNOME desktop. A host-binding seam stops at the actual
quality wait boundary and requires no acceptance publication; the separate real
managed-process fixture exercises signals, exits and lifecycle IPC.

## Verification

- macOS full suite: **67 passed, 1 skipped** (native AX live opt-in).
- macOS ASan/UBSan: **26 focused gates + native NSPasteboard = 27 passed**.
- macOS TSan: **26 focused gates + native NSPasteboard = 27 passed**.
  Halt-on-error, no suppressions or increased deadlines. Reused
  nostrc/nsync/FFmpeg/system dependencies are not a fully instrumented closure;
  no leak-sanitizer qualification is claimed.
- Linux `-Dmanager=true`, clean build and tests at `-j2`: **69 passed, 1 skipped**
  (live first-key opt-in). All four manager gates passed: model, private UI,
  host IPC, Access UI. The separate real host/client/private-relay run driven
  through GTK controls also passed approval, exact grant edit, revoke and active
  session termination, with its children/private accounts/sockets reaped.
- Windows ARM64 clean task-private patched LGPL FFmpeg build/tests, `-j2`:
  **46 passed, 2 skipped** in SSH/session 0. The two native programs also passed
  separately in the active console: output enumeration **3 checks**, clipboard
  roundtrip **6 checks**, program exits **0/0**. No console Lock/Disconnect or
  global clipboard mutation; the clipboard program uses a private window station.
- Cross-VM SHA-256 checks matched the Mac's final host, IPC, manager Access source
  and the three integration regression files (the manager test assertion-order
  correction was copied before its final Linux run).

Linux uses private HOME/XDG paths and a Landlock sandbox: read-only system
paths, task/tmp writable, no input-device file access, and a seccomp trap for
`UI_DEV_CREATE`. `/dev/shm` serves the private Xvfb fixtures, not desktop input.
No uinput device, GNOME setting, global display or session bus is modified.

Initial harness failures are not counted as passing evidence: Linux's first
sandbox omitted ancestor-directory and private-bus/shared-memory permissions;
Windows FFmpeg initially defaulted to absent gcc instead of CLANGARM64 clang.
Those task-local harness settings were corrected, not production assertions or
security policy. One new manager test initially selected permissions before its
existing disabled-Approve assertion; selection was moved after that assertion,
which remains intact. The first console receipt was empty because CMD parsed
an adjacent numeric exit code as a redirection handle; the task wrapper now
writes both codes with leading redirections to a temporary receipt and renames
it to the final completion file last. Native logs already reported passing
checks, but only complete independently checked 0/0 receipts count below.
No timeout extension, sleep or retry-to-green production code change.

Windows runtime provenance: FFmpeg n9.0.2 archive SHA-256
`6e374ed621e48faa40639307dff48ba6fe574a509977956d2cce9669b7cc27e9`, nv-codec
headers n13.0.19.0, AMF headers commit
`57ee0cfb0e1a08d9041f77435653794c1b012be0`. The repository's zero-fuzz
`VT_BOOL` MF patch was applied to a private source copy. Exact minimal runtime
inventory and LGPL-2.1-or-later license gates passed: H.264 decoder/parser,
`h264_nvenc,h264_amf,h264_mf`; no GPL/nonfree/x264/OpenH264 runtime components.
A native loader probe loaded the product `libmetadesk-0.dll` and verified that
avcodec/avutil resolved to the patched DLLs beside the products, not system GPL
FFmpeg. DLL SHA-256:

- `avcodec-63.dll`: `22479c599243b02382f3fd609bcc60e8c0b523eaf5b94f3ccb8771caa79ce3c9`
- `avutil-61.dll`: `0870903db831e5a7f81f515e6754820d08ac4469d76aa413ca1a121936c42286`

No shared dependency trees, package installations or signature policy were
changed; MSYS build tools were extracted into the task directory only.

## Nostr / independent review boundaries

Manual integration-scope Nostr A–J review found no new protocol smell. Existing
NIP-78 EOSE/OK and private allowlist version/OK boundaries remain intact. IPC
policy changes call W16's existing encrypted publisher, never a new timer,
broad subscription or websocket-send-is-success path. Existing TLS pinning,
exact grants, attended approval, and request/session correlation remain intact.
This is not a repository-wide audit.

RepoPrompt again rejected W15 artifact selection due to unbound-tab provenance.
An Oracle therefore reviewed explicitly supplied exact merge-resolution source
excerpts instead of unrelated selected context. It found no verified defect in
those excerpts, but **did not approve the full W15 diff**. It could not verify
linearization between the last lifecycle check and DM publication. The added
regression proves stop-during-wait refusal; it does not establish an atomic
signal-versus-relay-publication guarantee, nor claim a new one beyond W17.

## Remaining platform boundaries

The original [W15 report](w15-session-features.md) remains the source for native
feature evidence: this Mac has one physical display (mid-session replacement is
seam-qualified, not two-display hardware qualification), and Linux clipboard is
explicitly unsupported, tracked by open **md-n2gq**. Overlay/manual visual tours,
physical NVENC/AMF hardware and Windows network desktop sessions are not newly
qualified by this integration run.

## Cleanup / delivery

- Linux `/home/bizarro/md-w15` removed with zero active task processes; its
  cleanup script removed itself. Independent SSH checks confirmed both absent.
- Mac: normal/ASan/TSan build trees and task-created Python cache removed;
  **28** `/private/tmp/md-w15-*` scratch roots removed, absence verified.
- Windows `C:\Users\bizarro\md-w15` removed with zero active task processes
  and zero task-owned scheduled jobs. A separate SSH existence check confirmed
  absence. Extended-length paths safely handled tar metadata names.
- No shared dependencies, unrelated process, sibling worktree, stash, user key,
  live GNOME setting or desktop input state was changed during cleanup.
- The local merge commit has parents ecad4dc and e03f4de; its hash is reported
  in the handoff/Beads comments rather than embedded recursively here. Master
  and remotes remain untouched. No W15 Beads file changes beyond inherited
  master contents are included.

Tooling: Jev `find-lines`/`filter-search` relevance routing used 17 requests,
86993 input tokens, approximately $0.00365. Suggestions located the relevant
capability paths; source caller blocks, not relevance scores, determined edits.
