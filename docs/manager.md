# Linux Manager

`metadesk-manager` is an optional GTK 4.14 / libadwaita 1.5 companion. It does
not embed SDL or start a connection, host, input device, or accessibility probe.

```sh
meson setup build -Dmanager=true -Dclient=false -Dsigner_nip5f=true \
  -Dnostrc_root=/path/to/nostrc -Dnostrc_build_dir=/path/to/nostrc/build
meson compile -C build -j2
meson test -C build --suite manager --print-errorlogs
build/src/manager/metadesk-manager
```

The normal core dependencies are still required. Add `-Dsigner_nip55l=true`
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
  `first-eight…last-eight` hex fingerprint, and an expandable, selectable full
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
  `ws` is visibly described as unencrypted. No relay is preselected, contacted,
  or claimed connected. These settings are **not yet consumed** by CLI/core
  relay planning (`md-2l87`); they do not rewrite its legacy defaults.
- **Hosts / This Computer / Access / Activity:** honest status-page placeholders,
  not disabled simulations. Saved hosts and launch are `md-vb18`, authenticated
  host IPC and sessions are `md-trex`, approvals/access are `md-a9f2`. No fake
  hosts, permissions, sessions, availability, or outcomes are displayed.

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
Meson reports explicitly when this test cannot be registered. `GTK_A11Y=none`
is confined to that isolated test. A private runtime and a bus configuration
without activation service directories prevent portal, keyring, and AT-SPI
helpers from touching the live desktop.

The images in [screenshots/](screenshots/) are explicitly **fixture renders**,
not live-service evidence. To regenerate them, set `MD_MANAGER_SCREENSHOT_DIR`
to an existing directory when running the manager test suite. Rendering waits
for the navigation page's `shown` signal, not an arbitrary animation delay.
The narrow test asserts a 360px native surface; client content can be narrower
because window decoration shadows are included in that surface.

A fake host IPC test would currently test an invented protocol: the real
authenticated host-control contract is not implemented (`md-trex`). This is a
remaining acceptance boundary, not a passing host-integration claim.

The shell follows the GNOME HIG's
[adaptive sidebar](https://developer.gnome.org/hig/patterns/nav/sidebars.html)
and [placeholder](https://developer.gnome.org/hig/patterns/feedback/placeholders.html)
patterns, using a `.ui` resource, AdwNavigationSplitView, a breakpoint, header
bars, clamped scrollable forms, boxed lists, persistent status, and save toasts.
