# Linux Manager

On macOS, see the [native Manager preview](manager-macos.md).

`metadesk-manager` is an optional GTK 4.14 / libadwaita 1.5 companion. For a
remote connection it launches `metadesk-client` as a separate process; it does
not embed SDL or start a local host, input device, or accessibility probe.

```sh
meson setup build -Dmanager=true -Dclient=true -Dsigner_nip5f=true \
  -Dnostrc_root=/path/to/nostrc -Dnostrc_build_dir=/path/to/nostrc/build
meson compile -C build -j2
meson test -C build --suite manager --print-errorlogs
PATH="$PWD/build:$PATH" build/src/manager/metadesk-manager
```

The normal core dependencies are still required. `-Dmanager_qr=auto` (default)
enables QR display and image scanning when libqrencode, ZBar and GdkPixbuf are
available. Native Manager packages use the distro libraries. Source builds can
require them with `-Dmanager_qr=enabled` or omit QR with
`-Dmanager_qr=disabled`. The Manager Flatpak includes its own client but disables
QR and does not bundle libqrencode or ZBar pending `md-rvu4`. For an installed
native build, put `metadesk-client` on `PATH`. Add `-Dsigner_nip55l=true`
and/or `-Dsigner_nip46=true` to enable those existing signer adapters. Merely
listing an account does not mean its transport was compiled or is reachable.
With `-Dmanager=false` (the default), GTK and libadwaita are neither discovered
nor linked by this feature. Non-Linux manager builds are refused explicitly.

## Screens and boundaries

The adaptive sidebar contains Hosts, This Computer, Access, Identity, Relays,
and Activity. Below 650 scale-independent pixels it becomes a navigation view
with a standard back button. The minimum width is 360 pixels. Tab/Shift+Tab
traverse controls, Return activates navigation rows, and Ctrl+W/Ctrl+Q close
the window. Standard window decorations are retained.

- **Identity:** lists real `MdAccounts` metadata with backend, a short
  fingerprint (the first 16 hex digits in four groups), and an expandable, selectable full
  NIP-19 npub. The fingerprint is not an authentication check. Add public
  metadata from a local D-Bus signer, local socket signer, or an already-paired
  remote signer; no private-key import or local key vault is provided.
- **Roles:** independently choose client and host accounts, including the same
  account for both. Saving connects to the actual signer, checks its public key,
  then calls `md_accounts_bind_role` and saves. A failed verification cannot
  replace the saved role. “Not assigned” explicitly unbinds it. Existing
  direct-key integration accounts remain visible but are not offered for new
  managed role assignment. These are saved startup selections: running host
  and client processes must restart to adopt them. There is no live host IPC
  reconfiguration. FIPS host matching remains enforced by host startup, not
  inferred by this picker. A future managed connection must require a client
  role and validate the actual FIPS node key before hosting.
- **Signer status:** a read-only adapter calls `org.nostr.Grotto.Control1`
  `GetStatus` at `/org/nostr/signer/grotto` on `org.nostr.Signer`, and reacts to
  `StatusChanged` and name-owner changes. It never starts a signer service or
  calls privileged mutation methods. Replies are fenced by unique owner and
  request generation. Key lock, desktop-session lock, and signing pause are
  separate rows; none is inferred from another. Missing/error/disconnected
  status is unknown, never unlocked. Current Grotto reports `key_state=unknown`
  and `key_lock_enforced=false`; authoritative key-lock reporting is `md-lf9c`.
  Account discovery/custody/shared identity panels remain `md-crk7`.
- **Relays:** explicit local bootstrap choices, with add/remove, normalization,
  duplicate elimination, and atomic 0600 persistence in
  `$XDG_CONFIG_HOME/metadesk/manager-relays.ini` (default `~/.config/metadesk`).
  Encoded path delimiters are preserved so normalization cannot change the
  requested WebSocket endpoint. Only `ws`/`wss` URLs with a host and valid port
  are accepted; credentials,
  query parameters, fragments, whitespace, and malformed URLs are rejected.
  `ws` is visibly described as unencrypted. No relay is preselected. Use
  **Check Relay Authentication** on a saved relay to read its NIP-11 information
  document; “does not advertise” does not rule out a later NIP-42 challenge.
  Checking does not establish a WebSocket session. The managed client requires
  explicit `--relay` values and never falls back to the CLI default. The
  address book uses the client role's discovery, fetch and storage plans; a
  signaling route is selected separately by the video client.
- **Access:** connect to an explicit local host socket (or launch with
  `metadesk-manager --management-socket /absolute/private/control.sock`). This
  neither starts a host nor enables attended admission. Kernel peer credentials
  and private-path checks are shared with the CLI. Missing/untrusted sockets,
  disconnection, malformed/oversized frames and version mismatch clear authority
  and are shown honestly; reconnect is an explicit action, not polling.
  The inbox contains only unexpired requests in this host generation. Review
  shows the full npub, exact request ID, requested permissions and expiry.
  No permission is preselected; Input is unavailable under `--no-input`.
  Approve is once-only unless “Remember this exact grant” is selected. Deny
  is explicit. A notification only opens the matching current review, never
  approves; exact generation/request notifications are withdrawn on expiry,
  cancellation, approval, disconnect and window close. Existing reviews become
  non-actionable on expiry or a stale host revision. Unknown capability bits
  remain visible and cannot be granted by this manager version.
  Remembered clients come from the host's private allowlist. Edit replaces exact
  caps; Revoke removes the client. Both end existing admission authority after
  acknowledged publication. Legacy/unknown policy is displayed but not edited; unknown permissions can still be revoked.
  External policy edits require explicit Refresh Private Policy, with a
  policy-version writer fence preventing stale overwrites.
- **Activity:** the real reserved/active session and its effective permissions,
  with confirmed End Session. A reserved ID is not proof of TCP binding;
  “Ending” means revocation requested, not completed OS cleanup. The row must
  disappear to prove host teardown. This is not an invented activity history.
- **Hosts:** enter an `npub`, `nprofile`, lowercase hex key or NIP-05 name.
  A separate optional IP field is only an explicit route for the reviewed key;
  no connection falls back to TCP. Verify the full npub and short fingerprint
  before connecting or saving. A changed NIP-05 key blocks connection; replace
  it separately by typing the new fingerprint. First contact requests video
  only. Use saved hosts, favorites, tag filtering and recent attempts to find
  a reviewed identity again. Last-connected timestamps stay on this device.
- **Offline:** the signer can decrypt the cached address book without a relay.
  Saving and connecting remain unavailable until the required subscriptions
  have caught up. Conflicting documents offer Keep Remote and Reapply Draft;
  unknown or unreadable data is never overwritten. Refresh Address Book
  starts fresh scoped subscriptions after a delivery failure.
- **Exchange:** import `metadesk-hosts/1` after reviewing each new key. Existing
  entries and conflicting pins are not silently overwritten. Export is an
  explicit plaintext file with keys, labels and notes, never grants or signer
  secrets. Tag IDs travel with the file; the format does not include tag names.
  When QR is enabled, Scan QR Image reads an `nprofile` from an image file (no
  camera is accessed).
- **This Computer:** when QR is enabled, the selected host role can display its
  public `nprofile` QR. Local host lifecycle controls remain separate.
- **Video launch:** the only launch arguments are `--managed-session-fd 3`.
  The private inherited channel carries a versioned launch configuration,
  progress, cancellation, and outcome. The child independently restores its
  signer from the OS credential store and verifies the selected key. Closing
  the Manager cancels an unfinished launch; a connected video window survives.

All account and file operations run on a serialized worker task, with no GTK
objects accessed from that worker. The account controller is opened per
operation, preserving its exclusive writer lock and snapshot semantics; CLI
access is not blocked for the window's lifetime. Refresh rereads real metadata.
Mutations hold the application until completion even if the window is closed,
without accessing destroyed widgets. Signer calls retain their backend's own
watchdog; the manager adds no polling or retry-to-success loop.

## Validation

`manager-model` needs neither a display nor a signer daemon: it covers listing,
public-key conversion, fingerprint formatting, verified role persistence,
wrong-key refusal, writer contention, relay validation, deduplication, private
file mode, empty selection, and corrupt input. A custom signer fixture exercises
the same production `MdAccounts` role-binding path, not an alternate save path.

`manager-ui` runs the production window and read-only status adapter against a
fake Control1 service on a **private** session bus under Xvfb. It covers missing
service, locked/unlocked/unknown status, pause separation, stale replies, owner
loss, six-section navigation at 360px, focus traversal, invalid and valid public
metadata/relay edits, failed signer verification, persistence, and closing during
a worker task. Native keyboard checks use `xdotool` if available (otherwise an
explicit skip is printed); GTK focus traversal is always checked. Timeouts
are failure watchdogs only. `xvfb-run` and `dbus-run-session` must be installed;
Meson reports explicitly when this test cannot be registered. The private
runner sets `GTK_A11Y=test`. A private runtime and a bus configuration
without activation service directories prevent portal, keyring, and AT-SPI
helpers from touching the live desktop.

The images in [screenshots/](screenshots/) are explicitly **fixture renders**,
not live-service evidence. To regenerate them, set `MD_MANAGER_SCREENSHOT_DIR`
to an existing directory when running the manager test suite. Rendering waits
for the navigation page's `shown` signal, not an arbitrary animation delay.
The narrow test asserts a 360px native surface; client content can be narrower
because window decoration shadows are included in that surface.

`manager-host-ipc` uses a private fake Unix server for fragmented full snapshots,
exact command serialization, policy listing, revision/generation fencing,
version mismatch, oversized frames, disconnection and canceled-connect cleanup.
`manager-access-ui` drives the production window, review/permission controls,
private revoke, keyboard focus and 360px layout. A private
`org.gtk.Notifications` service observes real GNotification calls, verifies exact
review targets and no approval actions, and checks withdrawal on cancel/expiry.
These are isolated fixture tests, not GNOME Shell notification-appearance proof.

`manager-hosts-offline` uses an encrypted cache, a throwaway NIP-5F signer,
and a loopback relay. It checks offline saved data, native keyboard input
(`xdotool` required), a 360px surface, QR encoding/decoding when enabled (scan
control absent when disabled), and key review
after EOSE. `manager-launch` exercises the parent on a fixture child;
`client private managed launch` exercises the real child entry point without
needing a signer, desktop, or remote session. The scoped controller fixture
checks EOSE readiness, refusal, an echoed draft with negative OK, a retry of
the identical signed event, and a positive OK.

For the real-binary test on Linux:

```sh
python3 tests/test_manager_live.py BUILD/metadesk-host BUILD/metadesk-client \
  BUILD/access_signer_fixture BUILD/src/manager/test_manager_access
```

It uses the real attended host, headless client, private WebSocket relay and
throwaway cryptographic signers. The driver clicks the production manager GTK
controls under isolated Xvfb/D-Bus: once, deny, remember, kick, exact grant edit,
and revoke. It checks authenticated TLS binding/disconnection and private-only
persisted policy. It never creates uinput devices or captures the desktop.
GNOME was locked during W16 qualification; this is real host/IPC/relay evidence
through headless UI controls, not live-desktop interaction or Shell appearance.
Screenshot rendering waits for toolkit presentation/hit-testing and after-paint,
not guessed animation sleeps. Screenshot-only runs enable GTK’s reduced-motion
setting; normal test runs retain animations.

Diff-scoped Nostr protocol review A–J: no new relay REQ/filter/reconnect path,
polling, sleep-based completion or unchecked publication. GUI mutations call
same-user IPC only. Host edits reuse the positive-OK private writer and its
unchanged-base fence; the added policy version is checked under that lock.
Exact authenticated request deadlines only withdraw/disable stale UI, never
stand in for relay history or publish completion.

The shell follows the GNOME HIG's
[adaptive sidebar](https://developer.gnome.org/hig/patterns/nav/sidebars.html)
and [placeholder](https://developer.gnome.org/hig/patterns/feedback/placeholders.html)
patterns, using a `.ui` resource, AdwNavigationSplitView, a breakpoint, header
bars, clamped scrollable forms, boxed lists, persistent status, and save toasts.


### W16 qualification (2026-11-22)

| Gate | Result |
|---|---|
| macOS full configured suite, NIP-5F enabled | 60 passed, 1 native AX live test skipped, 0 failures |
| Linux ARM64 VM, private W16 source/dependencies, builds at `-j2` | 63 passed, 1 opt-in first-key desktop test skipped, 0 failures |
| Manager subset (included above) | 4/4 passed, including fake IPC and private headless GTK/notification tests |
| Real Linux host/client/private relay, production manager controls | Once, deny, remember, kick, exact grant edit, revoke/disconnect all passed |

GNOME remained locked. No uinput devices, desktop capture or native input were
used. The source/dependency/build scratch under `~/md-w16` and local
`/private/tmp/md-w16-*` are removed after qualification. No sibling checkout was
edited or built. Initial Linux transfer failures were missing dependency/source
files, not product-test retries; the final private dependency snapshot is nostrc
`cfcb86296068c1e4090bc5ce8a486392dc74eb11`.

Retained screenshots (isolated Xvfb, reduced motion):
[real pending request](screenshots/w16-access-live.png),
[real review after explicitly selecting Video](screenshots/w16-review-live.png),
[real session](screenshots/w16-activity-live.png), and
[360px fixture](screenshots/w16-access-fixture-360.png).
These show the production manager against real IPC or explicitly identified fake
state; they do not show a live GNOME Shell notification.

The second-opinion review received source excerpts after worktree artifact
routing failed. It found no concrete defect in those excerpts; it was **not** a
whole-patch review or independent verification of the tests. Source-backed
review checked the full changed paths and the Nostr boundaries described above.
Source triage used four Jev find-lines queries (12 requests, 87608 input tokens,
approximately $0.00368); narrow hits were expanded for mutex/acceptance context.
No AI relevance judgment controls an approval or policy mutation.


## Optional Grotto Control1 SDK

On Linux, `-Dmanager=true` uses `grotto-control-client-0` when found by
pkg-config. Set `-Dgrotto_control=enabled` to require it, or `disabled` to
build without it. With a compatible Control1 manager snapshot the Identity
page shows the remote signer state. That row stays hidden without the SDK,
without a signer owner, or if the peer does not report a recognized state.
The read-only key/session lock and pause rows still use Control1's existing
fields; missing fields remain unknown. The SDK integration invokes no
commands and grants no signer permissions.
