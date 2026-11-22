# Running the native host ↔ client matrix (md-5zi)

This is an attended acceptance procedure, not an unattended input bot. The
[owning matrix](e2e-matrix.md) includes all **32** host/client/transport combinations,
including same-platform pairs. [L7's first run](results/2026-11-21-l7.md) records
prerequisite blockers and component results, **not successful pairing runs**.

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
  and grant only the test client's required capabilities (`video,input,agent` for
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
42 passing test programs do not mean all live subchecks ran.

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

Linux: **ask the VM owner whether lane L3b is finished first**. Only then copy
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

**Direct IP is currently blocked by md-p4tl.** `metadesk-client HOST` connects
without Nostr negotiation/session proof, whereas the host requires negotiated
authorization. `--bunker` alone does not fix that branch. Do not publish a
fictional working direct-IP recipe: after md-p4tl lands, run its identity-pinned
negotiation/endpoint-selection recipe over native IPv4 and IPv6, verify no TUN
route, and record the exact command. The current refusal is reproduced by `probe`.

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
