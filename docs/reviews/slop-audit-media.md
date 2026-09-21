# Slop Audit — Media / Platform Layer

**Date:** 2026-09-20
**Scope:** `src/core/{capture,encode,decode,input,a11y,bitrate_ctrl,packet,stream,action,platform}.{c,h}` and the nine platform backends (`capture_dxgi.cpp`, `capture_pipewire.c`, `capture_screencapturekit.m`, `encode_videotoolbox.m`, `input_cgevent.m`, `input_sendinput.cpp`, `input_uinput.c`, `a11y_atspi.c`, `a11y_axui.m`, `a11y_uia.cpp`).
**Method:** every finding below was verified by reading the cited lines. Line numbers are from the working tree at audit time.
**Premise:** this code works. The findings are about *shape* — kludges standing in for primitives that already exist, duplication across the three platform backends, and quality patterns that impose recurring maintenance cost.

## Headline

Four structural problems dominate, and each has an unambiguous correct shape:

1. **The a11y diff engine is triplicated verbatim across the three backends** although it operates exclusively on the already-platform-neutral `MdA11yNode`. ~300 lines exist to do one thing. `a11y.c` owns no policy at all.
2. **A11y node IDs are per-walk DFS counters** that `a11y.h` calls "stable" and that the diff matches on. Insert one node and the entire remainder of the tree reports as changed. UIA hands the correct primitive (`GetRuntimeId`) to a handler that `(void)`s it.
3. **All three a11y push-subscriptions discard the event and re-walk the entire desktop over IPC while holding the backend lock.** On macOS `kAXValueChanged` fires per keystroke.
4. **`encode_videotoolbox.m` (456 lines, compiled into every macOS build) has zero callers**, and two of the eight functions `encode.h` declares for it do not exist.

Also notable: a red/blue channel swap between `decode.c` and `render.c`; a discarded `pts` in the encoder API; `SSL_set_tlsext_host_name` mistaken for hostname verification; and four independent copies of a 5 ms `nanosleep`.

---

## Findings Table

| # | Sev | Code | Location | Finding |
|---|-----|------|----------|---------|
| A1 | structural | S4/S7 | `a11y_{atspi.c:198-260,axui.m:322-384,uia.cpp:226-306}` | Diff engine triplicated verbatim; `a11y.c` is pure dispatch |
| A2 | structural | S5 | `a11y_*` `make_node_id` / `next_id = 0` | "Stable" node IDs are per-walk DFS counters; diff matches on them |
| A3 | structural | S5/C4 | `a11y_atspi.c:282`, `a11y_axui.m:470`, `a11y_uia.cpp:602` | Push events discarded; full desktop re-walk under the lock |
| A4 | drift | S1 | all three `nodes_differ` (`:234/:358/:262`) | State *contents* never compared — only `state_count` |
| A5 | load | S1 | all three `find_by_id` | O(n²) diff, run per event; glib already linked |
| A6 | structural | S4 | `a11y.c:183-259` vs three backends | Role vocabulary drift — backend roles a11y.c never heard of are dropped |
| A7 | structural | S4 | `a11y.c:262-286` vs three backends | State vocabulary drift — `*checked*`/`*pressed*` unreachable on 2 of 3 platforms |
| A8 | load | S1 | `a11y.c:183`, `a11y.c:227` | `role_abbrev` and `is_interactable` are two parallel tables over one vocabulary |
| A9 | structural | S5 | `a11y.c:373-380` | 256 KB fixed compact buffer, self-labelled placeholder, silent truncation |
| A10 | drift | S5 | `a11y_atspi.c:320-326` | `atspi_init()` returns 0/1/2; code tests `< 0` — failure reads as success |
| A11 | drift | S5 | `a11y_axui.m:640-650` | Permission denied → log and set `connected = 1` |
| A12 | drift | S4 | `a11y.c:63` | Liveness inferred from `backend_data`; capture has `active`, input has `ready` |
| A13 | load | C4 | `a11y_uia.cpp:183-200`, `:380-400` | Last `IUIAutomationElement` leaked when the 256-child cap is hit (×2) |
| A14 | drift | S4 | `a11y_uia.cpp:186`, `:373` | 2 KB stack arrays per recursion level; other two backends use the heap |
| A15 | structural | S5 | `a11y_axui.m:594-610` | 0.5 s poll loop around `CFRunLoopRunInMode` although `CFRunLoopStop` is already wired |
| C1 | structural | S4 | `capture_screencapturekit.m:139-155` vs `capture_pipewire.c:608-625` | Same frame-slot handoff; the PipeWire copy was fixed, the SCK copy was not |
| C2 | structural | S1/S4 | `capture.h:114`, `capture_dxgi.cpp:196` | Three different `get_frame` timeout policies; no timeout in the HAL contract |
| C3 | structural | — | `capture.h` / `src/host/main.c:203` | Host polls `get_size` every 10 ms; both backends already have a condvar |
| C4 | drift | S5 | `capture_pipewire.c:537` | Stride hardcoded `width * 4`; `chunk->stride` is right there |
| C5 | structural | S5 | `capture_pipewire.c:735-748` | Any portal failure — including user denial — silently falls back to direct connect |
| C6 | load | C4 | `capture_pipewire.c:194-215` | Timeout counted in iterations, not a deadline; steals messages off the shared bus |
| C7 | load | C4 | `capture_pipewire.c:90-124` | Three near-identical `dict_append_sv_*` builders |
| C8 | drift | S5 | `capture_pipewire.c:564`, `capture_screencapturekit.m:107` | Unknown pixel format → "best guess", in two backends, both unreachable |
| C9 | confidence | C4 | `capture_dxgi.cpp:113-140` | `goto fail` jumps over initialised declarations — ill-formed C++ (MSVC C2362) |
| C10 | cosmetic | S1 | `capture_dxgi.cpp:56` | `safe_release()` defined, never called; 12 hand-rolled copies instead |
| C11 | cosmetic | S1 | `capture_dxgi.cpp:175-177` | `(void)ctx;` immediately followed by `ctx->active = true;` |
| C12 | load | S1 | `capture.c`, `input.c`, `a11y.c` | Per-call vtable NULL checks on statically-built vtables — except on `init` |
| C13 | drift | S1 | `capture.c:33`, `input.c:169` | `init(ctx, cfg)` is always `init(ctx, &ctx->config)`; 2 backends `(void)cfg` |
| C14 | cosmetic | S1 | `capture_screencapturekit.m:178,333,397` | Same `@available` gate three times after `backend_create` already returned NULL |
| I1 | structural | S4 | `input.c:305-313` | `set_value` hardcodes `ctrl+a`; on macOS that is beginning-of-line, not select-all |
| I2 | structural | S1/S4 | `input.c:139`, `input_uinput.c:144`, `input_cgevent.m:135`, `input_sendinput.cpp:139` | Four copies of dimension validation; the three backend copies test a never-NULL pointer |
| I3 | structural | S5/C4 | `input_uinput.c:35-43` | `emit()` swallows `write()` failure; every caller then returns 0 |
| I4 | structural | S5 | `input_uinput.c:178`, `input_cgevent.m:159-171` | `init` returns success while not ready; the CGEvent "permission probe" tests nothing |
| I5 | drift | S4 | `input.c:19`, `input_uinput.c:48`, `input_cgevent.m:38`, `input_sendinput.cpp:41` | Four copies of a 5 ms magic delay; one of them is really a udev-settle wait |
| I6 | drift | S5 | `input_sendinput.cpp:206-225` | `si_mouse_scroll` ignores `SendInput`'s return; its two siblings check it |
| I7 | drift | S4 | `input_cgevent.m:122`, `input_sendinput.cpp:118` | Same lookup, two "not found" conventions (`UINT16_MAX` vs `0`) |
| I8 | load | S4 | `action.h:30` vs `input.h:104` | `MD_MAX_KEYS` and `MD_INPUT_MAX_COMBO_KEYS` are the same constant, twice |
| I9 | load | S1 | `input_uinput.c:224-268` | ~130 `strcasecmp`s per typed character |
| E1 | structural | S5 | `encode_videotoolbox.m` (whole file), `encode.h:125-135` | 456 dead lines compiled on macOS; 2 declared functions never defined |
| E2 | structural | S5 | `encode.c:355` | `md_encoder_submit` discards its `pts` argument |
| E3 | structural | S4 | `decode.c:50-65` vs `decode.h:30` / `render.c:60` | libyuv `*ToARGB` produces BGRA; SDL is told ABGR — red and blue swapped |
| E4 | drift | S4 | `encode.c:106-140` vs `encode_videotoolbox.m:352-362` | NV12 copy exists twice; one hardened against stride mismatch, one not |
| E5 | structural | S5 | `encode.c:56-70`, `:195-222` | Every spec §9 encoder parameter is log-and-continue |
| E6 | drift | S1 | `encode.c:437-483` | 4-branch `strcmp` cascade where 2 branches do nothing and all return 0 |
| E7 | drift | S9 | `encode.c:265-290` vs `encode.h:65` | `prefer_nvenc` gates only NVENC; VT and AMF are tried unconditionally |
| E8 | load | S4 | `encode.c:172`, `:505-510` | `bitrate ? : DEFAULT` expressed in two places |
| B1 | drift | S4 | `bitrate_ctrl.c:79-84` | `100000` / `8000000` are literally `MD_ENCODER_MIN_BITRATE` / `MD_ENCODER_DEFAULT_BITRATE` |
| B2 | drift | S5 | `bitrate_ctrl.c:38-64` | Invalid config silently rewritten; `0` overloaded as "unset" on 8 fields |
| S1x | structural | C4 | `stream.c:106-160` | `timeout_ms` is a per-syscall budget, not a deadline |
| S2x | structural | S3 | `stream.c:8-13`, `:568`, `:651-664` | File header describes an echo-based RTT the code does not implement |
| S3x | structural | S5 | `stream.c:515` | SNI set and commented as "for certificate verification"; hostname never verified |
| S4x | drift | C4 | `stream.c:236-253` | Six OpenSSL calls unchecked while building the self-signed cert |
| S5x | drift | C4 | `stream.c:620-623` | Payload `malloc` failure leaves the stream desynchronised |
| S6x | drift | S4 | `action.c:167-171` vs `:108-115` | `region` encoded only for screenshots, parsed for everything — click coords lost |
| S7x | drift | S5 | `action.c:71-76` | Unknown action string parses as success; rejected two layers later |
| P1 | load | S1 | `platform.h:80-99` | `memset_s` branch is dead; the comment admits the mechanism is "fragile" |
| P2 | cosmetic | S3 | `platform.h` | Named for the platform HAL, contains only `mlock` + secure-zero |
| X1 | load | S4 | `stream.c:70`, `a11y.c:143`, `capture_pipewire.c:822-831`, `capture_screencapturekit.m:290-299` | Four hand-rolled "monotonic ms now" |
| X2 | drift | S5 | `a11y.c:143,156,589` | `CLOCK_MONOTONIC` serialised as a wall-clock `"ts"` field |
| X3 | cosmetic | C5 | ~55 sites across the layer | `fprintf(stderr, ...)` is the logging system; no `md_log.h` exists anywhere in `src/` |

---

## A — Accessibility

### A1 · Diff engine triplicated — `structural`, S4/S7
**Observed.** `FlatEntry`, `flatten_tree`, `find_by_id`, `nodes_differ`, `clone_node_shallow` and the ~80-line body of `*_get_diff_unlocked` are byte-for-byte identical in all three backends, modulo `strdup`/`_strdup` and `NULL`/`nullptr`:
`a11y_atspi.c:193-260` + `:389-470`, `a11y_axui.m:317-384` + `:700-790`, `a11y_uia.cpp:221-306` + `:364-450`. `a11y_uia.cpp:218` even carries the comment *"same algorithm as other backends"* and `a11y_axui.m:286` *"same algorithm as a11y_atspi.c"*. Every line of it operates only on `MdA11yNode`, which `a11y.h` defines as platform-neutral.

**Correct shape.** Delete `get_diff` from `MdA11yBackend` (`a11y.h:65`). Move `last_snapshot` into `MdA11yCtx` and implement `md_a11y_diff()` once in `a11y.c` on top of `vtable->get_tree`. The vtable shrinks to `init / get_tree / subscribe_changes / destroy`.

**Cost.** Every change to delta semantics (A2, A4, A5 below) must be made three times in three languages, and history shows it isn't: C1 documents the same class of divergence in the capture layer.

### A2 · "Stable" node IDs are positional counters — `structural`, S5
**Observed.** `make_node_id()` (`a11y_atspi.c:47`, `a11y_axui.m:71`, `a11y_uia.cpp:60`) formats `"n%lu"` from `st->next_id++`, and every tree walk resets the counter: `a11y_atspi.c:395`, `a11y_axui.m:663`, `a11y_uia.cpp:350` all do `st->next_id = 0;`. IDs are therefore pure DFS position. `a11y.h:31` documents the field as a *"stable node identifier"*, and the diff engine matches previous↔current nodes by `strcmp` on it.

**Failure.** Open one menu and every node after it in DFS order shifts by one. `find_by_id` then pairs each node with its neighbour's snapshot, so the tail of the tree emits as `UPDATE`, plus a trailing run of `ADD`/`REMOVE`. The delta channel degenerates into "resend everything" exactly when something changed.

**Correct shape.** Derive the ID from platform identity:
- AT-SPI: `atspi_accessible_get_path()` (or `atspi_accessible_get_accessible_id()` where the toolkit sets it), prefixed by the application's bus name.
- UIA: `IUIAutomationElement::GetRuntimeId()` — the `SAFEARRAY *runtimeId` is already delivered to `HandleStructureChangedEvent` at `a11y_uia.cpp:588` and thrown away with `(void)runtimeId;`.
- AXUIElement: `CFHash(elem)` combined with the owning `pid` (AX has no public stable id).

**Cost.** The whole delta subsystem — `md_a11y_diff`, `md_a11y_delta_to_json`, `md_a11y_tree_patch`, `subscribe_changes` — is built on an identity that does not survive the events it exists to report.

### A3 · Push events discarded, full re-walk under the lock — `structural`, S5/C4
**Observed.** All three change callbacks follow the same shape:
```c
/* a11y_atspi.c:275-294 */
(void)atspi_get_diff_unlocked(ctx, &deltas, &delta_count);   /* inside g_mutex_lock */
```
`a11y_axui.m:449-470` `(void)observer; (void)element; (void)notification;` then `axui_get_diff_unlocked` inside `pthread_mutex_lock`. `a11y_uia.cpp:576-612` `(void)sender; (void)changeType; (void)runtimeId;` then `uia_get_diff_with_unlocked` inside `EnterCriticalSection`.

Each of those calls walks the **entire desktop** across process boundaries (D-Bus / AX IPC / COM), then diffs it O(n²) (A5). The macOS backend subscribes `kAXValueChangedNotification` (`a11y_axui.m:401`), which fires on every keystroke in every text field of every running application.

**Correct shape.** Two changes, both using primitives already in hand:
1. Build the delta from the event payload — `AtspiEvent.source/type/detail1`, `AXUIElementRef element` + notification name, `IUIAutomationElement *sender` + `runtimeId` — re-walking only that subtree.
2. Coalesce: mark dirty in the callback, let a debounce timer (GLib `g_timeout_add`, `CFRunLoopTimer`, `SetTimer`) do the work, and never hold the backend lock across a cross-process walk. Concurrent `md_a11y_walk()` callers currently block behind it.

**Cost.** Subscribing to change events makes the host *slower* than polling, and holding the lock across IPC means a hung a11y peer hangs the caller.

### A4 · `nodes_differ` compares only the state count — `drift`, S1
**Observed.** `a11y_atspi.c:234`, `a11y_axui.m:358`, `a11y_uia.cpp:262`: `if (a->state_count != b->state_count) return true;` and nothing else about states. Any equal-cardinality swap (selected↔focused, expanded↔collapsed where the toolkit substitutes rather than removes) is invisible. Compounded in AT-SPI by `extract_states` (`a11y_atspi.c:64-68`) setting `state_count = states->len` while leaving `NULL` holes for unmapped state enums, so the count doesn't even describe the array.

**Correct shape.** Compare element-wise in the single shared implementation from A1; and in `extract_states`, set `node->state_count` to the number actually stored.

### A5 · O(n²) diff — `load`, S1
**Observed.** `find_by_id` (`a11y_atspi.c:216`, `a11y_axui.m:340`, `a11y_uia.cpp:244`) is a linear `strcmp` scan, called once per node in each of two loops (`:437`/`:447` in atspi and the equivalents). A desktop tree of 5 000 nodes is 25 M `strcmp`s per diff — per event (A3).

**Correct shape.** In the shared `a11y.c` implementation, index the previous snapshot once. glib is already a hard dependency of the AT-SPI backend (`GHashTable`); for the portable version, sort the flattened array by id and `bsearch`, or keep the flat array in DFS order and hash ids into an open-addressed table.

### A6 · Role vocabulary drift — `structural`, S4
**Observed.** Three independent role vocabularies feed two tables in `a11y.c` that know a fourth:
- `a11y_uia.cpp:79-115` emits `"tree item"`, `"document"`, `"heading"`, `"table cell"`, `"tool tip"`.
- `a11y_axui.m:82-129` emits `"table row"`, `"table column"`, `"table cell"`, `"scroll pane"`, `"document web"`, `"tree item"`, `"heading"`, plus an open-ended fallback that strips `AX` and lowercases (`"systemwide"`, `"browser"`, …).
- `a11y.c:183-225` (`role_abbrev`) knows none of those, and `a11y.c:227-259` (`is_interactable`) knows none of them either.

**Failure.** Every Windows tree item and every macOS outline row maps to `"UNK"` and is filtered out of the compact view entirely — the format the agent actually consumes.

**Correct shape.** One `MdA11yRole` enum in `a11y.h` with a single `{ MdA11yRole, const char *wire_name, const char *abbrev, bool interactable }` table in `a11y.c`; backends return the enum, not a string. Unknown natives map to a single `MD_A11Y_ROLE_UNKNOWN` that is at least visible in one place.

### A7 · State vocabulary drift — `structural`, S4
**Observed.** AT-SPI emits 17 state names (`a11y_atspi.c:76-95`); UIA emits exactly five — `enabled/focused/focusable/visible/showing` (`a11y_uia.cpp:118-145`); AXUIElement emits four — `enabled/focused/selected/visible/showing` (`a11y_axui.m:222-252`). `a11y.c:262-286` (`compact_states`) renders `*disabled*`, `*checked*`, `*pressed*`, `*expanded*` — **none of which any backend but AT-SPI can produce.** `is_text_role` + the `<focused>` handling at `a11y.c:325-345` is the only state path that works everywhere.

**Correct shape.** As A6: an `MdA11yState` bitmask in `a11y.h` plus one string table; `MdA11yNode.states` becomes `uint32_t state_mask`, which also deletes three `char *states[8]` hand-rolls and makes A4 a single integer compare.

### A8 · Two parallel tables over one vocabulary — `load`, S1
**Observed.** `role_abbrev` (`a11y.c:183`, 33 `strcmp`s) and `is_interactable` (`a11y.c:227`, 23 `strcmp`s) enumerate the same role vocabulary for different purposes.
**Correct shape.** Merged into the single table from A6.
**Cost.** Adding a role is a two-file, two-function edit today, with nothing to catch a half-done addition — which is precisely how A6 happened.

### A9 · Fixed 256 KB compact buffer — `structural`, S5
**Observed.**
```c
/* a11y.c:373-374 */
/* Allocate a generous buffer; real implementation would use dynamic sizing */
size_t buf_size = 256 * 1024;  /* 256 KB — enough for deep trees */
```
`compact_node` guards each `snprintf` with `if (n > 0 && (size_t)n < buf_len - written)` and, on overflow, simply does not advance `written` — so the next write silently overwrites the truncated tail. The caller gets a short string and no error.

**Correct shape.** Two-pass: run `compact_node` with `buf = NULL, buf_len = 0` to measure (the `snprintf` return value already gives exact lengths), allocate, run again. Or `open_memstream` on POSIX. Either removes the magic constant and the truncation path.
**Cost.** A large desktop yields a silently truncated tree; the agent acts on a partial view of the UI with no signal that anything was dropped.

### A10 · `atspi_init()` failure reads as success — `drift`, S5
**Observed.** `a11y_atspi.c:320-326`: `int ret = atspi_init(); if (ret < 0) { ... return -1; }`. `atspi_init()` returns **0** on success, **1** if already initialised and **2** when the accessibility bus cannot be reached — never a negative. With no a11y bus, `st->connected = 1` and `md_a11y_create()` returns a live-looking context.
**Correct shape.** `if (ret != 0 && ret != 1) { ...; return -1; }`.

### A11 · Permission denial logged and ignored — `drift`, S5
**Observed.** `a11y_axui.m:640-650`: on `!AXIsProcessTrustedWithOptions(...)` the backend prints a message and comments *"Don't fail — permission may be granted while we're running"*, then sets `connected = 1`. Contrast `uia_init` (`a11y_uia.cpp:290-296`), which returns `-1` on failure, and `atspi_init_backend`, which intends to (A10). Three backends, three readiness conventions — and see I4 for the same split in the input layer.
**Correct shape.** Return `-1`; if live re-checking is genuinely wanted, add one explicit `md_a11y_permission_status(ctx)` to the HAL rather than encoding "maybe" as "yes".

### A12 · Liveness inferred from a private pointer — `drift`, S4
**Observed.** `a11y.c:63`: `return ctx && ctx->backend_data; /* backends set backend_data on init */`. `MdCaptureCtx` has an explicit `volatile bool active` (`capture.h:89`) and `MdInputCtx` has `bool ready` (`input.h:67`).
**Correct shape.** Add `bool connected` to `MdA11yCtx` and set it in `md_a11y_create` — three HALs, one convention.

### A13 · Leaked element at the child cap — `load`, C4
**Observed.** `a11y_uia.cpp:186-199` (and the copy at `:373-396`): `while (child && childCount < MD_UIA_MAX_CHILDREN) { ... }`. Exiting on the count condition leaves `child` held with no matching `Release()`.
**Correct shape.** `SAFE_RELEASE(child);` after the loop — the macro is already defined at `a11y_uia.cpp:29`.

### A14 · 2 KB stack array per recursion level — `drift`, S4
**Observed.** `a11y_uia.cpp:186` `MdA11yNode *children[MD_UIA_MAX_CHILDREN];` inside `walk_element`, which recurses to depth 32 → up to 64 KB of stack, plus another copy at `:373`. `a11y_atspi.c:151` and `a11y_axui.m:315` `calloc` the same array.
**Correct shape.** Match the other two backends and heap-allocate.

### A15 · Poll loop around an already-stoppable run loop — `structural`, S5
**Observed.** `a11y_axui.m:594-610` runs `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true)` in a loop, re-locking the mutex each iteration to test `stop_requested`. But `axui_destroy` (`a11y_axui.m:924-928`) already calls `CFRunLoopStop(run_loop)` — the event-driven primitive is wired and then not relied upon.
**Correct shape.** `CFRunLoopRun();` once, terminated by the existing `CFRunLoopStop`. Deletes the flag, the 0.5 s wake-up, and the per-iteration lock.
**Cost.** Two shutdown mechanisms to keep in sync, and a thread that wakes twice a second forever.

---

## C — Capture

### C1 · Frame-slot handoff duplicated; one copy fixed, one not — `structural`, S4
**Observed.** Both backends hold `pthread_mutex_t frame_lock; pthread_cond_t frame_cond; MdFrame pending_frame; bool frame_ready;` plus one held buffer, and both `get_frame` bodies (`capture_pipewire.c:818-845`, `capture_screencapturekit.m:286-311`) are identical down to the `tv_nsec += 100000000L` normalisation.

The producer sides have diverged. PipeWire (`capture_pipewire.c:608-625`) handles the case where the consumer still holds the previous buffer:
```c
if (pw->held_pw_buf) {
    if (pw->frame_ready) { pw_stream_queue_buffer(...); pw->held_pw_buf = NULL; }
    else { /* consumer owns it — requeue the NEW buffer instead */ ... return; }
}
```
ScreenCaptureKit (`capture_screencapturekit.m:139-153`) only handles the first case:
```c
if (sck->frame_ready && sck->held_pixel_buf) { unlock; CFRelease; }
CFRetain(sampleBuffer); sck->held_sample_buf = (void *)sampleBuffer;
```
When `frame_ready == false` but the consumer still holds the frame, the old `CMSampleBuffer` pointer is overwritten — never released, never unlocked — and the consumer's `MdFrame.data` now refers to a buffer whose retain count will be dropped by the *next* `release_frame`.

**Correct shape.** One `MdFrameSlot` in `capture.c`: `md_frame_slot_publish(slot, frame, token, release_fn)` / `_take()` / `_release()`, with the backend supplying the release callback (`pw_stream_queue_buffer` vs `CVPixelBufferUnlockBaseAddress` + `CFRelease`). One ownership rule, one condvar, one timeout.
**Cost.** A leak-then-UAF on macOS under consumer back-pressure, in logic whose Linux twin was already fixed for exactly this.

### C2 · Three timeout policies for one HAL call — `structural`, S1/S4
**Observed.** `capture.h:114` documents `md_capture_get_frame` as *"may block until a frame is available ... Returns 0 on success, -1 on error / timeout"* with no way to express a budget.
- DXGI: `for (int attempts = 0; attempts < 20; attempts++)` around `AcquireNextFrame(100, ...)` (`capture_dxgi.cpp:196-208`) — an unconfigurable 2 s cap, after which an unchanged desktop is indistinguishable from a device error.
- PipeWire/SCK: `while (!frame_ready && ctx->active)` — block forever.

**Correct shape.** Put the timeout in the contract: `int md_capture_get_frame(MdCaptureCtx *, MdFrame *, uint32_t timeout_ms)` returning `0` = frame, `1` = timeout, `-1` = error — the exact convention `stream.c`'s `read_exact` already uses. DXGI then becomes a single `AcquireNextFrame(timeout_ms, ...)` with `DXGI_ERROR_WAIT_TIMEOUT → 1`, and the retry loop disappears.
**Cost.** "No screen activity" and "duplication lost" are the same return value on Windows; on Linux/macOS a stalled compositor hangs the capture thread with no recovery path.

### C3 · Dimension readiness polled at 10 ms — `structural`
**Observed.** `src/host/main.c:186-205` loops `md_capture_get_size()` with `usleep(10000)` until `HOST_CAPTURE_SIZE_WAIT_MS`. The capture HAL exposes no "format negotiated" signal — yet both backends know the exact moment: `on_param_changed` (`capture_pipewire.c:525-531`) and the SCK delegate (`capture_screencapturekit.m:132-134`) both write `ctx->width/height`, and both already own a condvar.
**Correct shape.** `int md_capture_wait_for_size(MdCaptureCtx *, uint32_t timeout_ms)`, signalled from those two sites via the existing `frame_cond` (and set synchronously in `dxgi_init`, which knows the size immediately). The host loop collapses to one call.
**Cost.** A 10 ms-granular startup delay standing in for an event the producer already has, plus a second place where "dimensions are valid" is defined (see I2).

### C4 · Stride computed instead of read — `drift`, S5
**Observed.** `capture_pipewire.c:536-538`:
```c
uint32_t bpp = 4; /* most formats are 4 bytes/pixel (BGRx, RGBx, etc.) */
pw->stride = ctx->width * bpp;
```
PipeWire delivers the authoritative value per buffer in `spa_buf->datas[0].chunk->stride`, which `on_process` already has in hand as `d` (`capture_pipewire.c:578`).
**Correct shape.** `frame.stride = (uint32_t)d->chunk->stride;` in `on_process`, falling back to `width * bpp` only when `chunk->stride == 0`.
**Cost.** Any driver that pads rows (common for GPU-backed SHM) produces a sheared image; the comment's "most formats" is doing load-bearing work.

### C5 · Universal portal fallback — `structural`, S5
**Observed.** `capture_pipewire.c:733-748`: `if (portal_screencast_open(&pw->portal) == 0) { ... } else { fprintf(stderr, "portal unavailable, falling back to direct connect"); pw->core = pw_context_connect(...); }`. `portal_screencast_open` returns `-1` for *every* failure, including `rc != 0` from `Start` — which is exactly the code returned when the user clicks Cancel on the consent dialog (`capture_pipewire.c:352-357`).
**Correct shape.** Distinguish the cases. Return a reason code from `portal_screencast_open` (`PORTAL_UNAVAILABLE` when the bus name is unowned or `XDG_SESSION_TYPE != wayland`, `PORTAL_DENIED` on `rc != 0`), and make the direct-connect path an explicit opt-in — `MdCaptureConfig.allow_direct_connect`, default false — rather than a catch-all `else`.
**Cost.** Denying screen-capture consent downgrades to an unprompted capture attempt, and a genuinely broken portal reports itself only as "no frames ever arrive" (see C2).

### C6 · Iteration-counted timeout on a shared bus — `load`, C4
**Observed.** `capture_pipewire.c:192-215`:
```c
int polls = timeout_ms / 100;
for (int i = 0; i < polls; i++) {
    dbus_connection_read_write(p->bus, 100);
    while ((msg = dbus_connection_pop_message(p->bus)) != NULL) { ...; dbus_message_unref(msg); }
}
```
`dbus_connection_read_write` returns as soon as *any* traffic arrives, so each unrelated session-bus message consumes one of the 600 iterations budgeted for the 60 s consent dialog. The inner loop also pops and unrefs every message that isn't the awaited `Response` — on a shared `DBUS_BUS_SESSION` connection, that silently destroys other subscribers' messages. `dbus_message_iter_get_basic(&args, &result)` at `:203` reads the first argument without checking its type.

**Correct shape.** Register `dbus_connection_add_filter()` for the Response signal and loop on a monotonic deadline: `uint32_t deadline = now_ms() + timeout_ms; while (now_ms() < deadline) dbus_connection_read_write_dispatch(bus, (int)(deadline - now_ms()));`. Same deadline-not-budget correction as S1x in `stream.c`; `md_stream_now_ms()` already exists (X1).
**Cost.** The consent dialog's timeout is whatever the desktop's bus chatter makes it; on a busy session it can expire in well under a second.

### C7 · Three copies of one D-Bus dict builder — `load`, C4
**Observed.** `dict_append_sv_string` (`:90`), `dict_append_sv_uint32` (`:101`), `dict_append_sv_bool` (`:112`) are identical eight-line bodies differing only in the type constant, signature string and value type.
**Correct shape.** `static void dict_append_sv(DBusMessageIter *dict, const char *key, int type, const char *sig, const void *val)` — three call sites collapse to one function.

### C8 · "Best guess" pixel format in two backends — `drift`, S5
**Observed.** `capture_pipewire.c:564` `default: return MD_PIX_CAPTURE_BGRX;` and `capture_screencapturekit.m:106-108` `default: capFmt = MD_PIX_CAPTURE_BGRA; /* best guess */`. Both are unreachable in practice: PipeWire negotiates a closed set of four formats (`capture_pipewire.c:764-769`) and SCK forces `config.pixelFormat = kCVPixelFormatType_32BGRA` (`capture_screencapturekit.m:214`).
**Correct shape.** Return failure for an unnegotiated format — `spa_to_capture_fmt` returns `int` with `-1`, and `on_process` requeues rather than publishing. A defensive arm that guesses wrong silently is worse than no arm.

### C9 · `goto` past initialised declarations — `confidence`, C4
**Observed.** `capture_dxgi.cpp:113` `if (FAILED(hr)) goto fail;` jumps over `IDXGIAdapter *adapter = nullptr;` (`:115`), `IDXGIOutput *output = nullptr;` (`:120`) and `IDXGIOutput1 *output1 = nullptr;` (`:129`). Jumping into the scope of a variable with a non-vacuous initialiser is ill-formed in C++; MSVC reports C2362.
**Correct shape.** Declare and initialise all COM pointers at the top of `dxgi_init`, before the first `goto`, and release them in `fail:`.
**Note.** Flagged `confidence` rather than `structural`: the file only compiles on Windows and this audit could not build it. If Windows CI is green, verify what the compiler actually accepted.

### C10 · Dead helper, twelve hand-rolled copies — `cosmetic`, S1
**Observed.** `safe_release()` (`capture_dxgi.cpp:56-62`) has no callers; `dxgi_destroy` (`:281-296`) and the `fail:` label (`:161-166`) hand-roll `if (p) p->Release();` twelve times. `a11y_uia.cpp` defines and *does* use a `SAFE_RELEASE` macro for the same job.
**Correct shape.** Use `safe_release` at all twelve sites, or delete it and adopt `a11y_uia.cpp:29`'s macro in a shared Windows header.

### C11 · `(void)ctx` then use `ctx` — `cosmetic`, S1
**Observed.** `capture_dxgi.cpp:175-177`: `(void)ctx;` followed immediately by `ctx->active = true;`.
**Correct shape.** Delete the cast.

### C12 · Per-call vtable NULL checks — `load`, S1
**Observed.** `capture.c:41-56`, `input.c:181-235`, `a11y.c:36-59` guard every dispatch with `!ctx->vtable->method`. The vtables are file-scope `static const` aggregates with all members initialised (`capture_pipewire.c:903`, `input_uinput.c:337`, `a11y_atspi.c:625`, …) — the checks can never fire. Tellingly, the one call that *isn't* guarded is `init` (`capture.c:33`, `input.c:169`, `a11y.c:28`), which proves the pattern is reflex rather than policy.
**Correct shape.** Validate the vtable once in `md_*_create` (an `assert` over the required members), then dispatch directly.
**Cost.** ~20 unreachable branches that are never exercised by tests and must be read past on every visit.

### C13 · Redundant `cfg` parameter — `drift`, S1
**Observed.** `capture.c:33` calls `ctx->vtable->init(ctx, &ctx->config)`; `input.c:169` calls `inp->vtable->init(inp, &inp->config)`. The parameter is therefore always the address of a member of its own first argument. `dxgi_init` (`capture_dxgi.cpp:92`) and `sck_init` (`capture_screencapturekit.m:163`) both `(void)cfg;` and read `ctx->config` later anyway. Meanwhile the three input backends test `cfg &&` (I2) on a pointer that cannot be null.
**Correct shape.** `int (*init)(MdCaptureCtx *ctx);` — matching `MdA11yBackend.init`, which already got this right.

### C14 · Triple availability gate — `cosmetic`, S1
**Observed.** `md_capture_backend_create` returns `NULL` below macOS 12.3 (`capture_screencapturekit.m:395-401`), so `sck_start` (`:177-183`) and `sck_stop` (`:332-337`) cannot run on an unsupported OS — yet both re-check, and two of the three print the same message.
**Correct shape.** Keep the gate in the factory; delete the other two.

---

## I — Input

### I1 · `ctrl+a` hardcoded in the platform-agnostic layer — `structural`, S4
**Observed.** `input.c:305-313`:
```c
case MD_ACTION_SET_VALUE:
    if (action->text[0] != '\0') {
        const char *select_all[] = { "ctrl", "a" };
        if (md_input_key_combo(inp, select_all, 2) < 0) return -1;
        input_delay();
        return md_input_type_text(inp, action->text);
    }
```
`"ctrl"` resolves to `0x001D` (`input.c:36`), which `input_cgevent.m:52` maps to `kVK_Control`. On macOS, Control-A is *move-to-beginning-of-line*; select-all is Command-A. So `set_value` on macOS moves the caret and **prepends** the new text instead of replacing the old.
**Correct shape.** Add an `"accel"` / `"primary"` alias to the `key_names` table resolving to `KEY_LEFTMETA` (`0x007D`) under `__APPLE__` and `KEY_LEFTCTRL` elsewhere, and use `{ "accel", "a" }` here. A cleaner alternative is an explicit `set_value` vtable entry so each backend uses its native idiom (`AXUIElementSetAttributeValue` on macOS is better still).
**Cost.** A silent, data-corrupting platform bug in the one place the codebase promised to be platform-neutral.

### I2 · Four copies of dimension validation — `structural`, S1/S4
**Observed.** `input_dimensions_are_valid` (`input.c:139-142`), `uinput_dimensions_are_valid` (`input_uinput.c:144-147`), `cg_dimensions_are_valid` (`input_cgevent.m:135-138`), `si_dimensions_are_valid` (`input_sendinput.cpp:139-142`) — identical predicates, followed by three identical `fprintf` templates (`input_uinput.c:150-157`, `input_cgevent.m:141-148`, `input_sendinput.cpp:145-152`). All three backend copies test `cfg &&`, but by C13 `cfg` is always `&inp->config`. And `input.c:157-166` has *already* substituted the fallback dimensions before `init` is called, so the backend checks validate a state the caller guarantees.
**Correct shape.** One `static inline bool md_input_config_is_valid(const MdInputConfig *)` in `input.h` beside the constants it uses; call it once in `md_input_create`; delete the three backend copies, their dead NULL tests and their three message templates.
**Cost.** Four sites to keep in step; `src/host/main.c:191` makes it five with `host_dimensions_are_valid`.

### I3 · Swallowed `write()` on the uinput fd — `structural`, S5/C4
**Observed.** `input_uinput.c:35-43`:
```c
static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    ...
    if (write(fd, &ev, sizeof(ev)) < 0) { /* ignore */ }
}
```
The fd is opened `O_NONBLOCK` (`:55`, `:99`), so `EAGAIN` is a live outcome. Every caller — `uinput_mouse_move`, `uinput_mouse_button`, `uinput_mouse_scroll`, `uinput_key_event`, `uinput_type_text` — returns `0` unconditionally, so the whole Linux input backend reports success even when nothing was injected. `md_input_key_combo`'s careful release-on-failure logic (`input.c:263-280`) can therefore never trigger on Linux.
**Correct shape.** `static int emit(...)` returning `-1` on short/failed write; `static int syn(int fd)` likewise; propagate in all five vtable functions.
**Cost.** The error-handling machinery above it is decorative on the platform that is the project's primary target.

### I4 · "Ready" that isn't, and a permission probe that probes nothing — `structural`, S5
**Observed.** Two different escape hatches:
```c
/* input_uinput.c:169-178 */
if (!ctx->ready) fprintf(stderr, "input_uinput: ERROR — no virtual devices created. ...");
return 0; /* return success even if not ready — caller checks is_ready */
```
```c
/* input_cgevent.m:159-171 — "Test if we can post events (requires Input Monitoring permission)" */
CGEventRef test = CGEventCreate(NULL);
if (test) { ...; ctx->ready = true; }
```
`CGEventCreate(NULL)` allocates an event object; it does not touch the event tap and succeeds regardless of Input Monitoring status. The comment claims the opposite. Meanwhile `si_init` sets `ready = true` unconditionally (`input_sendinput.cpp:164`). Three backends, three meanings for `ready`.
**Correct shape.** `init` returns `-1` when it cannot inject, so `md_input_create` returns `NULL` and callers cannot forget `is_ready`. For the real macOS check use `CGPreflightPostEventAccess()` / `CGRequestPostEventAccess()` (macOS 10.15+), which is the primitive this code is pretending to be.
**Cost.** `md_input_create` returns a non-NULL handle for a dead injector on two platforms; every caller must remember an out-of-band readiness check that nothing enforces.

### I5 · Four copies of a 5 ms delay — `drift`, S4
**Observed.** `input_delay()` (`input.c:19-22`), `uinput_delay()` (`input_uinput.c:48-51`), `cg_delay()` (`input_cgevent.m:38-41`) — identical `nanosleep` of `5000000` ns — and `si_delay()` (`input_sendinput.cpp:41-44`) `Sleep(5)`. Four magic constants, four comments saying "small delay".

These are not one thing. Two distinct purposes are conflated:
- **Inter-event settling** (`input.c:196` between press and release, `:216` between clicks, `input_uinput.c:281`, `input_cgevent.m:314`, `input_sendinput.cpp:311` between typed characters) — pacing so the target app's event loop keeps up.
- **Device settling** (`input_uinput.c:95` and `:139`, right after `UI_DEV_CREATE`) — waiting for udev to create `/dev/input/eventN` and for the compositor to notice a new device. This one *is* a kludge standing in for an event: the primitive is an `inotify` watch on `/dev/input` (or a `udev_monitor` with `udev_monitor_receive_device`).

**Correct shape.** One `#define MD_INPUT_EVENT_SETTLE_NS 5000000` in `input.h` plus `md_input_settle()` (`input.c`) used by all inter-event sites — and a separately named `uinput_wait_for_device()` that waits on inotify rather than sleeping. Note that `input.c` already spaces the backend calls, so the per-character sleeps inside `uinput_type_text` / `cg_type_text` / `si_type_text` are a third, undiscussed policy applied at the wrong layer.
**Cost.** Changing injection pacing means finding four constants; and the device-creation race is only probabilistically closed.

### I6 · One of three `SendInput` calls unchecked — `drift`, S5
**Observed.** `si_mouse_move` (`input_sendinput.cpp:196`) and `si_mouse_button` (`:222`) both `return (SendInput(...) == 1) ? 0 : -1;`. `si_mouse_scroll` (`:210`, `:219`) discards both returns and ends with `return 0;`.
**Correct shape.** Same check as its siblings; accumulate and return `-1` if either scroll axis fails.

### I7 · Two "not found" conventions for the same lookup — `drift`, S4
**Observed.** `linux_key_to_mac_vk` returns `UINT16_MAX` (`input_cgevent.m:126`); `linux_key_to_vk` returns `0` (`input_sendinput.cpp:122`). Both iterate a sentinel-terminated `{linux_key, native}` table with the same loop, and both print the same "unknown keysym 0x%04x" message.
**Correct shape.** `static bool md_keymap_lookup(const MdKeyMapEntry *tbl, uint32_t sym, uint32_t *out)` shared between the backends (the tables stay platform-specific; the lookup and the error convention should not). Since the keysym space is dense and capped at `0x7D`, a 128-entry direct-index array is simpler and O(1).

### I8 · One constant, two names — `load`, S4
**Observed.** `action.h:30` `#define MD_MAX_KEYS 8` and `input.h:104` `#define MD_INPUT_MAX_COMBO_KEYS 8`, both commented "Maximum keys in a combo". `md_input_key_combo` silently truncates `key_count` to its own copy (`input.c:245-246`), so divergence would drop keystrokes without a diagnostic.
**Correct shape.** `#define MD_INPUT_MAX_COMBO_KEYS MD_MAX_KEYS` (or delete one and include the other's header).

### I9 · Linear name lookup per character — `load`, S1
**Observed.** `char_to_keysym` (`input_uinput.c:224-268`) resolves every character through `md_input_keysym_from_name`, which is a `strcasecmp` walk of the ~130-entry `key_names` table (`input.c:124-136`) — and for single characters it walks the table a *second* time (`input.c:130-135`). Typing a 1 000-character string costs ~260 000 `strcasecmp` calls on top of 1 000 × 5 ms of sleeping.
**Correct shape.** `static const struct { uint32_t sym; bool shift; } ascii_map[128]` initialised once — a direct index replaces both the switch and the lookups.

---

## E — Encode / Decode

### E1 · A dead parallel encoder, compiled and half-declared — `structural`, S5
**Observed.** `encode.h:121-137` declares eight `md_vt_encoder_*` functions; `encode_videotoolbox.m` (456 lines) defines **six** of them — `md_vt_encoder_set_bitrate` and `md_vt_encoder_get_bitrate` (`encode.h:133-134`) have no definition anywhere in the tree. Nothing outside `encode.h` and `encode_videotoolbox.m` references the API at all, yet `meson.build:172` compiles the file into every macOS build. Meanwhile `encode.c:271-275` already reaches VideoToolbox through FFmpeg's `h264_videotoolbox`.

Inside the dead file:
- `kVTCompressionPropertyKey_MaxKeyFrameInterval` is set from a variable named `maxBFrames` initialised to `0` (`encode_videotoolbox.m:277-281`) — a copy-paste that also sets the wrong property to the wrong value.
- All six `VTSessionSetProperty` returns are discarded (`:266-303`).
- `enc->nal_buf = malloc(...)` (`:317`) is unchecked, and `vt_output_callback` writes into it.
- The AVCC→Annex B converter (`:157-190`) sizes its buffer as `totalLength + 256` and then writes `enc->nal_buf[outOffset++]` with no bound check.

**Correct shape.** Delete `encode_videotoolbox.m`, the `#ifdef __APPLE__` block in `encode.h`, and the `meson.build` entry. If a no-FFmpeg path is genuinely wanted later, it belongs behind the existing `MdEncoder` API as a backend vtable — the pattern the other three subsystems already use — not as a second public API surface.
**Cost.** 456 lines compiled and linked that no test or caller exercises, with a header that promises two symbols the linker cannot supply.

### E2 · The encoder throws away its timestamps — `structural`, S5
**Observed.** `encode.c:355` `(void)pts;`, then `:369` `enc->frame->pts = enc->frame_idx++;`. `encode.h:72-79` documents the parameter as *"pts: presentation timestamp in microseconds"*. The capture layer goes to some trouble to produce real timestamps (`MdFrame.timestamp_ns`, `capture_pipewire.c:584`, `capture_screencapturekit.m:113-118`, `capture_dxgi.cpp:238`) and they are discarded here. The dead VT encoder does the same at `encode_videotoolbox.m:328`.
**Correct shape.** `enc->frame->pts = av_rescale_q(pts, (AVRational){1, 1000000}, enc->ctx->time_base);` — with a monotonicity guard if the source clock can jump.
**Cost.** Output PTS is a frame counter, so any capture-side frame drop (which C1's slot logic makes routine) silently becomes a timing error instead of a gap; A/V sync and latency measurements are unrecoverable downstream.

### E3 · Red and blue swapped between decoder and renderer — `structural`, S4
**Observed.** `decode.c:48-66` uses libyuv `NV12ToARGB` / `I420ToARGB` / `NV21ToARGB`. In libyuv's naming, `ARGB` means little-endian `0xAARRGGBB`, i.e. **B, G, R, A in memory**. `encode.c:88-90` knows this and says so:
> `/* libyuv: ARGBToNV12 expects BGRA/BGRx (it calls it "ARGB" in little-endian convention: memory order B-G-R-A) */`

`decode.h:30` declares the output as *"RGBA pixel data"*, and `src/client/render.c:57-60` creates an `SDL_PIXELFORMAT_ABGR8888` texture with the comment *"Our decoded frames are RGBA (R in byte 0)"*. They are not: R is in byte 2.
**Correct shape.** `NV12ToABGR` / `I420ToABGR` / `NV21ToABGR` in `decode.c` — libyuv's `ABGR` is memory order R, G, B, A, which is what `decode.h` and `render.c` both expect.
**Cost.** The remote desktop renders with red and blue transposed. The same trap was documented correctly in `encode.c` and then walked into in `decode.c` because the knowledge lives in a comment rather than in a shared conversion helper.

### E4 · NV12 passthrough copy exists twice, hardened once — `drift`, S4
**Observed.** `encode.c:106-140` copies NV12 row by row and explains why:
> `/* Do not assume the source has FFmpeg's aligned linesizes. */`
with `height % 2`, stride and overflow checks. `encode_videotoolbox.m:352-362` does the same job as two flat `memcpy`s sized from the *destination* stride while reading from a source laid out with the *caller's* stride — a buffer overread whenever `CVPixelBufferGetBytesPerRowOfPlane` exceeds the input stride, which is the normal case for VideoToolbox's 64-byte-aligned pools.
**Correct shape.** One `md_nv12_copy(const uint8_t *src, uint32_t src_stride, uint8_t *dst_y, int dst_stride_y, uint8_t *dst_uv, int dst_stride_uv, uint32_t w, uint32_t h)` in `encode.c`, used by both. (Moot if E1 is applied — which is the point: the duplicate only exists because the second encoder exists.)

### E5 · Every encoder parameter is log-and-continue — `structural`, S5
**Observed.** `set_opt_warn` / `set_opt_int_warn` (`encode.c:56-70`) print a warning and return the code; `try_open_encoder` ignores all 14 return values (`:195-222`). If `preset=p1`, `tune=ull`, `rc=cbr` or `zerolatency=1` is rejected — different FFmpeg builds name these differently — the encoder opens successfully in its *default* configuration, which for NVENC means B-frames and a multi-frame lookahead. The spec §9 low-latency contract silently evaporates and the only evidence is a line on stderr.
**Correct shape.** Collect the options into an `AVDictionary` and pass it to `avcodec_open2`, then check that the dictionary comes back empty — FFmpeg's built-in mechanism for exactly this. A rejected option then means "this codec cannot meet our contract", which should advance the cascade to the next encoder rather than accept a degraded one.
**Cost.** A latency regression that looks like a network problem, with no programmatic signal anywhere.

### E6 · Four-branch cascade where two branches are no-ops — `drift`, S1
**Observed.** `md_encoder_set_bitrate` (`encode.c:437-483`) branches on `strcmp(codec->name, ...)` four ways. The NVENC and AMF branches set the private `"b"` option and `rc_max_rate`; the VideoToolbox branch sets only `rc_max_rate` under the comment *"setting bit_rate on the context is sufficient; VT reads it before each frame encode"*; the x264 branch sets `rc_max_rate` and `rc_buffer_size` under a comment asserting this *"triggers x264_encoder_reconfig"*. Then `:485-487` sets `ctx->bit_rate` for everyone anyway, and the function returns `0` regardless of whether anything took effect.
**Correct shape.** One unconditional body — set `ctx->bit_rate`, `ctx->rc_max_rate`, `ctx->rc_buffer_size`, and `av_opt_set(ctx->priv_data, "b", ...)` treating `AVERROR_OPTION_NOT_FOUND` as benign — returning the real result. The codec-name `strcmp` chain (duplicated from `try_open_encoder`'s own chain at `:193-226`) disappears.
**Cost.** `bitrate_ctrl` cannot distinguish "bitrate applied" from "bitrate ignored", so the AIMD loop (which assumes its output takes effect) may be steering nothing. Adding a fifth encoder means editing two `strcmp` chains.

### E7 · `prefer_nvenc` gates one of four encoders — `drift`, S9
**Observed.** `encode.h:46` documents `prefer_nvenc` as *"true = try NVENC first, false = x264"* and `encode.h:65-66` says *"Tries NVENC if prefer_nvenc is set, falls back to libx264."* The implementation (`encode.c:263-290`) tries NVENC only under the flag, then VideoToolbox, then AMF, then x264 — unconditionally. Setting `prefer_nvenc = false` on a Mac still selects hardware VideoToolbox, contradicting the documented contract.
**Correct shape.** A single ordered `static const char *const cascade[]` chosen from the config (or an explicit `MdEncoderCodec` enum with `MD_CODEC_AUTO`), with the header describing what the code does.

### E8 · Default-or-value expressed twice — `load`, S4
**Observed.** `encode.c:171-173` computes `fps`/`br` with `cfg->x ? cfg->x : DEFAULT` for the codec context, while storing the raw config at `:260`; `md_encoder_get_bitrate` (`encode.c:505-510`) repeats the same fallback expression at read time. `md_vt_encoder_create` (`encode_videotoolbox.m:212-213`) is a third copy.
**Correct shape.** Normalise once in `md_encoder_create` (`enc->config.fps = fps; enc->config.bitrate = br;`), after which every reader is a plain field access.

---

## B — Bitrate control

### B1 · Bitrate bounds duplicated as literals — `drift`, S4
**Observed.** `bitrate_ctrl.c:79-83`:
```c
if (ctrl->cfg.min_bitrate == 0) ctrl->cfg.min_bitrate = 100000;  /* 100 Kbps */
if (ctrl->cfg.max_bitrate == 0) ctrl->cfg.max_bitrate = 8000000; /* 8 Mbps */
```
`encode.h:93` defines `MD_ENCODER_MIN_BITRATE 100000` and `encode.h:50` defines `MD_ENCODER_DEFAULT_BITRATE 8000000` — the same numbers with the same comments. Every other default in this file is a named `MD_BITRATE_CTRL_DEFAULT_*` macro in `bitrate_ctrl.h` (`:41-63`); these two are the odd ones out. The controller's output is then clamped a second time by `clamp_bitrate` (`encode.c:425-429`) against a *different* pair of bounds.
**Correct shape.** `MD_BITRATE_CTRL_DEFAULT_MIN_BITRATE` / `_MAX_BITRATE` in `bitrate_ctrl.h`, defined in terms of the encoder's constants, so the two clamps provably agree.

### B2 · Invalid config silently rewritten — `drift`, S5
**Observed.** `apply_defaults` (`bitrate_ctrl.c:38-64`) treats `0` as "unset" for six fields (so `rtt_low_ms = 0` or `decrease_pct = 0` cannot be expressed), then silently corrects contradictions: `rtt_low_ms >= rtt_high_ms` becomes `rtt_high_ms / 2`; `decrease_pct >= 100` becomes `70`. `md_bitrate_ctrl_create` adds two more (`:84-85`, `min >= max` → `max / 2`). A caller who mis-configures gets a controller that runs a different algorithm than requested, with nothing logged.
**Correct shape.** `md_bitrate_ctrl_create` already returns `NULL` for `!cfg`; return `NULL` for contradictory ranges too, and keep `0`-means-default only where the header documents it. Contradictions are programming errors, not runtime conditions.

---

## S — Stream / Packet / Action

### S1x · Timeout is a per-syscall budget, not a deadline — `structural`, C4
**Observed.** `read_exact` (`stream.c:106-160`) restarts the full `timeout_ms` on every loop iteration — `poll(&pfd, 1, (int)timeout_ms)` at `:131` inside `while (total < n)`, and `ssl_retry_wait(fd, err, timeout_ms > 0 ? (int)timeout_ms : -1)` at `:119`/`:145`. `md_stream_recv` calls it twice (header, then payload), so a peer that dribbles one byte per 99 ms keeps a "100 ms" receive alive indefinitely.
**Correct shape.** Compute the deadline once and pass the remainder:
```c
uint32_t deadline = timeout_ms ? md_stream_now_ms() + timeout_ms : 0;
...
int budget = deadline ? (int)(deadline - md_stream_now_ms()) : -1;
if (deadline && budget <= 0) return 1;
```
`md_stream_now_ms()` already exists at `stream.c:70`. `md_stream_recv` should compute the deadline once and share it across both `read_exact` calls. This is the same defect as C6 in `capture_pipewire.c` — the shared fix is one deadline helper (X1).
**Cost.** Every timeout in the transport is advisory; a slow or hostile peer pins the receive thread.

### S2x · Documented RTT scheme is not the implemented one — `structural`, S3
**Observed.** `stream.c:8-13`:
> *"Latency measurement: ping packets carry the send timestamp in the header's timestamp_ms field. Pong echoes it back. RTT is computed on pong receipt."*

`md_stream_send` stamps `timestamp_ms = md_stream_now_ms()` on **every** packet (`:568`), so nothing can echo anything. `md_stream_handle_pong` (`:651-664`) takes `const MdPacketHeader *hdr` and never reads it, computing RTT from the single-slot `s->ping_send_ms` (`:647`) instead. With more than one ping in flight, every pong is measured against the newest ping.
**Correct shape.** Compute `rtt = now - hdr->timestamp_ms` in `handle_pong`, and give the send path a way to preserve an echoed timestamp (e.g. `md_stream_send_at(s, type, seq, ts_ms, payload, len)` used by the pong responder). Then `ping_send_ms` and its single-outstanding-ping assumption disappear.
**Cost.** `avg_rtt_ms` is the input to the entire adaptive-bitrate loop, and it is wrong whenever more than one ping is outstanding — exactly the condition congestion creates.

### S3x · SNI mistaken for hostname verification — `structural`, S5
**Observed.** `stream.c:514-515`:
```c
/* Set SNI for certificate verification */
SSL_set_tlsext_host_name(ssl, host);
```
SNI tells the server which certificate to present; it does not make OpenSSL check the presented certificate against the hostname. With `verify_peer = true`, `tls_client_ctx` (`:279-281`) sets `SSL_VERIFY_PEER` — so chain validation runs — but any valid certificate for **any** host is accepted.
**Correct shape.** Add `SSL_set1_host(ssl, host);` (OpenSSL 1.1.0+) immediately after the SNI call, before `SSL_connect`. The comment then becomes true.
**Cost.** `verify_peer` reads as "verified" at every call site while the property callers actually want is absent.

### S4x · Unchecked OpenSSL during cert generation — `drift`, C4
**Observed.** `tls_server_ctx_self_signed` (`stream.c:222-253`) discards the returns of `ASN1_INTEGER_set`, `X509_set_pubkey`, `X509_NAME_add_entry_by_txt`, `X509_set_issuer_name`, `X509_sign`, `SSL_CTX_use_certificate` and `SSL_CTX_use_PrivateKey`. A failure in any of them yields a context that is returned as valid and fails later inside `SSL_accept` with an opaque message.
**Correct shape.** Check each against `1` (or non-NULL) and `goto err`, mirroring the explicit checks `tls_server_ctx_from_files` (`:261-268`) already performs a few lines below.

### S5x · Desynchronised stream on allocation failure — `drift`, C4
**Observed.** `stream.c:619-623`: when `malloc(hdr->payload_len)` fails, the function returns `-1` without setting `s->connected = false` and without draining the payload bytes. The next `md_stream_recv` reads payload bytes as a header, fails the version check and *then* marks the stream dead — one packet later, with a misleading cause.
**Correct shape.** `s->connected = false;` before returning, consistent with every other error path in the function (`:600`, `:608`, `:614`).

### S6x · Encoder and parser disagree about `region` — `drift`, S4
**Observed.** `md_action_parse` (`action.c:108-115`) reads a 4-element `region` for **any** action type. `md_input_execute_action` uses `action->region[0]`/`[1]` as the click coordinates for `CLICK`, `DBL_CLICK` and `RIGHT_CLICK` (`input.c:287-296`). But `md_action_encode` (`action.c:167-171`) emits `region` only when `action->type == MD_ACTION_SCREENSHOT`. A click action therefore does not survive `encode → parse`: the coordinates come back as `(0, 0)`.
**Correct shape.** Emit `region` whenever any component is non-zero (or whenever the type uses it), so the two halves of the format agree. The `region[4]` field being documented as *"x, y, w, h for screenshot"* (`action.h:41`) while also carrying click coordinates is the underlying problem — two meanings in one field.

### S7x · Unknown action parses successfully — `drift`, S5
**Observed.** `md_action_parse` (`action.c:71-76`) assigns `md_action_type_from_str(...)`, which returns `MD_ACTION_UNKNOWN` for anything unrecognised, and then returns `0`. The rejection happens two layers later in `md_input_execute_action`'s `default:` (`input.c:324-327`).
**Correct shape.** `if (action->type == MD_ACTION_UNKNOWN) { cJSON_Delete(root); return -1; }` — reject at the boundary where the JSON is still available for a useful error message.

---

## P / X — Platform and cross-cutting

### P1 · Dead `memset_s` branch with a comment admitting it — `load`, S1
**Observed.** `platform.h:21-25` defines `__STDC_WANT_LIB_EXT1__` and includes `<string.h>` — which only works if `platform.h` is the first thing in the translation unit to include it; nothing enforces that. `platform.h:83-88` then guards on `defined(__APPLE__) && defined(__STDC_LIB_EXT1__)`, a macro Apple's libc does not define, under a comment that concedes:
> *"Fragile in practice — fall through to the portable fallback when the macro wasn't early enough."*

The header comment at `:76` nonetheless claims *"macOS: memset_s (C11 Annex K, always available on Apple)"*. The glibc branch (`:89-91`) hand-computes a version comparison for a function that has been in glibc since 2.25 (2017).
**Correct shape.** Two branches: `SecureZeroMemory` on Windows, and the `volatile` function-pointer fallback (`:96-97`) everywhere else — it is correct on every platform and is already the path macOS takes. Delete the `__STDC_WANT_LIB_EXT1__` dance, the Apple branch and the glibc version arithmetic.
**Cost.** Four platform branches, a documented-as-fragile include-order dependency and a misleading comment, all to reach the same behaviour as the fallback.

### P2 · `platform.h` is not the platform layer — `cosmetic`, S3
**Observed.** The file is named for the concept that `capture.h`/`input.h`/`a11y.h` implement ("platform backends selected at compile time") but contains only `md_mem_lock`, `md_mem_unlock` and `md_secure_zero`.
**Correct shape.** Rename to `secure_mem.h`.
**Cost.** Small but recurring: it is the first file anyone looking for the platform HAL opens.

### X1 · Four hand-rolled "monotonic milliseconds" — `load`, S4
**Observed.** `md_stream_now_ms` (`stream.c:70-74`), `now_ms` (`a11y.c:143-147`), and two open-coded copies of the `clock_gettime` + 100 ms + `tv_nsec` normalisation for `pthread_cond_timedwait` (`capture_pipewire.c:822-831`, `capture_screencapturekit.m:290-299`, byte-identical to each other). Both condvar copies use `CLOCK_REALTIME`, so a wall-clock adjustment lengthens or shortens the wait.
**Correct shape.** One `md_time_now_ms()` / `md_time_add_ms(struct timespec *, uint32_t)` in a small shared header, used by all four, plus `pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)` at both condvar initialisation sites (`capture_pipewire.c:695`, `capture_screencapturekit.m:170`). This is also the helper C6 and S1x need for deadline-based timeouts.

### X2 · Monotonic clock serialised as a wall-clock timestamp — `drift`, S5
**Observed.** `a11y.c:143-147` reads `CLOCK_MONOTONIC`, and its value is emitted as the document timestamp in `md_a11y_to_json` (`:156`), `md_a11y_to_compact` (`:378`) and `md_a11y_tree_patch` (`:589`). `CLOCK_MONOTONIC`'s epoch is boot time, so the `"ts"` field is meaningless to any consumer that did not boot on the same machine at the same moment.
**Correct shape.** `CLOCK_REALTIME` for document timestamps; keep `CLOCK_MONOTONIC` for interval measurement (`stream.c`'s RTT, correctly).

### X3 · `fprintf(stderr, ...)` is the logging system — `cosmetic`, C5
**Observed.** No logging macro, level, prefix or sink exists anywhere in `src/` (`session_log.h` is a signed Nostr event log, not a logger). In scope there are ~55 `fprintf(stderr, ...)` sites, including 15 across the three a11y backends (`a11y_atspi.c:325,329,364,381,549`; `a11y_axui.m:527,551,644,841,854`; `a11y_uia.cpp:302,320,330,781,851`). Severity is encoded ad hoc in the message text — `"WARNING —"` (`input.c:145`, `input_uinput.c:173`), `"ERROR —"` (`input_uinput.c:151`, `stream.c:341`), `"warning:"` (`encode.c:53`, `stream.c:190`), and bare messages elsewhere. Some subsystems prefix with the module (`a11y_atspi:`, `capture_dxgi:`), some with the subsystem (`capture:`, `input:`, `stream:`), and some with neither.
**Correct shape.** One `src/core/log.h` with `MD_LOG_ERR/WARN/INFO/DEBUG(subsys, fmt, ...)`, a runtime level, and a single output function. Mechanical to apply, and it is the precondition for ever making the host quiet, structured or redirectable.
**Cost.** Today, silencing or capturing logs is impossible; the per-site `WARNING`/`ERROR` prefixes are the only severity signal and they are inconsistent.

---

## Suggested order of work

1. **A1 + A2 + A5** together — hoisting the diff engine into `a11y.c` is the natural moment to fix the ID scheme and the O(n²) lookup, and it deletes ~300 lines.
2. **E1** — delete the dead VideoToolbox encoder; E4 disappears with it.
3. **E3, E2, I1, S3x, A10** — small, isolated, each a real behavioural defect.
4. **C1** — port the PipeWire ownership guard to SCK, then extract `MdFrameSlot`.
5. **A3** — event-driven deltas; largest remaining performance win.
6. **X3 then C12, I2, I5, X1** — the shared-helper cleanups, once the logger exists to absorb the message templates.
