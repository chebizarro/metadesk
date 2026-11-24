# RemoteDesktop / libei input testing

`portal_input=auto` builds the backend when libei ≥ 1.2 is installed. Runtime
`auto` prefers RemoteDesktop v2+ only in a declared Wayland session; explicit
`portal` and `uinput` overrides do not fall back. Managed portal input refuses
before session creation: restore tokens cannot guarantee that consent UI will
not appear. See the [RemoteDesktop persistence contract](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html).

## Automated coverage (no desktop devices)

Run `meson test -C build -j2 --print-errorlogs`. Backend-selection tests use
device-free factories, covering missing/empty/X11/Wayland/unknown session
types, portal discovery, initialization failure and native fallback, both
explicit overrides, NULL config, and managed-policy propagation. The private
D-Bus/libeis fixture covers actual wire events, bounded denial/timeout/partial
grant/owner loss/revocation, cleanup, ASCII preflight without partial injection,
mixed shifted text, keymaps with and without a NUL terminator, and managed
refusal with no portal-session calls (including when a token exists).
Physical Left Shift remapped to Control or Shift+Alt is rejected during
whole-string preflight, with no partial text or shortcut injected.

## Owned-window live acceptance

Use an unlocked test desktop and its actual session bus, runtime directory,
Xwayland display and authority. Never run the uinput variant on a shared
desktop. Use task-private token storage:

```sh
mkdir -m 700 -p "$HOME/portal-test-state"
export XDG_STATE_HOME="$HOME/portal-test-state"
MD_INPUT_DESKTOP_BACKEND=portal ./build/test_input_desktop
```

Have the operator enable Remote Interaction and remember permission in GNOME's
Remote Desktop dialog. The three-minute consent deadline is a human-operation
failure watchdog, not a readiness sleep. The test requires its own focused
window before keys, compares delivered `Metadesk 42!` text and ControlMask,
declares global logical bounds for multiple outputs, verifies pointer position
and WM-frame ancestry/client-local hit bounds before buttons/scroll, and checks
left/right/middle buttons plus vertical/horizontal scroll. It closes its input
session and window on completion. Do not touch the desktop during injection.

Repeat with the same state directory: it must complete with no second consent.
Then run with `MD_INPUT_DESKTOP_BACKEND=auto` to exercise the production factory
(the test requires Wayland and an available portal before this mode). For live
revocation, set `MD_INPUT_DESKTOP_REVOKE=1` and keep stdin open. Once the test
prints `REVOKE READY`, the operator stops that Remote Desktop session using
GNOME's sharing indicator, then sends a newline on stdin. The test checks
readiness is false and a new key press is rejected, then destroys the session.
The stop/newline is an explicit human barrier, not a timer-driven close.

Remove the task-private state directory after the session is closed. Do not
delete another application's tokens or alter AT-SPI. Native Xwayland target
delivery on GNOME is evidence for portal input, not Flatpak or paired capture
acceptance. This input-only test does not exercise the paired host-video path.

## Native GNOME acceptance, 2026-11-22 (md-y8kc)

Ubuntu 24.04 aarch64, GNOME/Mutter 46 Wayland, RemoteDesktop v2 and libei 1.2.1;
owned Xwayland target, two logical outputs (2080×768 global bounds). The operator
approved Remote Interaction and remember once. No uinput devices were created.

| Check | Result |
|---|---|
| First key, delivered `Metadesk 42!`, Ctrl+A ControlMask | PASS |
| Absolute pointer, left/right/middle buttons, vertical/horizontal scroll | PASS |
| Restore-token reuse without another consent; explicit portal and auto | PASS |
| Ordinary session/window teardown | PASS |
| External compositor-side stop, then not-ready and rejected key press | PASS |
| Managed portal refuses without session calls, including saved token | PASS (private fixture) |
| Paired capture/input consent | NOT EXERCISED by md-y8kc |
| Flatpak / native Wayland target / literal Shell indicator click | NOT QUALIFIED |

The stop was **compositor-initiated stop (same path as the Shell indicator),
not a literal UI click**, triggered through the session-owning backend. Direct
Mutter `Stop` from an unrelated D-Bus sender returned AccessDenied. We therefore
closed only the backend session belonging to test PID 466343 / sender
`:1.25917` (`.../session/1_25917/md_input_session`). A narrow bus monitor captured
the owning GNOME portal backend `:1.25276` issuing
`org.gnome.Mutter.RemoteDesktop.Session.Stop` on
`/org/gnome/Mutter/RemoteDesktop/Session/u10`, then Mutter's `Closed` signal.
The test logged `Disconnected by EIS`, `portal revocation fails closed=PASS`,
destroyed its session/window and exited 0. This verifies the EIS-disconnect
revocation path; the private fixture separately covers portal `Session.Closed`.

The remembered PermissionStore `remote-desktop` entry was matched against the
task-private token before deleting that entry and local token. No other token
or permission was deleted. AT-SPI address and GUID were unchanged.

Full suites at `-j2`: macOS 55 pass / 0 fail / 1 opt-in skip; Linux 53 pass /
0 fail / 1 opt-in skip. Device-free selection: 905 checks; private D-Bus/EIS:
225 checks. The final review-driven physical-modifier hardening was validated
by that fixture and both full suites after live consent had been revoked; the
live matrix above predates that last preflight-only change. These counts do
not turn native acceptance into Flatpak evidence.

## Attended paired video qualification (md-kmoa; not run)

A Linux video host requesting portal input uses one RemoteDesktop session. It
selects one monitor and requires both input devices plus a unique EIS region
matching the stream's `mapping_id`. Capture-only and input-only hosts do not
claim paired consent. The private fixture exercises the portal and EIS wire
flow; its PipeWire socket is a stand-in, not a video producer.

1. An operator must unlock the GNOME Wayland desktop and approve this test.
   Record GNOME/Mutter, portal, libei and PipeWire versions. Use a disposable,
   unlocked session or an owner-approved test desktop; open a test window on a
   known monitor, ideally a scaled monitor with nonzero desktop origin.
2. Prepare the private signer, signed allowlist, controlled relay, and native
   direct TCP client as in [Pairing launch gate](e2e-runbook.md#pairing-launch-gate).
   On the host use a fresh owned `XDG_STATE_HOME`, omit `--no-input` and
   `--managed`, and add `--input-backend portal`. Start host and client, select
   the intended monitor, and approve remote interaction plus Remember. Record
   exactly one chooser/approval and one RemoteDesktop Start. If collecting a
   bus trace, retain only method names, handle paths and result codes, never
   token values. Verify CreateSession → SelectDevices → SelectSources → Start
   → OpenPipeWireRemote → ConnectToEIS; all handle-bearing calls must use the
   CreateSession handle, and there must be no ScreenCast Start.
3. Verify real video from the selected monitor. Move/click near the corners
   of the owned window through the client; confirm the desktop offset and
   scale are correct and a second output is never targeted. Check a key,
   a button and a scroll event on the owned target. Repeat a host/client run
   with the same private state; record whether it restores with no prompt.
   Verify only the RemoteDesktop token file is used; do not assume a token
   value must change on this compositor.
4. During an active run, stop sharing using the GNOME indicator. Verify video
   stops, the host rejects new input, and there is no replacement chooser or
   fallback injection. Separately verify managed startup with the saved state
   reports `needs_interaction` without a chooser or Start. Stop the test
   processes; remove only test-private state and its matching portal permission
   if one was remembered. Do not restart desktop services. Flatpak qualification
   is separate.

## GNOME lock gate qualification (md-xhkx; owner-attended, not run)

Both paired RemoteDesktop and legacy ScreenCast check the local session's
GNOME ScreenSaver state before portal setup and again before `Start`. An
interactive host waits for a local unlock notification within the existing
consent budget. An unattended capture or standalone portal input fails fast.
A new lock during setup fails fast in either capture mode. Managed capture
still returns `needs_interaction` without contacting the portal, even with saved
consent: the portal cannot guarantee prompt-free operation. `desktop_locked`
is a capture startup rejection, not a request for the application to unlock.

On an owner-approved disposable GNOME desktop (including Ubuntu 24.04 with
portal-gnome 46.2), seed separate remembered grants for paired capture and
legacy `--no-input` capture, using private state. Capture method names and
ordering only, never tokens. Let the operator lock locally. For each path,
verify interactive startup sends no `Start` while locked, then locally unlock
and confirm startup reuses the grant and delivers real video. Separately verify
unattended startup reports `desktop_locked` and sends no `Start`, the grant
survives, and a fresh attempt after unlock restores it. Check managed startup
reports `needs_interaction` without a chooser or `Start` while locked and
unlocked. Clean up only the test grants and state.

On a disposable backend with the upstream fixes, also cancel a pending legacy
chooser and trace the `Request.Close` reply before `Session.Close`. Do not try
that cancellation against 46.2 on a shared desktop: close ordering does not
repair its upstream use-after-free. Lock state and `Start` are on separate
services; the final check cannot prevent a new lock after the last observation.
