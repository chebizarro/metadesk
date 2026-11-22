# Host process lifecycle (md-pw1q / W17)

## Decision

The supported desktop owner is a **per-user graphical-session agent**. Installing
Metadesk does not silently start hosting. Provision/bind a host account and its
allowlist first, authorize the responsible capture/accessibility identity
interactively, then opt into autostart. Closing the manager does not stop the
host. Running the CLI interactively remains supported; do not run a second host
against the same endpoint.

**Unattended after logout, boot/pre-login access, and login-screen control are
not supported.** No root/system service, launch daemon, password-storing task,
Windows SYSTEM principal, or `loginctl enable-linger` is installed. An allowlisted
remote connection while the user is logged in is not after-logout hosting.
Headless/OCI agents have a different capability/lifecycle contract.

| Transition | Contract |
|---|---|
| Login | Opted-in user agent starts with that user's credentials and policy. Linux needs a real graphical-session.target; macOS needs Aqua; Windows uses InteractiveToken. |
| Lock | No unlock, secure-desktop capture or consent bypass. The process can remain alive; capture may stall/fail according to its backend. No promise of streaming while locked. |
| Logout | Session agent is stopped or torn down by the OS. No desktop work continues under another user's identity. Windows logoff/shutdown can impose a hard OS termination deadline. |
| Reboot | No pre-login host. Opted-in agent starts at the next graphical login. |
| Crash | Supervisor retries with rate limits; managed Unix IPC recovers its checked stale socket. Authentication/grants are rebuilt, not replayed from a previous process. |

This deliberately does **not** claim lock/logout/reboot qualification from the
start/stop tests below. Secure-screen or service-plus-per-session-agent support
would be a separate security design.

## `--managed`

Use a previously bound host account. No account creation, automatic signer
selection, fresh bunker pairing, MCP transport, stdin question, or consent UI is
initiated. External signers must already authorize this host; the host cannot
suppress an external application's own UI. OS credential restoration already
uses no-unlock/no-authentication-UI lookups and never falls back to plaintext.
Admission remains allowlist-only unless explicitly `--attended`; this flag does
not grant capabilities, enable input, or weaken relay/stream authorization.

* **Linux capture limit:** ScreenCast restore tokens do not guarantee that Start
  will avoid a consent dialog. Managed capture therefore refuses **before any
  portal Start**, rather than racing/dismissing UI or bypassing the portal. Use
  an interactive host for video today. `--managed --no-capture --no-input` is the
  lifecycle/agent-only diagnostic path, not video qualification. The default
  unit can wait for authenticated requests, but an attempted video setup exits
  78 and needs an operator. `--capture-unattended` only caps a consent budget;
  it is not a no-prompt API and does not override this restriction.
* **Linux input limit:** RemoteDesktop likewise has no no-prompt Start flag.
  An invalid or revoked restore token can display consent, so managed portal
  input refuses before creating a session, even with a saved token. Native
  input backends retain their existing behavior; uinput refuses known Wayland
  sessions before device creation. Unavailable requested input makes a managed
  host fail with exit 78 / `needs_interaction`, rather than silently serve
  without control. Use `--no-input` explicitly for a read-only host.
* **macOS:** managed SCK preflights Screen Recording permission before enumeration.
  Missing authorization refuses without requesting it. AX already preflights
  trust. Permission for a Terminal-launched binary is not permission for a
  bundled host. The independent signed helper/TCC identity and actual
  SMAppService registration UI remain **md-wdi3**; this lane provides the agent
  layout, not that qualification. OS-mandated privacy reminders are OS policy.
* **Windows:** use the logged-in user's desktop, never session 0. The existing
  authenticated management IPC is still unsupported on Windows; no insecure
  substitute pipe has been enabled. Attended management options still refuse.

### Status and exit policy

The authenticated Unix `management.snapshot` / `management.subscribe` responses
(and request/session lists) add:

```json
{"host":{"managed":true,"pid":1234,"mode":"user-session","state":"waiting"}}
```

States are `starting`, `waiting` (signaling ready, **not capture ready**),
`serving` (stream listener established), `needs_interaction`, and `stopping`.
Lifecycle changes advance the same revision and notify existing subscriptions.
Final states are best-effort before EOF, not a delivery acknowledgement. Before
an account identity/endpoint exists, startup failures are reported in supervisor
logs, not through an unauthenticated fallback IPC. Endpoint absence is **host not
running/unavailable**, not permission to approve; the manager should offer the
platform-specific start command, not repeatedly probe/restart (W16 owns UI).

| Exit | Meaning | Supervisor policy |
|---|---|---|
| 0 | SIGTERM/SIGINT, Windows managed stop, or intentional normal exit | Do not restart |
| 78 | Missing/invalid account, identity/credential intervention, interactive-only option, managed capture needs consent | Do not restart until operator correction |
| 1 | Runtime/setup failure, signer connection/timeout, busy metadata or temporarily unavailable credential backend | Restart with bounded policy |
| Signal/crash | Unexpected termination | Restart with bounded policy |

Signals only set a signal-safe flag/event; cleanup remains in normal host code:
stop capture/threads and stream resources, stop relay producers, join management
workers, remove the owned endpoint, then release signers/network runtime.
Existing blocking startup operations retain their bounded transport deadlines;
a supervisor's 30-second stop timeout is a failure watchdog, not proof that all
possible blocked backends terminate gracefully. Lifecycle tests cover idle
signaling stop and crash recovery, not every backend stop.

Managed logs are unbuffered, newline-delimited; core/lifecycle records are JSON
with level/component/message, including `event=host_lifecycle` and the original
exit code. Existing host progress lines remain plain text, without terminal
control codes. Linux uses journald; macOS also sends core records to unified
logging (`os_log`). Windows keeps stdout/stderr logs and Task Scheduler records
lifecycle in its Operational event log; no machine-wide Event Log source or
administrator requirement is introduced. Logs are diagnostics, not credentials.

Managed Unix endpoints additionally hold a mode-0600, owner-only, no-symlink,
single-link `.lock` file under the existing checked private parent. A lifetime
nonblocking flock serializes managed starters. Only an owner-only socket whose
connect fails **ECONNREFUSED**, with unchanged inode/device, can be recovered.
Live sockets, regular files, symlinks, unsafe parents and foreign ownership are
never removed. Interactive/default `md_management_start` retains its original
no-recovery behavior. Empty lock files persist to preserve lock-inode identity;
the runtime/private test directory owns their cleanup.

## Linux native packages

`packaging/linux/systemd/metadesk-host.service` is installed to
`/usr/lib/systemd/user` by `produce.py`; the deb copies that staged tree, RPM
lists the unit explicitly, and AUR repackages the same `tree/usr`. None enables
it in a package install script. No root system service is shipped.

```sh
# After provisioning; review the managed Linux capture limitation above.
systemctl --user daemon-reload
systemctl --user enable --now metadesk-host.service
systemctl --user stop metadesk-host.service
systemctl --user disable metadesk-host.service
journalctl --user -u metadesk-host.service
```

The unit has Requisite/After/PartOf `graphical-session.target`: starting it outside
the graphical session fails; it does not manufacture a fake session or linger.
The desktop/compositor must import DISPLAY/WAYLAND_DISPLAY and the session bus
into the user manager. Minimal compositors without that target integration need
their own correct session setup, not a system service workaround. Restart is
on-failure, 5 seconds, three starts per minute; exit 78 is excluded. Stop uses
SIGTERM with a 30-second failure watchdog, owner-only umask and no-new-privileges.

**W14 Flatpak handoff:** do not install/control a host systemd unit from inside
the sandbox. Request `org.freedesktop.portal.Background.RequestBackground`
interactively, handle Response/cancellation/denial and user revocation, and use
the sandbox's app-ID/command for autostart. Background permission is not
ScreenCast/RemoteDesktop/input permission and does not supply a GUI session
after logout. Managed prompt-free portal video still needs a supported
no-interaction contract. No `packaging/appimage` or `packaging/flatpak` edits here.

## macOS bundle

The bundler installs `Contents/Library/LaunchAgents/dev.metadesk.host.plist`.
Its BundleProgram is the sealed resource
`Contents/Resources/metadesk-host-agent`, which launches the host in
`Contents/MacOS`. A shell resource avoids adding another unsigned Mach-O helper;
it is **not** a replacement for md-wdi3's stable host TCC identity. It forwards
TERM/INT, reaps the child, and maps original exit 78 to supervisor success because
launchd's `KeepAlive.SuccessfulExit=false` cannot exclude a particular nonzero
code. The original host code stays in structured logs. Crashes/runtime failures
remain nonzero. The agent is Aqua-only with a 10-second throttle and 30-second
exit watchdog.

The packaged plist is ready for `SMAppService.agent(plistName:)` from the
responsible installed app (macOS 13+). Shipping it **does not register it**.
md-wdi3 owns registration/approval UX, Developer ID, updates and helper identity.
Do not copy a plist containing BundleProgram into `~/Library/LaunchAgents` and
expect standalone launchctl to resolve the bundle. For diagnostic bootstrap,
rewrite BundleProgram to an absolute Program and use a **private test label**;
`tests/qualify_managed_supervisor.py HOST CRYPTO APP` does this and unregisters.
The release floor/signing qualification is separate from a local ad-hoc bundle.

## Windows MSI / logon task

A per-user logon task is chosen over a Windows service: interactive capture
belongs to the user desktop; a session-0 service would need another privileged
broker and per-session agent. No such broker is introduced.

The optional WiX `HostLogon` feature (`ADDLOCAL=Main,HostLogon`, level 2) installs
`host-task.ps1` and **Enable Metadesk Host at logon**. Enabling is a separate
explicit user action after provisioning, not a silent MSI custom action. It
registers `Metadesk Host <user SID>` with InteractiveToken/Limited, no password,
no elevation, one instance, no runtime limit, and up to three failure restarts at
one-minute intervals. Exit 78 is logged and mapped to task success by the Run
wrapper. Diagnostics are in `%LOCALAPPDATA%\Metadesk\Logs\host.*.log` (replaced
at each run). The task starts at next logon; start it explicitly to run now.

```powershell
.\host-task.ps1 -Action Install
Start-ScheduledTask -TaskName ('Metadesk Host ' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value)
.\host-task.ps1 -Action Stop
.\host-task.ps1 -Action Uninstall
```

Run stop/uninstall **in the same interactive session**. `--managed-stop` signals
a protected owner-only, session-local named event; it does not sign anyone out,
lock, disconnect, or kill a process. The script checks task ownership by SID,
waits for matching same-session child handles, and refuses forced termination.
`Stop-ScheduledTask`/Task Scheduler End are not clean host stop APIs. The MSI
removes the task before removing its files when the optional component is
uninstalled; an unavailable same-session stop fails uninstall rather than
silently killing another session's host. Windows management snapshot/subscription
remains unavailable pending the owner-DACL/client-SID pipe work from md-trex.

## Source references

* [systemd graphical-session.target](https://www.freedesktop.org/software/systemd/man/255/systemd.special.html)
* [Apple bundled helper layout](https://developer.apple.com/documentation/servicemanagement/updating-helper-executables-from-earlier-versions-of-macos)
  and [SMAppService](https://developer.apple.com/documentation/servicemanagement/smappservice)
* [Task settings/restart/hard termination](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/new-scheduledtasksettingsset)
* [Background portal interface](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.Background.xml)

## W17 evidence — 2026-11-22

Worktree `md-w17-host-lifecycle`, branch `md/w17-host-lifecycle`, base
`e531fa6df989f0256d9371a39b334b19b73f08c5`. All builds/edits stayed out of
main. Private fixture keys/relay only; no public relay, real capture, uinput,
AT-SPI desktop operation, lock, disconnect or reboot was used in live lifecycle
checks.

| Gate | Evidence / boundary |
|---|---|
| macOS 27.0.1 / arm64 full configured Meson suite | **62 passed, 0 failed, 1 skipped** (63 total). Skip: opt-in AX owned clicks/native video. |
| Managed process tests | Config 78/runtime 1; SIGTERM/SIGINT 0; real private-relay host status; socket cleanup, distinct generations; SIGKILL then recovery of the checked stale socket. |
| IPC/security tests | Owner credentials, live endpoint refusal, regular-file/symlink refusal, stale recovery and exclusive managed lock. Host-binding test preserves interaction-required vs generic capture errors. |
| macOS LaunchAgent live | Bundled host + shipped wrapper/plist, private test label: bootstrap/start, IPC, SIGTERM, actual process-exit event, bootout, bootstrap restart, SIGKILL auto-restart with new generation/stale recovery, final bootout/unregister. |
| macOS bundle | Bundler succeeded; `codesign --verify --strict --deep` passed. **Ad-hoc/local**, 29 private libraries, effective minimum **27.2**, using installed development dependencies. Not the macOS-13 release profile, Developer ID/notarization or helper/TCC qualification. |
| Linux VM 192.168.64.3 | Own `/home/bizarro/md-w17`, read-only `/home/bizarro/nostrc-master/_build` dependency. Native build passed. Two focused managed/IPC tests passed. Own `md-w17-host.service`: start/restart/SIGKILL-auto-restart/stop passed; generation changed; final `Result=success ExecMainStatus=0 ActiveState=inactive`. Graphical target remained the existing session; no input/a11y test was run. |
| Windows VM 192.168.64.4 | Own `C:\Users\bizarro\md-w17`. MSYS2 CLANGARM64 compiled the production lifecycle module with a standalone native process fixture. InteractiveToken tasks installed/started; readiness was a kernel event, stop a same-session user event. Child and wrapper exited 0; direct config 78/runtime 1; script uninstall and zero-task/process checks passed. **Not a full product build, capture/session test, native MSI build/install/ICE test, or Windows management IPC qualification.** |
| Packaging/static | macOS parser/layout 5 passed; Windows authoring 2 passed; Linux safety 9 passed/1 platform skip on Mac. Shell syntax, shellcheck and `git diff --check` passed. Existing W9 deb/rpm/AUR staging of the unit verified in source; native distro packages were not rebuilt here. |

The scoped Oracle review initially received unrelated tab context; the actual
review used explicit lifecycle/IPC/packaging source excerpts, with no claimed
independent test execution. Applied findings: distinguish prompt-required capture
failure from restartable runtime failure, and hold Windows close/logoff callbacks
until normal cleanup signals completion (subject to OS deadline). Its MSI quoting
finding did not apply to the actual XML: both executable and script path are
already quoted. No second source review was performed.

Diff-scoped Nostr Protocol Review (A–J): **no new verified protocol findings**.
The existing REQ/EOSE/OK/CLOSED/AUTH, filters, correlation, grants and replay fence
are unchanged. New tests reuse the real private relay/signers, and readiness is
host output, IPC generations, filesystem/process notifications or kernel events;
watchdogs are failure-only. Supervisor restart backoff is process recovery, not
relay polling. This is not a repository-wide Nostr audit. Jev triage located plan
and startup ranges (17 requests, 137641 input tokens, about $0.0058); startup
ranges were widened where individual hits did not show resource ownership.

### Remaining acceptance boundaries

* W16 owns the manager's host-not-running/start affordance; no manager code was
  changed. Start commands and unavailable-status semantics are documented here.
* md-wdi3 owns actual SMAppService approval/registration UX, stable separate
  signed helper/TCC identity, release floor/signing and permission qualification.
* Windows authenticated management IPC remains md-trex's explicit unsupported
  boundary. Full Windows product/MSI and hostile-user IPC qualification remain
  required; authoring/native lifecycle fixtures are not substitutes.
* Linux managed no-prompt video is intentionally unsupported until a portal
  contract can guarantee no consent UI. W14 receives the Background-portal
  handoff above; its package directories were not edited.
* Lock/logout/reboot behavior is a documented support boundary, not live-tested
  transition evidence. No disruptive console operation was performed.

Cleanup: all temporary test units/agents/tasks and sockets were removed; both VM
source/build directories and all `/private/tmp/md-w17-*` outputs were deleted.
The assigned worktree retains only source/documentation/test artifacts. No shared
VM dependency tree, sibling worktree, user credential store or console state was
modified. Bead remains open; no push or merge is authorized in this lane.
