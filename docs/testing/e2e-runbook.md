# Running the native host ↔ client matrix (md-5zi)

This is an attended acceptance procedure, not an unattended input bot. The
[owning matrix](e2e-matrix.md) includes all **32** host/client/transport combinations,
including same-platform pairs. [L7's results](results/2026-11-21-l7.md) retain
Round 1 blockers and Round 2 native streaming/failures. Neither partial streaming
nor component results are complete pairing acceptance.

## Safety and reproducibility

- Use an assigned worktree on each endpoint; record `git rev-parse HEAD`, OS/build,
  architecture, dependency commits, encoder/decoder and display geometry/scale.
  Record transport implementation/version separately from metadesk/nostrc.
- Use a fresh private home/config/runtime for accounts and capture tokens. Resolve
  symlink parents (`Path(home).resolve()` on macOS); account persistence deliberately
  rejects symlink components. Do not change the operator's saved account/allowlist.
- Use throwaway host/client/unauthorized identities, a controlled private relay,
  and task-owned signer daemons. Explicitly pass `--relay` to **every** account,
  access, host and client operation that uses relays. Never use the default public
  relay. A bunker URI contains a secret: do not commit it or raw argv/logs containing
  it; restrict credential files to 0600 and fixture directories to 0700.
- The host signer identity must equal the FIPS daemon identity. Save the host role
  and grant only the test client's required capabilities (`video,agent,input` for
  the complete matrix); verify membership/scope by reading the allowlist back.
  Keep an unlisted signer for negative checks. Never bypass authorization to make
  direct IP work.
- Input targets must be windows created by this run. Stop if focus leaves them.
  No locking the Windows console. No live-GNOME uinput devices, bus restarts,
  provider killing, or desktop/session replacements. Device-free unit tests are
  distinct from live input. Permission denial/revocation requires an isolated
  identity/desktop; do not revoke the user's existing grants.
- A deadline is a failure bound, not proof of readiness/completion. Observe relay
  `OK`/`CLOSED`/`AUTH`, `EOSE` catch-up, correlated session acceptance and first
  decoded frame; do not sleep to infer successful negotiation.

## Build and automated native probes

macOS, inside the assigned metadesk worktree (do **not** update/build shared nostrc):

```sh
: "${NOSTRC_ROOT:?Set to the approved dedicated nostrc source tree}"
NOSTRC_BUILD_DIR="${NOSTRC_BUILD_DIR:-$NOSTRC_ROOT/_build}"
meson setup build \
  -Dnostrc_root="$NOSTRC_ROOT" \
  -Dnostrc_build_dir="$NOSTRC_BUILD_DIR" \
  -Dsigner_nip46=true -Dsigner_nip5f=true -Dtools=true
python3 tools/e2e/test_matrix.py -v
python3 tools/e2e/matrix.py probe --build build --tests --capture
```

Receipts default to the **git-ignored `tools/e2e/output/` directory**, never the
tracked docs tree. `probe` creates a timestamped `native-UTC/` subdirectory;
`--out DIR` overrides it. Ledger commands default to `output/pairings.json`;
an explicit file argument overrides that. Keep raw receipts untracked and fold
important measurements/outcomes into a dated summary before cleanup.

`probe` refuses to overwrite results; it runs the build/default tests, checks
fresh-home unauthenticated host refusal, and checks TCC without prompting. The
explicit `--capture` flag captures **one frame to `/dev/null`** if already permitted.
No screenshot is retained. The negative host check uses loopback relay port 9,
no signer, no capture and no input. Its expected exit 1 is a security PASS, **not
successful direct-IP streaming**. Test timeouts kill/reap the owned process group.
The current probe is Unix-only. The default suite leaves live subchecks gated;
Passing default test programs do not mean all live subchecks ran.

Windows: copy the worktree's `src`, `tests`, `tools`, `imgui_vendor`, `meson.build`
and `meson_options.txt` into a **new** `$env:USERPROFILE\md-l7` directory, plus a
`SOURCE_COMMIT` file containing the exact source revision. Use `COPYFILE_DISABLE=1`
when making a tar on macOS. Check the directory does not already exist. Do not
write to `$env:USERPROFILE\metadesk` or install global dependencies as a workaround.

```powershell
$Root = Join-Path $env:USERPROFILE 'md-l7'
$Out = Join-Path $Root 'tools\e2e\output'
powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\tools\e2e\windows-components.ps1" -Phase Prepare -Root $Root
```

The Windows driver gives each check a 120 s failure deadline and puts a gated
child in a private kill-on-close Windows job before allowing it to launch any
descendants. Timeouts/cancellation terminate that owned tree. `-Phase SelfTest`
checks exit-code propagation, a missing executable, a deliberately stalled child
and child removal. Console checks additionally require the runner process's
session ID to equal the active console session, not merely that one exists.

Prepare inventories prerequisites, attempts the actual full Meson configuration,
and builds/runs existing native harnesses in SSH session 0. Its nonzero exit is
expected when product configuration is blocked; inspect **each** JSON result,
not just the script's aggregate exit. `libyuv`/`libnostr` pkg-config absence is
inventory, not proof no alternative build-tree library exists.

For the console phase, create a wrapper `.cmd` in that same isolated directory:

```bat
@echo off
set "ROOT=%USERPROFILE%\md-l7"
set "OUT=%ROOT%\tools\e2e\output"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\e2e\windows-components.ps1" -Phase Console -Root "%ROOT%" > "%OUT%\console-runner.log" 2>&1
set result=%ERRORLEVEL%
echo %result% > "%OUT%\console.exit"
echo done > "%OUT%\console.done"
exit /b %result%
```

Then from SSH run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\tools\win\run-interactive.ps1" -Command "$Root\console.cmd" -DoneFile "$Out\console.done" -TimeoutSeconds 300
```

The runner's exit 0 means the marker arrived, **not tests passed**. Read
`console.exit` and `Console.json`. Exit 3 means no active console: NOT_RUN, never
substitute session-0 results. The console checks create owned fixture windows;
the DPI probe makes one real click in its own button. DXGI's own fixture handles
console-display power-off; do not lock the desktop or inject wake input elsewhere.
PowerShell 5.1 shell redirection can emit UTF-16 while Set-Content/native
forwarding uses other encodings; detect the BOM when collecting and normalize
local receipts to UTF-8 if needed. Do not commit raw receipts.

Linux: **obtain the VM owner's explicit availability confirmation first**. Only then copy
into `~/md-l7`, set `NOSTRC_ROOT` and `NOSTRC_BUILD_DIR` to the approved read-only
dedicated source/build trees, and use `-j2` on this 3 GB VM. Never modify `~/nostrc`,
`~/nostrc-master`, or `~/metadesk`. Enable only the current session's display,
XDG runtime and bus environment; never restart AT-SPI. Linux native input remains
NOT_RUN unless the portal RemoteDesktop path is available/qualified; **do not
substitute uinput**. A container's private Sway test is not live-GNOME input.

Omarchy: repeat the Linux build/role procedure on a disposable Arch/Hyprland
machine, recording Hyprland, portal backend and PipeWire versions. Check capture
chooser, restore/revocation and input backend explicitly. A Sway container is
not Omarchy. With no machine, record NOT_RUN; no VM creation is required.

## Pairing launch gate

**Authenticated direct IP (md-p4tl, merged):** provision the private signer,
host-role account and signed allowlist first; see [accounts](../accounts.md) and
[client CLI](../../src/client/README.md). Each process uses its endpoint's private
HOME/XDG config/state. A transport address alone is not an authenticated identity.

```sh
# Run host/client on their respective machines. Variables contain public IDs,
# private signer socket paths, and a controlled relay URL, never secret keys.
build/metadesk-host --bind "$HOST_IP" --port "$PORT" \
  --relay "$PRIVATE_RELAY" --socket-signer "$HOST_SIGNER" \
  --no-input --no-nvenc --fps 10 --capture-max-size 1280x720
build/metadesk-client --host "$HOST_IP" --npub "$HOST_NPUB" --port "$PORT" \
  --relay "$PRIVATE_RELAY" --socket-signer "$CLIENT_SIGNER" --timeout 10000
```

The client negotiates with the pinned npub, proves the one-use token on native
TCP and waits for ACK. Verify correlated request/session IDs, history rejection,
the actual IPv4/IPv6 endpoint and no TUN. Run an unlisted signer negative first;
never bypass authorization. Round 2 exercised IPv4 only, not IPv6.

FIPS commands below are the **planned attended procedure**, not commands exercised
by this run. First provision two real daemons using task-private identities,
controlled relay/static peer configuration, their documented control endpoints,
TUNs and routes. Reuse the configuration schema in the
[mesh reference](../../deploy/containers/reference/mesh-validation.md); that
Linux/container receipt is not a native macOS/Windows qualification. Check actual
`fipsctl show status`, `show peers`, `show sessions`, route and TCP delivery.
Do not replace the daemon with a socket fixture or an address alias. Windows
metadesk control is unsupported at this revision (**md-m1we**).

With a verified host role, controlled NIP-46 bunkers, allowlist and real mesh ready:

```sh
# Variables are provisioned privately; never echo the bunker URIs/secret keys.
set +x
build/metadesk-host --bind "$HOST_FIPS_ADDR" --port "$PORT" \
  --npub "$CLIENT_NPUB" --fips-control "$HOST_CONTROL" \
  --bunker "$HOST_BUNKER" --relay "$PRIVATE_RELAY" --no-nvenc \
  --capture-max-size 1280x720 --no-input
build/metadesk-client --npub "$HOST_NPUB" --port "$PORT" \
  --fips-control "$CLIENT_CONTROL" --bunker "$CLIENT_BUNKER" \
  --relay "$PRIVATE_RELAY"
```

Start with `--no-input` for capture/network qualification. Remove it only for the
owned-window HUMAN/AGENT checks. On live GNOME leave input disabled until the safe
portal path is demonstrated. GUI and `--no-display` are separate runs; displayed=0
in decode-only mode is not GUI success. Record x264 separately from hardware
encoding; then repeat with hardware selection if available.

## Native observation helpers (Round 2)

These helpers generate ignored receipts, not automatic E2E verdicts. Run their
non-desktop tests with `python3 tools/e2e/test_observe.py -v` and
`python3 tools/e2e/test_mcp_probe.py -v` in addition to `test_matrix.py`.

Private NIP-5F setup, on each endpoint, using the existing cryptographic fixture:

```sh
umask 077
# Short, resolved path avoids Unix-domain socket path-length limits.
R=$(python3 -c 'import tempfile,os; print(os.path.realpath(tempfile.mkdtemp(prefix="md-l7-")))')
mkdir "$R/self" "$R/home"
python3 deploy/containers/reference/mesh-peer.py --generate-key "$R/self.key"
python3 deploy/containers/reference/mesh-peer.py --identity "$R/self.key" > "$R/identity.json"
cc -O2 -Wall -o build/nip5f-crypto deploy/containers/reference/nip5f-crypto.c \
  -Isrc/core $(pkg-config --cflags --libs libcjson) -Lbuild -lmetadesk \
  -Wl,-rpath,"$PWD/build"
python3 -u -B deploy/containers/reference/client-fixture.py --tests tests \
--crypto "$PWD/build/nip5f-crypto" --relay "$RELAY_BIND" --sockets "$R" \
    --signer "self=$R/self.key" --no-store-ephemeral
```

Wait for `fixture ready`. One endpoint binds the controlled LAN relay; the other
may bind an unused loopback relay for its signer, while all product commands
explicitly select the first endpoint's `PRIVATE_RELAY`. **Omit `--control`: this
is only relay/signer plumbing, never evidence of a FIPS mesh.** Record the Python
PID and stop/reap it during cleanup. Repeat with a separate unlisted identity for
negative authorization checks. Keep keys, token files and raw trees untracked.

In a separate terminal set endpoint-specific public IDs and isolated account env:

```sh
export HOME="$R/home" XDG_CONFIG_HOME="$R/home/.config" XDG_STATE_HOME="$R/home/.local/state"
export NOSTR_SIGNER_SOCK="$R/self/signer.sock"
# SELF_HEX comes from identity.json; PEER_NPUB is the other endpoint's public ID.
build/metadesk-host account add nip5f "$SELF_HEX" 'L7 throwaway'
build/metadesk-host account bind-role host nip5f "$SELF_HEX"
build/metadesk-host access --relay "$PRIVATE_RELAY" add "$PEER_NPUB" video,agent
build/metadesk-host access --relay "$PRIVATE_RELAY" list
```

Use the current real session's `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS` and
`WAYLAND_DISPLAY` on GNOME; do not manufacture another bus. Set input capability
only where the owned-fixture safety preconditions are satisfied; capability
grants do not override `--no-input`. Account commands use no relay; all access
commands above do. Pass the same socket explicitly to native host/client launches.

Timestamp any native launch without exposing secret argv:

```sh
python3 tools/e2e/observe.py --name host-unique-run --pid-file "$R/host.pid" \
  --deadline 600 -- build/metadesk-host --bind "$HOST_IP" --port "$PORT" \
  --relay "$PRIVATE_RELAY" --socket-signer "$HOST_SIGNER" --no-input --no-nvenc
# Another terminal: signal the runner PID (not an arbitrary process-name match).
kill -TERM "$(cat "$R/host.pid")"
```

Choose a unique receipt name and an unused PID file for each invocation. The
runner rejects both collisions; it timestamps lines/spawn/exit, forwards stop to
its owned group and reaps descendants. Deadline/forced termination returns 124;
signal termination is normalized to 128+signal. These are failure/safety outcomes,
not capture readiness or successful teardown. A setup run without any client may
hit the host's initial admission timeout; label it setup, not a video sample.

Owned fixture windows:

```sh
clang tools/e2e/macos-fixture.m -fobjc-arc -Wall -Wextra -Werror \
  -framework Cocoa -o build/macos-fixture
build/macos-fixture
# GNOME: only this owned process loads the accessibility bridge.
GTK_MODULES=atk-bridge python3 tools/e2e/gnome-fixture.py
# Synthetic owned solid frame, native SDL/Metal readback; no desktop pixels.
build/render_probe
```

`render_probe` builds with client support and `-Dtools=true`; resize/readback
checks and pure `test_video_geometry` do not establish live remote input mapping.
The animated fixture timer only generates damage; it is not relay polling.

For a separately configured bearer MCP endpoint, run `mcp-target.py --url URL
--token FILE --window EXACT_OWNED_TITLE --button EXACT_OWNED_LABEL`. Its default
checks unauthorized 401, initialize/initialized/tools, a unique owned tree/button,
and input-disabled rejection. This is supplemental MCP HTTP evidence, **not** the
binary stream's MCP/session integration. It rejects redirects and optimized Python
does not disable safety checks. **md-ixkl:** an action may hit an occluding window;
`--click` is only for the macOS fixture's `--occlusion-test` (both receivers owned),
never an operator window. Stop ordinary input tests on target mismatch.

GNOME consent: use an attended chooser, stop and ask for a human click when needed.
Do not repeatedly ask while a response is pending. Record the private token file's
mode/length, never its contents. Restore is not guaranteed: locked/inhibited
sessions may fall back to a chooser or reject creation. Read-only session/journal
checks are appropriate; never unlock, replace services or restart the bus to turn
a failure green. A token file is not proof of unattended first-frame restoration.

### Windows native attended helper

Follow [windows-build.md](windows-build.md) in the assigned Windows directory.
For real NIP-46 sessions, additionally build nostrc targets `nostr_nip46_core`
and `nip04`, and configure metadesk with `-Dsigner_nip46=true`. Reuse installed
MSYS2 packages, not a sibling lane's source/build. Include the selected nostrc
`build/libjson`, product build directory and native CLANGARM64 bin directory in
PATH. Use a private bunker and the same pinned host identity/direct-IP recipe.

Compile in the native MSYS2 CLANGARM64 shell:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror tools/e2e/windows-fixture.cpp \
  -o windows-fixture.exe -luser32 -lpowrprof
```

In a **separate task-owned interactive console**, launch
`windows-fixture.exe PRIVATE_COMMAND_FILE OWNED_STOP_FILE [OWNED_DONE_FILE]`.
Use a fresh optional done path for automated runs: after actual child exit and
job cleanup the helper atomically publishes JSON containing `native_exit`,
`helper_result` and `deadline`. All must indicate success; CMD's own trailing
commands can be skipped by Ctrl+C, so do not depend on a CMD marker alone.
The command file is
a private UTF-8 CreateProcess command line, possibly containing a throwaway
bunker URI: restrict its ACL and never print/commit it. Its child is the real
host/client, not a video fixture. Run the enclosing `.cmd` with
`tools/win/run-interactive.ps1 -TimeoutSeconds 43200`, recording helper exit and
a done marker only after completion. A scheduled-task marker alone is not a
product exit receipt. Stop by creating only the assigned stop file; the helper
requests Ctrl+C in its private console and waits for the product, then reaps its
kill-on-close job. Never run it in an operator's existing terminal.

The visible **md-l7 Owned Windows Target** has an animated label, owned button
and text entry. Power notifications re-wake only that owned window; no input is
injected to wake the display. Do not lock the console or use session 0 as a GUI
receipt. Keep the viewer open while awaiting human confirmation; the long helper
limit is only a leak guard, never a success/capture/consent deadline.

`tools/e2e/windows-progress.py` is a separate no-display/no-capture/input-disabled
regression. Supply `--binary PATH --role client|host --relay PRIVATE_URL
--host-npub HOST_NPUB --unused-signer-pubkey UNUSED_TRANSPORT_HEX`. The unused
valid key must have **no bunker listening** on that controlled private relay.
After an actual NIP-46 startup diagnostic, it uses `PeekNamedPipe` to prove the
already-printed progress is readable while the child still awaits the signer,
then terminates/reaps the test process. `--expect-buffered` reproduces the old
Windows baseline. This is not an authenticated pairing or graceful-exit test.

### Windows progress observer

Use a fresh run prefix inside an **untracked, task-owned output directory**.
Start this alongside the interactive fixture, with the same stop/done paths:

```powershell
powershell -NoProfile -File tools/e2e/windows-observe.ps1 `
  -Log "$OutputDir\client.log" -StopFile "$OutputDir\client.stop" `
  -DoneFile "$OutputDir\client.done" -TargetFrames 6000 -TimeoutSeconds 1200
powershell -NoProfile -File tools/e2e/test_windows_observe.ps1
```

The observer writes `client.log.observed.jsonl`, waits for real decoded/displayed
counters, creates only its owned stop marker, drains the final log after native
completion, and rejects losses, stale receipts, nonzero exits and deadlines.
It watches changed/created/renamed files; unread-file tail checks are local file
I/O, not relay polling or success sleeps. Check its final `pass` record as well
as the native completion receipt. Five synthetic cases test the observer, not
product acceptance. For human checks keep the viewer running until the answer;
do not use the automated frame goal to close it prematurely.

For private NIP-46/long runs, the existing relay/signer fixture must use
`--no-store-ephemeral`: NIP-46 ephemeral RPCs must not consume retained history.
This retains session/grant history and the unchanged stored-event cap; it does
not emulate FIPS or permit public-relay test publication. Stop and diagnose a
saturated fixture; do not increase its cap or call its failure an auth rejection.

## Per-cell attended checks and recording

For every pairing, record the following **ten checks separately** in order:

1. **connect_auth**: intended signer identities, verified allowlist/grants,
   correlated live NIP-44/NIP-17 session request/accept and one-use binding;
   unlisted client and replay rejected before frames/actions.
2. **video**: changing owned test pattern, dimensions/aspect and resize/max-size,
   fills intended viewport without trails or stale pixels. Retain only fixture
   screenshots, not operator desktop pixels.
3. **decode_performance**: actual codec/encoder/decoder, decoded/displayed counters,
   elapsed observation interval, FPS and bitrate from measured frames/bytes.
   Do not report target FPS/bitrate as measurement.
4. **overlay**: fixture a11y overlay tracks the correct window on move/resize,
   scaling and scroll, including mixed scale if hardware permits.
5. **human_input**: owned receiver records motion, left/right/middle down/up,
   horizontal/vertical scroll, text and modifier press/release; compare physical
   and logical positions at center/edges for 1:1 mapping. Stop on lost focus.
6. **agent**: real MCP initialize → initialized → tools/list → a11y tree →
   action-by-id; verify target-only accessible state change, missing/unready and
   input-disabled negatives. A readable tree alone is insufficient.
7. **reconnect_restore**: disconnect only owned client/transport, observe loss,
   reconnect and reissue subscriptions, ensure dedup/no replay, capture restoration
   and consent behavior. Explicitly record GNOME md-3nv, not merely token existence.
8. **teardown**: SIGTERM/normal window close, exit code/duration, no residual host,
   client, signer, relay, task, socket or route. A deadline expiry is FAIL.
9. **permissions**: granted plus denied/revoked behavior on isolated permissions:
   SCK TCC, Windows interactive DXGI, GNOME/Hyprland chooser and restore token.
10. **transport**: IP endpoint/family/no TUN, or real FIPS identities/control,
    route/peer/session, actual TCP frames and route-loss recovery. No extrapolation
    from a fixture, daemon status or socket existence.

Create a fresh ledger in ignored output; record observations with local evidence
references and preserve the important numbers in the dated tracked summary:

```sh
python3 tools/e2e/matrix.py init
python3 tools/e2e/matrix.py record \
  --host macos --client windows --transport ip --check connect_auth \
  --status BLOCKED --evidence 'Windows full product configuration missing FFmpeg' \
  --defect md-z9pd
python3 tools/e2e/matrix.py report
```

Allowed statuses: PASS, FAIL, NOT_RUN, BLOCKED. FAIL requires a defect bead;
all ten checks must PASS for overall PASS. Earlier observations remain in
per-check history. File real new defects with `--deps discovered-from:md-5zi`;
reuse a root-cause bead for multiple affected cells, not duplicate bugs.

## Cleanup receipt

Collect/sanitize receipts before removing owned directories. Stop/wait for each
owned PID/process group and signer/relay; confirm tasks unregistered; remove only
rules/routes/TUNs created by this run; delete throwaway credentials/tokens/files.
Check no owned executable is running before deleting `md-l7`. Never kill another
lane's processes, prune unrelated branches, remove existing VMs, clear unrelated
stashes, or alter global firewall/TCC policy. Preserve a concise results summary
in this worktree, not the raw output directory.
Leave md-5zi open, add bd notes, commit only task paths with `BD_EXPORT_AUTO=false`;
**do not stage `.beads/`, push or merge**.
