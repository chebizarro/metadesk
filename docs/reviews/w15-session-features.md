# W15 session parity

## md-wvd2 — negotiated quality

Optional `quality` objects carry `max_width`, `max_height`, `bitrate` (bps),
`fps` in the authenticated request/accept and secret-free bind ACK. Missing
means legacy host defaults; malformed values fail rather than default. Explicit
non-default preferences against an old host produce an upgrade error.

Host CLI settings are ceilings, intersected per dimension/rate with the client.
Bitrate is 100 Kbps–100 Mbps, FPS 1–240, dimensions 2–16384 or zero/unbounded.
Native capture retains the host cap (SCK hardware, DXGI GPU, PipeWire CPU).
An additional session downscale uses bounded reusable CPU scratch, preserving
native output coordinates. The capture owner validates resolution/FPS/bitrate as
one transactional codec replacement before accepting them. Unsupported admission
gets `quality_unsupported` without killing the host; unsupported live changes
retain the previous stream and return a visible error. Pacing cannot be bypassed
by repeated controls, and codec timestamps count paced submissions rather than
native sequence gaps. RTT AIMD is bounded by the negotiated ceiling. Settings are ceilings/targets, not a promise that a
hardware encoder emits an exact byte rate or a static desktop produces new frames.

Client: `--max-size 1280x720 --bitrate 2000000 --fps 30`, or F1 → Session quality.
Overlay changes are temporary; `--save-quality` persists CLI preferences per host
in self-encrypted NIP-78 kind 30078, d=`metadesk-session-preferences`, version 1
`hosts[hex].quality`. Unknown fields/other hosts are preserved. The host key is
inside ciphertext, not a public d-tag. Reads complete at EOSE; writes require
positive relay OK. Replacements use NIP-33 created-at/lowest-id ordering. Local
concurrent saves are rejected; concurrent writers on different devices remain
Nostr last-writer-wins, not distributed CAS. No grant is stored or inferred here.

Development checkpoint (superseded by final gates below): macOS full suite 61 pass / 1 explicit native AX skip before
preference integration; focused quality, real host-dispatch seam and encrypted
preference relay roundtrip 3/3 after integration. Real Mac SCK host/client through
private relay and external NIP-5F signers decoded 640x416 under requested
640x480, 1 Mbps, 15 FPS; host logged applied settings. No public relay or input
injection was used. Final cross-platform/sanitizer gates are recorded below; this development
checkpoint alone is not those qualifications.

## md-qb6t — display selection

Requests optionally name a stable `output` (`id:N`; DXGI also `all`). Accept DMs
include a bounded `displays` snapshot (selector, name, desktop rect, selected,
switch support). `--monitor-control` requests the new `monitor` capability;
`--output id:N` also requests it. Defaults still request only the old three caps.
The F1 Displays picker sends a bound-stream monitor control. Initial selection
and changes require an exact monitor grant. No grant is inferred from video.

SCK enumerates active CG display IDs, which its SCDisplay selector accepts.
DXGI enumerates attached adapter outputs using its existing physical geometry
helpers. The capture owner starts and obtains a frame from a new source before
replacing the old source, discards the encoder's old queued frames, and clears
old sequence attribution even for identical sizes/rectangles. Capture-sync
continues to attach geometry by capture generation. Late completion is fenced
by the session control epoch/revision; failed replacement retains the old source.

Linux ScreenCast portals, including GNOME, expose only the locally consented
source; they cannot enumerate/select arbitrary physical outputs remotely without
new consent. The advertised list is one `portal` entry with switching disabled.
Explicit remote initial selection fails with `output_unavailable`, never silently
captures a different display. This also conservatively applies to wlroots portal
sessions; no compositor-specific consent bypass is attempted.

Evidence: native Mac enumeration exposes **one** display (`id:1`). Unit/parser,
production host-switch seam (equal-size different rect, stale input dropped,
replacement failure rollback), existing geometry/capture gates: 4/4 pass. A real
second-display switch is unavailable on this machine; seam evidence is not
multi-display hardware qualification.

## md-vex4 — explicit clipboard permission

Added `clipboard` bit 4 (monitor is bit 3), with canonical private-grant and
attended-approval parsing. Default grants/client directions remain off. Directions
are intersected in the request/accept and bind ACK; the stream enforces the grant
and receive direction independently. The 64 KiB text packet is bounded before
allocation, UTF-8 checked (including overlongs, surrogates, NUL), and never logged.
Native revision metadata is checked only with send permission; receive-only does
not inspect native clipboard state. Content dedup prevents echo loops, including
an intervening local edit between a native write and the next revision check.

Native implementations: macOS NSPasteboard, Windows CF_UNICODETEXT. Linux clipboard
is explicitly unsupported, never a silent no-op. This is a remaining platform
limit, not Linux clipboard qualification. New caps are visible in the existing
host IPC snapshots; `src/manager` was not changed.

Development checkpoint (superseded by final gates below): fake-backend grant/direction/UTF-8/bounds counters, native private
NSPasteboard test, host unauthorized clipboard/pre-allocation oversized-packet
seam. Real Mac host/client with private relay and NIP-5F signers sent and received
text in both directions over the bound stream. The fixture snapshots **all**
original pasteboard types in RAM, refuses incomplete/oversized snapshots, and
restores them after both products stop; restoration succeeded. The ungranted
real session reported no reads/writes and no clipboard packets; unit counters
independently prove zero native accesses without the bit. No user clipboard
contents or raw logs are retained in the repository.


## Final verification — final W15 source, 2026-11-22

| Gate | Result / boundary |
|---|---|
| macOS full suite, NIP-5F/client/tools enabled | **66 passed, 1 skipped** (native AX live opt-in) |
| macOS ASan + UBSan focused gates | **20/20 passed**, halt-on-error, no suppressions |
| macOS TSan focused gates | **20/20 passed**, halt-on-error, no suppressions |
| Native Mac host/client, private relay + external NIP-5F | Quality and explicit initial display selection decoded **640x416** under 640x480 / 1 Mbps / 15 FPS; clipboard bidirectional and ungranted cases passed; full pasteboard restoration passed |
| Linux ARM64 clean compile + full suite, `~/md-w15`, `-j2` | **64 passed, 1 skipped** (first-key desktop opt-in) |
| Windows ARM64 clean compile + full suite, `C:\Users\bizarro\md-w15`, `-j2` | **46 passed, 2 skipped** in SSH/session 0 |
| Those two Windows native cells in active console | **2/2 programs passed**, recorded exits 0/0: DXGI enumerated one physical output plus `all`; production clipboard backend round-tripped UTF-8 in a private window station and restored/closed its objects |

The 20 sanitizer gates cover quality policy/scaling, wire/native outputs,
clipboard bounds/directions/native NSPasteboard, capture geometry, host binding
(including codec rollback/admission recovery and stale-epoch packet rejection),
client refusals, session JSON/correlation, encrypted preferences including negative
OK, private allowlist host/client, attended host/client, capability grants,
NIP-44/ACL/AUTH, stream transport, bitrate control and management IPC. Metadesk
is instrumented; reused nostrc/nsync/FFmpeg/system dependencies are **not** a fully
instrumented closure. No leak-sanitizer qualification is claimed.

VM products were rebuilt from clean objects after export, not trusted solely on
archive timestamps. Cross-VM SHA-256 checks matched final Mac `host/main.c`,
`core/encode.c` and `client/main.c`. Linux ran with private HOME/XDG paths, no
shared display/session-bus variables, and a task-local Landlock allowlist plus a
seccomp trap for `UI_DEV_CREATE`. The input-device paths were inaccessible.
An earlier strace diagnostic recorded **zero `/dev/uinput` open attempts**; final
standard gates retained kernel restrictions without ptrace overhead. No uinput
device or GNOME setting was changed. Linux `--clipboard both` exited 1 with the
explicit unsupported-backend diagnostic.

Windows used a task-private FFmpeg n9.0.2 LGPL build, applying the repository's
MF `VT_BOOL` low-latency patch to a private source copy. The source archive SHA-256
was `6e374ed621e48faa40639307dff48ba6fe574a509977956d2cce9669b7cc27e9`.
Headers: nv-codec-headers n13.0.19.0 and AMF
`57ee0cfb0e1a08d9041f77435653794c1b012be0`. Exact runtime inventory passed:
H.264 decoder/parser and `h264_nvenc,h264_amf,h264_mf`; QSV is disabled on ARM64.
Both runtime licenses reported LGPL-2.1-or-later. A probe loading the product core
resolved avcodec/avutil to the DLLs beside the test executables, not the shared
MSYS2 GPL installation:

- `build/avcodec-63.dll`: `39d5abd94e1b391686e24b39c05783f26535ed3bda5be58f997468e0f702dcd4`
- `build/avutil-61.dll`: `5c8d1b28d4f698304647d35591a618d1e1418680df56ef2fa22c3259d560be5d`

This qualifies the native MF path and isolated clipboard APIs, **not** physical
NVENC/AMF hardware or a Windows-host/network-client desktop session. Console
jobs used the existing limited interactive runner; no Lock/Disconnect, input
injection or global clipboard mutation was performed there. Two console task
program exit codes were checked independently of runner completion.

### Development failures and corrections

- Windows MFT rejected the arbitrary 32x32 resize fixture. FPS/configuration tests
  now use supported 640x360 -> 320x180 geometry, with explicit invalid-config and
  production host rollback/recovery assertions retained. This exposed the need
  for transactional full-quality validation rather than silent dropped frames.
- Session-0 DXGI inventory is inapplicable. Wire assertions are a separate test;
  native assertions still run unchanged in the active console and passed.
- OpenH264's existing cache test correctly rejected a shared writable temporary
  ancestor. Gates use an owner-only task TEMP/TMP directory; no cache-security
  checks were weakened. The final offline-fetch test passed.
- An initial export glob accidentally omitted `build-minimal-lgpl.sh`; exports now
  use the tracked/unignored file manifest. Initial traced Linux relay fixtures
  hit watchdogs under tracing overhead; that run is not reported as a pass.
- Synthetic encoded packets in the rebind fixture now carry the actual host
  session/geometry tag. Receipt assertions were not weakened. Old-session packets
  are explicitly tested to be dropped, while the current session delivers.

The producer-loss admission test exercises the existing five-second first-frame
failure watchdog with no producer, requires refusal/no publish and fences late
work. It uses an unreserved negotiating session, not an already minted token.
No sleeps, extended deadlines, added codec feed frames or weakened assertions
were used to turn failures green.

## Nostr Protocol Review — W15 diff against e531fa6

Manual source review covered changed session serializers/dispatch, new NIP-78
subscribe/read/publish paths and their existing EOSE, publish, owner/cancellation
and DM integration. **No remaining A-J finding in that scope.** This is not a
repository-wide Nostr audit.

- One merged REQ includes narrowly scoped NIP-78 author/kind/d-tag/limit filters;
  it does not replace borrowed pool filters or poll/reopen subscriptions.
- Preference reads use EOSE/CLOSED/connection-state completion. Writes inspect
  positive relay OK; the negative-OK fixture proves refusal is not success.
- Existing NIP-11/NIP-42, CLOSED recovery, request-id/rumor freshness, event-id
  dedup, private-only grants and host/client role ownership remain in place.
- NIP-33 preferences use newest-created-at / lowest-id tie ordering and preserve
  other hosts/unknown fields. They are preferences, never grant authority.
- TLS certificate pins remain inside authenticated accept DMs; one-use stream
  proof and exact grant/direction/quality ACK checks precede application traffic.
- Clipboard revision checks and media pacing are local operational cadences,
  not substitutes for relay delivery. Native text never enters relay events.

The requested Oracle review was **blocked**, not passed: this turn's scheduled
worktree binding rejected the correct W15 git artifacts as provenance-mismatched.
The Oracle explicitly inspected metadata only and did not review source. No
findings or approval are inferred from that unavailable independent review.

## Delivery / limits

Primary feature commits: `369203d` (md-wvd2), `5d81af5` (md-qb6t), followed by this
clipboard/integration commit (md-vex4), which also contains cross-platform
codec-limit, frame-epoch/pacing and regression hardening of the first two.

- Linux clipboard remains explicitly unsupported; native Wayland/X11/portal
  implementation is tracked in **md-n2gq**, not silently claimed as delivered.
- This Mac has one active output; physical second-display switching is unqualified.
  Transactional replacement, equal-size geometry changes, old-frame input drops
  and failed replacement retention are covered by production seams.
- Output inventory is refreshed for each acceptance; reconnect after hot-plug
  to refresh an already-open overlay list. Overlay controls compiled on Mac/Windows;
  runtime evidence used the CLI and production control seams, not a manual UI tour.
- The monitor grant authorizes advertised outputs; `--capture-output` is an initial
  selection, not an output allowlist. No grants are implicit or on by default.
- No manager/service/packaging sibling work was edited. Branch remains based on
  e531fa6; subsequent sibling landings still need normal integration by the lander.
- No Beads files are committed; beads remain open. No push or merge was performed.

## Cleanup receipts

- Linux `/home/bizarro/md-w15` removed with zero active task processes; a
  separate SSH existence check confirmed absence. The cleanup script removed itself.
- Windows `C:\Users\bizarro\md-w15` removed with zero active task processes
  and zero task-owned scheduled entries; a separate SSH existence check confirmed
  absence. A tar-created trailing-dot AppleDouble file required the Win32
  extended-length path namespace for deletion; no shared files were removed.
- Mac: all three W15 build trees and the task-created Python cache removed;
  **97** `/private/tmp/md-w15-*` paths removed, with absence checked afterward.
- No stash, sibling worktree, shared dependency installation, user key, display
  setting or unrelated process was modified during cleanup.


Tooling note: Jev was used for `find-lines`/`filter-search` relevance routing
(33 requests, 246896 input tokens, approximately $0.01037 from call receipts).
Host lifecycle caller blocks needed widening beyond suggested snippets; source,
not relevance scores, determined the implementation. RepoPrompt could not read
this unbound worktree, so edits/builds used absolute paths in the assigned W15
checkout. The main checkout was never edited or built.
