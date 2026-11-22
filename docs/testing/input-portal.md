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
acceptance. Capture and input currently use separate portal sessions; shared
consent, lifecycle and `mapping_id` work is tracked by md-kmoa.

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
| Paired capture/input consent | NOT IMPLEMENTED; md-kmoa |
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
