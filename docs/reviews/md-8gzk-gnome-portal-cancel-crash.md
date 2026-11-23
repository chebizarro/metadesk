# md-8gzk — xdg-desktop-portal-gnome 46.2 crashes when a pending ScreenCast chooser is cancelled

**Bead:** md-8gzk (stays open; upstream filing is the owner's call) · **Mitigation bead:** md-xhkx
**Date:** 2026-11-23 · **Reviewed metadesk:** `cb7b1cd` (task branch base) and master `8be1c74` (after md-kmoa)
**Environment:** Ubuntu 24.04 arm64 VM, xdg-desktop-portal 1.18.4-1ubuntu2.24.04.3,
xdg-desktop-portal-gnome 46.2-0ubuntu1, gnome-shell/mutter 46.x (mutter 46.2 reported by the shell), GTK 4.14.5, GLib 2.80.0

## Context / scope

During L7 (md-5zi), metadesk asked the ScreenCast portal to restore a remembered session
while the GNOME desktop was locked. The backend logged `Error restoring stream from session:
… Session creation inhibited`. metadesk's 180 s consent budget then expired and it cancelled the
request, and about 2 s later `xdg-desktop-portal-gnome` exited with SIGSEGV. This review:

1. reproduces the crash in an isolated headless GNOME session and finds the faulting code;
2. checks upstream GNOME GitLab, Launchpad and release history for a fix;
3. provides a ready-to-file upstream report (not filed);
4. reviews how metadesk times out, cancels and closes, and specifies a metadesk-side mitigation (md-xhkx)
   without editing `capture_pipewire.c` or `input_portal.c`.

## Summary

- **Reproduced: yes, deterministically.** Every ScreenCast run on stock 46.2 that left a chooser pending
  and then cancelled crashed the backend: 10/10 runs under gdb, plus two more that apport caught.
  The restore token and the lock/inhibit are not required. They only guarantee the chooser is pending
  and unattended, because a failed restore falls back to showing the chooser behind the lock screen.
- **Root cause:** two lifetime bugs in `src/screencast.c` around `ScreenCastDialogHandle` (46.2 line numbers):
  - **Bug A (double free).** `on_request_handle_close_cb()` (L210–217) frees the dialog handle but leaves
    `screen_cast_session->dialog_handle` dangling. The later session close (`Session.Close` or the
    frontend's peer-disconnect cleanup) frees it again in `screen_cast_session_close()` → `g_clear_pointer(&…->dialog_handle,
    screen_cast_dialog_handle_close)` (L916). Fixed upstream by **9a4dffa** (MR !164, 2024-05-27), first
    released in **47.0**.
  - **Bug B (use-after-free).** `screen_cast_session_close()` frees the handle but leaves its `handle-close`
    handler connected to the still-exported `Request`. A later `Request.Close`, sent explicitly or by the
    frontend on disconnect, runs `on_request_handle_close_cb()` on freed memory: `cancel_start_session()`
    L203 / L214–216. Fixed upstream by **056029c** (MR !231, 2025-09-18), first released in **50.0**.
    It was cherry-picked to `gnome-49` as 1d765f1, but no 49.x release after 49.0 includes it.
- **Ordering decides which bug fires, and no ordering is safe on 46.2.** The 1.18 frontend forwards
  `Request.Close` and `Session.Close` to the backend with synchronous calls from GDBus worker threads, so
  metadesk's back-to-back no-reply burst can reach the backend in either order.
- **Ubuntu noble still ships 46.2-0ubuntu1 with neither fix.** The gnome-46 branch has 9a4dffa as 7c44035, but no 46.3 was
  tagged. The matching Ubuntu report is **LP #2103434** (status *New*, no noble task). The matching upstream
  report is **GNOME/xdg-desktop-portal-gnome#60** ("Crashes after closing OBS", 2022, still open,
  labelled *Needs Information*). Our reproducer is the missing information.
- **The same build with both fixes applied survived every ordering** (A/B table below).
- **metadesk side.** On master after md-kmoa, the default interactive Wayland host uses
  `RemoteDesktop.Start`, and 46.2's RemoteDesktop path did not crash in 4/4 orderings. The legacy
  `ScreenCast.Start` path (`--no-input`, `--managed`, `--input-backend uinput|wayland`, X11, builds without
  portal input) still triggers the crash. The worthwhile mitigation (**md-xhkx**) is to *not issue a portal
  Start while the GNOME session is locked*. That avoids the guaranteed 180 s stall → cancel → crash, and it
  keeps the remembered consent, which the frontend consumes on lookup and only re-stores on a successful
  session. A secondary fix for the legacy path is to wait for the `Request.Close` reply before `Session.Close`.

## Reproduction (isolated session)

All work happened under `~/md-8gzk` on the VM. Nothing was installed or restarted, and the shared desktop
session was never touched (see the cleanup receipt).

**Private stack.** `dbus-run-session --config-file=<private session.conf>` with `HOME`, `XDG_RUNTIME_DIR`
(and therefore dconf, the permission store, the keyring and the Wayland socket) all under `~/md-8gzk`. Inside it:
`gnome-shell --headless --no-x11 --virtual-monitor 1280x720 --wayland-display md8gzk-wl`. The frontend,
permission store and other helpers were auto-activated *on the private bus only*. The private
`session.conf` replaces the standard service dirs with one directory where
`org.freedesktop.impl.portal.desktop.gnome` is activated through a wrapper running the backend under
`gdb -batch`. That removed a race in the first attempts, where an auto-activated backend competed with the
gdb-run one.

**Inhibit.** Headless gnome-shell deliberately skips remote-access inhibition on lock (`main.js`:
`if (remoteAccessController && !global.backend.is_headless())`). So the repro calls the same Mutter API the
shell calls on lock, through `org.gnome.Shell.Eval` in the private shell:
`global.backend.get_remote_access_controller().inhibit_remote_access()`. Eval was enabled by a tiny private
extension that sets `global.context.unsafe_mode = true`. This produced the identical backend message:
`Error restoring stream from session: GDBus.Error:org.freedesktop.DBus.Error.Failed: Session creation inhibited`.

**Restore data.** A restore entry was seeded in the *private* permission store, in the format 1.18.4/46.2 use:
`PermissionStore.Set("screencast", true, <uuid>, {"": ["yes"]}, <("GNOME", uint32 1, <(int64 now, int64 now,
[(uint32 0, uint32 1, <"MetaVendor:MetaVirtualMonitor:0x00">)])>)>)`.

**Client.** A Python/Gio client mirrors metadesk's `capture_pipewire.c` sequence: CreateSession
(`session_handle_token=metadesk_session`), SelectSources (`types=1`, `cursor_mode=2`, `persist_mode=2`,
`restore_token`), Start with parent `""`. After a shortened Start timeout (8 s instead of 180 s; the length
doesn't matter) it runs one of several cancellation sequences. `metadesk` mode reproduces
`capture_pipewire.c:228-235` + `:600-624`: `Request.Close` (no-reply), `Session.Close` (no-reply), then
immediate disconnect. The client is in the appendix.

### Results (stock Ubuntu 46.2 binary, fresh private session per run)

| Run | Client cancel sequence | restore+inhibit | Result | Crash site |
|---|---|---|---|---|
| r1, r3 | metadesk burst (Req.Close, Sess.Close, disconnect) | yes | **SIGSEGV** | A: `screen_cast_session_close` :916 |
| r2 | metadesk burst | yes | **SIGABRT** (`malloc(): mismatching next->prev_size`) | A, as heap corruption |
| r4 | metadesk burst | **no / no** | **SIGSEGV** | A |
| r5 | Req.Close (await) → 2 s → Sess.Close | yes | **SIGSEGV** | A |
| r6 | Sess.Close (await) → 2 s → Req.Close | yes | **SIGSEGV** | B: `on_request_handle_close_cb` :214 |
| r7 | Sess.Close only (await) → disconnect | yes | **SIGSEGV** | B: :216 (frontend closes the request on disconnect) |
| r8 | disconnect only | yes | **SIGSEGV** | A (frontend `peer_died_cb` closes requests, then sessions) |
| r9 | Req.Close only (await), stay connected, then exit | yes | **SIGSEGV** | A (on exit) |
| run2d (apport) | Sess.Close → Req.Close | yes | **SIGSEGV** | B, identical to the Nov 14 field core |

### A/B with upstream fixes (backends built from the 46.2 tag in `~/md-8gzk`; dev headers from `apt-get download` + `dpkg -x`, no install)

| Variant | metadesk burst | Sess.Close → disconnect | Sess.Close → Req.Close | disconnect only |
|---|---|---|---|---|
| 46.2 as tagged | crash (A) | crash (B) | crash (B) | crash (A) |
| 46.2 + 9a4dffa (≈ 47.x–49.0 screencast code) | survives | **crash (B)** | **crash (B)** | survives |
| 46.2 + 9a4dffa + 056029c (≈ 50.0) | survives | survives | survives | survives |

"Survives" means the backend was still running 20 s after the client finished and answered `org.freedesktop.DBus.Peer.Ping`.

### RemoteDesktop path (what master uses by default after md-kmoa)

The client used a paired session, `RemoteDesktop.CreateSession` + `SelectDevices(types=3)` +
`ScreenCast.SelectSources` + `RemoteDesktop.Start`. The chooser was created in every run (parent-window warning logged).
On the stock 46.2 backend, all four sequences survived (burst, Session→Request, Session-only, disconnect)
with no GLib criticals. Caveat: 46.2 `remotedesktop.c` has a related lifetime defect.
`handle_close()` (L215–225) frees the handle without clearing `session->dialog_handle`, and session
close never frees the chooser. These runs show it doesn't crash on these sequences, not that the path
is memory-safe. Restore+inhibit for the RemoteDesktop table was not exercised.

## Stack traces

**Bug A**, faithful repro r1 (stock binary, Ubuntu debuginfod symbols; LTO folds the identical
`screen_cast_dialog_handle_free`/`remote_desktop_dialog_handle_free` bodies, hence the alias in frame #1):

```
** WARNING: Error restoring stream from session: GDBus.Error:org.freedesktop.DBus.Error.Failed: Session creation inhibited
** WARNING: Failed to associate portal window with parent window
GLib-CRITICAL **: g_atomic_ref_count_dec: assertion 'old_value > 0' failed      <- 2nd unref of the Request via the freed handle
Thread 1 "xdg-desktop-por" received signal SIGSEGV, Segmentation fault.
#0  gtk_window_destroy () at /lib/aarch64-linux-gnu/libgtk-4.so.1
#1  remote_desktop_dialog_handle_free (dialog_handle=0xaaaaaad8faa0) at ../src/remotedesktop.c:202   [== screencast.c:103]
#2  screen_cast_dialog_handle_close (dialog_handle=0xaaaaaad8faa0) at ../src/screencast.c:113
#3  screen_cast_session_close (session=0xaaaaaac2f6d0) at ../src/screencast.c:916
#4  session_close (notify=0, session=0xaaaaaac2f6d0) at ../src/session.c:87
#5  handle_close (object=0xaaaaaac2f6d0, invocation=0xffffdc018170) at ../src/session.c:117
#6  _g_dbus_codegen_marshal_BOOLEAN__OBJECT … (org.freedesktop.impl.portal.Session.Close)
```

The self-built 46.2 control shows the same path without the alias: `#1 screen_cast_dialog_handle_free (…) at ../src/screencast.c:103`.
Other runs fault one statement later, in `g_object_unref (dialog_handle->request)` (screencast.c:105), or abort in malloc.

**Bug B**: runs r6/run2d, and the pre-existing field core `/var/crash/_usr_libexec_xdg-desktop-portal-gnome.1000.crash` from 2026-11-14
(same package and binary; the session pointer is reused string data, ASCII `"me/Shell"`):

```
Program terminated with signal SIGSEGV, Segmentation fault.
#0  cancel_start_session (response=2, screen_cast_session=0x6c6c6568532f656d) at ../src/screencast.c:203
#1  on_request_handle_close_cb (object=<optimized out>, invocation=<optimized out>, dialog_handle=0xbb85154a3da0) at ../src/screencast.c:214
#2  _g_dbus_codegen_marshal_BOOLEAN__OBJECT.part.0.lto_priv.0 (…) at src/xdg-desktop-portal-dbus.c:443
#7  _xdp_impl_request_skeleton_handle_method_call (…, interface_name="org.freedesktop.impl.portal.Request", method_name="Close", …)
#9  call_in_idle_cb (…) at ../../../gio/gdbusconnection.c:5453
#14 main (…) at ../src/xdg-desktop-portal-gnome.c:336
```

**The Nov 21 incident itself.** No core was kept for 19:43:12 (`systemd-coredump` isn't installed, and the
only report for this binary in `/var/crash` was the Nov 14 one). The journal shows the repro's
preconditions in order: *Session creation inhibited* → *Failed to associate portal window with parent
window* (chooser created behind the lock) → 180 s → SEGV. In the repro the crash follows the cancel within
~20–40 ms, while in the field it came ~2 s later. The gap wasn't explained, and it doesn't change the diagnosis:
the faulting code runs when the frontend's forwarded close reaches the backend.

## Upstream status

| Item | Status |
|---|---|
| 9a4dffa "screencast: Fix valgrind warning" (Bug A), MR !164 | in 47.0+; cherry-picked to gnome-46 as 7c44035 (2024-05-28) but **no 46.x tag after 46.2** |
| 056029c "screencast: Disconnect signal handlers on data free" (Bug B), MR !231 | in 50.0+; gnome-49 backport 1d765f1 not in any tag; **not on gnome-46/47/48** |
| GNOME issue #60 "Crashes after closing OBS" (43.0) | open, *1. Crash / 2. Needs Information*; same Bug B stack |
| LP #2103434 "…SIGSEGV in cancel_start_session from on_request_handle_close_cb…" | New; Ubuntu task only, no noble SRU |
| Ubuntu noble | `46.2-0ubuntu1` in noble-updates is the candidate, with neither fix |

Fixed releases: **50.0 or later** (both bugs). 47.x/48.x and 49.0 still crash when the session closes
before the request. Not checked: whether a later Ubuntu upload for 24.10–25.10 cherry-picked 056029c.

## metadesk review: timeout, cancel and Close ordering

**At `cb7b1cd`, and still the legacy path on master** (`src/core/capture_pipewire.c`; master line numbers in brackets):

- `capture.c:72-77`: consent budget is 180 s interactive, 10 s unattended. The *length* doesn't matter:
  the repro crashes with 8 s. Shortening or lengthening it only moves the crash.
- `capture_pipewire.c:287 [290]`: one absolute deadline covers all steps, so by the time Start times
  out the budget is spent.
- `:225-235 [228-238]`: on timeout, `Request.Close` is sent **no-reply**.
- `:465-469 [468-472]` → `fail:` `:590-591`: goes straight to `portal_screencast_close()`.
- `:600-616 [603-619]`: `Session.Close`. The budget is `portal_remaining_ms(portal, 1000)` (`:607 [610]`),
  which is 0 after a timeout, so it also becomes **no-reply** (`:613-616 [616-619]`).
- `:624 [627]`: the private connection is closed immediately, so the frontend's `peer_died_cb` also closes
  requests and sessions.

So metadesk triggers three overlapping teardowns at once, and their order at the backend isn't deterministic.
The order metadesk *intends*, Request before Session, is the right one: it's the only one that survives
47.x–49.0. But it's sent as an unacknowledged burst, and on 46.2 no order is safe anyway.

**Master `8be1c74` after md-kmoa** (default for interactive Wayland hosts, `src/host/main.c:1927-1941`):
`src/core/input_portal.c` drives `RemoteDesktop.Start` (`:628-629`). On cancel, `close_object()`
(`:152-164`) sends `Request.Close` **with a reply** and a 1 s bound, then `Session.Close` is acked
(`:548`), then the connection closes (`:549`). That is already the deterministic ordering, and the
RemoteDesktop backend path didn't crash in testing.

**What would and wouldn't help:**

| Option | Verdict |
|---|---|
| Reorder to Session.Close first | **Harmful**: crashes 46.2 *and* 47–49 (Bug B). |
| Change/shorten the 180 s timeout | No effect on the crash; only moves it. |
| Ack `Request.Close` before `Session.Close` (legacy path) | Helps on 47.x–49.0 by removing the thread race. No help on 46.2. |
| **Don't issue Start while the GNOME session is locked** | **Recommended.** On a locked GNOME session restore *always* fails (Mutter inhibits session creation) and a chooser nobody can see is guaranteed. Skipping Start avoids the crash, the 180 s stall, and the loss of the remembered grant: the frontend deletes the stored permission on lookup (`restore-token.c` in 1.18.4) and only re-stores it when the session completes. |

**Filed: md-xhkx** (discovered-from md-8gzk; depends on md-kmoa, now closed):

- **M1, lock gate.** Before the portal Start (`input_portal.c:628` on master; legacy `capture_pipewire.c` Step 3
  `:439` on master / `:436` at cb7b1cd, ideally before CreateSession `:341`/`:338`), check whether the session is
  locked: `org.gnome.ScreenSaver.GetActive` on the session bus, or logind `Session.LockedHint` for our own
  session. If locked, don't issue Start. Either return a distinct "desktop locked" capture failure promptly,
  or subscribe to `org.gnome.ScreenSaver.ActiveChanged` and continue on `false` within the existing consent
  deadline. That's event-driven with no polling, and it never does Start-then-cancel.
- **M2, legacy path only.** Send `Request.Close` with a reply and await it (or the Response) within a small
  fixed cleanup budget that doesn't depend on the spent consent deadline. Then send `Session.Close` and await
  its ack, then disconnect. Keep Request-before-Session. This mirrors `input_portal.c:close_object()`.
- Acceptance: a private-session fixture where a locked or inhibited session (or a fake `GetActive=true`) causes no
  Start, a distinct status, and a surviving restore token. Tests never lock or unlock a live desktop.

Neither change helps a *pending chooser on an unlocked desktop* that nobody answers (r4) on the
ScreenCast path. That case can only be fixed by the backend (upgrade or SRU), or avoided by not using the
legacy path on GNOME 46 hosts.

## Recommendations

1. **Owner: decide on upstream filing.** The recommended route is to comment on GNOME #60 with the reproducer below
   and ask for 056029c to be backported to gnome-47/48 and a 46.3 to be tagged (or 9a4dffa + 056029c added to the
   46 branch). Then add a noble task and an SRU request on LP #2103434 for 9a4dffa + 056029c.
2. **Implement md-xhkx M1.** It's useful on both paths and saves consent even on fixed backends.
3. **Operations.** On Ubuntu 24.04 GNOME hosts, prefer the paired RemoteDesktop path (the master default) and
   avoid `--no-input`/`--input-backend uinput` until the backend is fixed. After a backend crash, recovery
   requires a human unlock and a new attended consent; never restart services on a shared desktop.

## Ready-to-file upstream report (not filed)

> **Title:** screencast: use-after-free / double free of ScreenCastDialogHandle when a pending Start is cancelled (Request.Close + Session.Close); still crashes 47–49 when the session closes first
>
> **Versions:** xdg-desktop-portal-gnome 46.2 (Ubuntu 24.04 `46.2-0ubuntu1`, arm64), xdg-desktop-portal 1.18.4, GNOME Shell/Mutter 46, GTK 4.14.5, GLib 2.80.0. Also reproduced with 46.2 + 9a4dffa (≈ 47–49 screencast code). Not reproducible with 46.2 + 9a4dffa + 056029c.
>
> **Summary:** If a ScreenCast `Start` chooser is pending and the client gives up, the backend crashes. Two paths:
> (A) `on_request_handle_close_cb()` frees `dialog_handle` without clearing `screen_cast_session->dialog_handle`, and the following `Session.Close` (or the frontend's peer-death cleanup) frees it again in `screen_cast_session_close()` (screencast.c:916). Fixed by 9a4dffa, which was never released on 46.x.
> (B) If the session closes first, `screen_cast_session_close()` frees the handle but its `handle-close` handler stays connected to the exported Request. The later `Request.Close` (explicit, or sent by the frontend when the client disconnects) runs `on_request_handle_close_cb()` on freed memory (screencast.c:203/214). Fixed only by 056029c (50.0; gnome-49 backport untagged).
> The frontend dispatches `Request.Close` and `Session.Close` from GDBus worker threads, so a client sending both back to back can hit either path. A common trigger is restoring a session while the screen is locked: Mutter returns "Session creation inhibited", `restore_stream_from_data()` falls back to the chooser behind the lock screen, and the client times out.
>
> **Steps to reproduce** (headless and isolated; no user interaction needed):
> 1. `dbus-run-session` with a private `HOME`/`XDG_RUNTIME_DIR`; start `gnome-shell --headless --no-x11 --virtual-monitor 1280x720 --wayland-display test-wl` and run the backend under gdb on that bus.
> 2. Optional (the locked-screen case): seed the permission store with screencast restore data for the virtual monitor and call `global.backend.get_remote_access_controller().inhibit_remote_access()` through Eval in that shell.
> 3. As a client: `ScreenCast.CreateSession`, `SelectSources(types=1, persist_mode=2[, restore_token])`, `Start(session, "")`. Don't answer the chooser.
> 4. After a few seconds, either (A) call `Request.Close` and then `Session.Close`, or just disconnect, or (B) call `Session.Close` and then `Request.Close`, or `Session.Close` and then disconnect.
> 5. The backend dies with SIGSEGV or SIGABRT (malloc corruption). 10/10 runs on 46.2.
>
> **Expected:** the chooser is dismissed, Start completes with response 2, and the backend survives.
>
> **Backtraces:** (A) `gtk_window_destroy ← screen_cast_dialog_handle_free (screencast.c:103) ← screen_cast_dialog_handle_close (:113) ← screen_cast_session_close (:916) ← session_close (session.c:87) ← handle_close (session.c:117)`, preceded by `GLib-CRITICAL g_atomic_ref_count_dec: assertion 'old_value > 0' failed`. (B) `cancel_start_session (screen_cast_session=0x6c6c6568532f656d) (screencast.c:203) ← on_request_handle_close_cb (:214) ← _xdp_impl_request_skeleton_handle_method_call ("org.freedesktop.impl.portal.Request", "Close")`. The full gdb output and a ~100-line Python client are available.
>
> **Request:** backport 056029c to gnome-47 and gnome-48, and tag a 46.3 with 7c44035 + 056029c (or point distributions at both commits). For Ubuntu, add a noble task to LP #2103434 and SRU 9a4dffa + 056029c into 46.2.

## Cleanup receipt (VM 192.168.64.3)

- **Processes.** All private-session processes were matched by the private bus address in `/proc/<pid>/environ`
  and sent SIGTERM (7 still on the bus at the end; the rest exited with their parents). A later sweep found no
  process with cwd or fds under `~/md-8gzk`, no extra gnome-shell, dbus-daemon, portal, keyring, ibus,
  evolution, gvfs or gdb instances, and every remaining GNOME process dated from Nov 8–22. The live backend PID 3045,
  live frontend 301038 and live shell 300603 kept running throughout.
- **Files.** `~/md-8gzk` removed (source, private builds, dev-deb sysroot, logs, cores, private HOME/runtime).
  No `/tmp/dbus-*` private sockets left. No private document-portal FUSE mount left.
- **Side effect, restored.** When my harness still had a race (runs 2d and 5), apport handled two
  private-backend crashes and **replaced** `/var/crash/_usr_libexec_xdg-desktop-portal-gnome.1000.crash`
  (00:55:35 and 00:57:12). I restored the original Nov 14 report each time from a byte copy, with its
  original mtime (7983984 bytes, 2026-11-14 10:07:49). No `.upload`/`.uploaded` markers exist. The live
  session's `update-notifier` (PID 301514) was running, so it **may** already have queued a crash prompt, which
  would appear after the desktop is unlocked. After the harness fix, every crash was caught by gdb and apport was never invoked.
- **Not done.** No sudo, no package installs (dev debs were only extracted into `~/md-8gzk`), no service or bus
  restarts, no lock/unlock of the live desktop, no live consent dialogs, no uinput, no AT-SPI. `~/md-kmoa`
  was never touched; it was already removed by that agent by the end of this run.
- **Side effects of the private headless shell (expected, released on exit).** It logged "Registering session with GDM"
  and took a logind sleep-delay inhibitor (`loginManager.js`); both are released when the process exits.
- **Local.** Upstream clones and scratch scripts under `/tmp/md-8gzk-*` on the Mac removed after writing this report.

## Appendix: client used for the reproduction

```python
#!/usr/bin/env python3
# usage: portal_client.py <metadesk|req-then-sess|sess-then-req|sess-only|disconnect|req-only> [restore_token] [start_timeout_s]
# RD=1 in the environment uses RemoteDesktop.CreateSession/SelectDevices + ScreenCast.SelectSources + RemoteDesktop.Start
import os, sys, uuid
from gi.repository import Gio, GLib
BUS, OBJ = "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop"
SC, RDI, REQ, SES = ("org.freedesktop.portal.ScreenCast", "org.freedesktop.portal.RemoteDesktop",
                     "org.freedesktop.portal.Request", "org.freedesktop.portal.Session")
mode, token = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "")
wait_s, RD = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0, os.environ.get("RD") == "1"
conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
conn.set_exit_on_close(False)
sender, loop, responses = conn.get_unique_name()[1:].replace(".", "_"), GLib.MainLoop(), {}
def on_resp(c, s, path, i, sig, params, _): responses[path] = params.unpack(); loop.quit()
conn.signal_subscribe(BUS, REQ, "Response", None, None, Gio.DBusSignalFlags.NONE, on_resp, None)
def request(method, args_fn, timeout_s, iface=SC):
    t = "md8gzk" + uuid.uuid4().hex[:8]; path = f"{OBJ}/request/{sender}/{t}"
    conn.call_sync(BUS, OBJ, iface, method, args_fn(t), None, Gio.DBusCallFlags.NONE, 5000, None)
    src = GLib.timeout_add(int(timeout_s * 1000), loop.quit); loop.run()
    if path in responses: GLib.source_remove(src); return path, responses[path]
    print(method, "TIMEOUT"); return path, None
def call(path, iface, member, reply=True):
    if reply:
        try: conn.call_sync(BUS, path, iface, member, None, None, Gio.DBusCallFlags.NONE, 5000, None)
        except GLib.Error as e: print(iface, member, "error:", e.message)
    else:
        m = Gio.DBusMessage.new_method_call(BUS, path, iface, member)
        m.set_flags(Gio.DBusMessageFlags.NO_REPLY_EXPECTED); conn.send_message(m, Gio.DBusSendMessageFlags.NONE)
V = GLib.Variant
_, r = request("CreateSession", lambda t: V("(a{sv})", ({"handle_token": V("s", t),
    "session_handle_token": V("s", "metadesk_session")},)), 5, RDI if RD else SC)
session = r[1]["session_handle"]
if RD:
    request("SelectDevices", lambda t: V("(oa{sv})", (session, {"handle_token": V("s", t),
        "types": V("u", 3), "persist_mode": V("u", 2)})), 5, RDI)
opts = {"types": V("u", 1), "cursor_mode": V("u", 2)}
if not RD: opts["persist_mode"] = V("u", 2)
if token: opts["restore_token"] = V("s", token)
request("SelectSources", lambda t: V("(oa{sv})", (session, dict(opts, handle_token=V("s", t)))), 5)
start, r = request("Start", lambda t: V("(osa{sv})", (session, "", {"handle_token": V("s", t)})), wait_s, RDI if RD else SC)
if r is not None: call(session, SES, "Close"); sys.exit(0)
if mode == "metadesk":            # capture_pipewire.c:228-235 + :600-624
    call(start, REQ, "Close", False); call(session, SES, "Close", False); conn.flush_sync(None); conn.close_sync(None)
elif mode == "req-then-sess": call(start, REQ, "Close"); GLib.usleep(2_000_000); call(session, SES, "Close")
elif mode == "sess-then-req": call(session, SES, "Close"); GLib.usleep(2_000_000); call(start, REQ, "Close")
elif mode == "sess-only":     call(session, SES, "Close"); GLib.usleep(5_000_000); conn.close_sync(None)
elif mode == "disconnect":    conn.close_sync(None)
elif mode == "req-only":      call(start, REQ, "Close"); GLib.usleep(10_000_000)
```
