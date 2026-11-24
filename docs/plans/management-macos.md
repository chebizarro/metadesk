# macOS management companion prototype (`md-pep3`)

This is the native AppKit counterpart to the [Linux manager](../manager.md), not a port of GTK. The host CLI and SDL overlay remain independent. The companion never starts a host, capture, input, or a client session. Closing it does not stop the host. See the [management UX plan](management-ux-2026-11-14.md) and [host lifecycle](../host-lifecycle.md).

## Build and layout

With the normal macOS core dependencies and a built, pinned nostrc tree:

```sh
meson setup build-mac-manager -Dmacos_manager=true -Dclient=false \
  -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dnostrc_root="$NOSTRC" -Dnostrc_build_dir="$NOSTRC/_build"
meson compile -C build-mac-manager
meson test -C build-mac-manager --suite macos_manager --print-errorlogs
```

`-Dmacos_manager` is off by default and macOS-only. It enables no GTK or libadwaita dependency. The build produces `build-mac-manager/src/macos_manager/Metadesk Manager.app` (`Contents/{Info.plist,MacOS/metadesk-macos-manager}`). It uses build-tree libraries; it is not installed, relocatable, or added to the release DMG. The [packaging plan](packaging-2026-11-21.md) keeps the shared private library closure and existing host/client products; release integration, Developer ID, notarization, and the stable host helper/TCC identity are separate (`md-wdi3`). The prototype is not a gate on the current host/client DMG.

## Pages and core boundaries

| Page | Prototype | Follow-up |
| --- | --- | --- |
| Hosts | Explicit read from an existing private state directory via `MdManagedStore`, with the saved client role and its verified signer. Shows the committed cached membership as selectable `npub` and fingerprints. Missing cache is not empty relay history. | Adopt the GTK-free `md-vb18` controller for live sync, metadata, favorites/tags, trust review, and client launch. This prototype owns no relay bridge, outbox transport, or connect workflow. |
| This Computer | One-shot v1 same-user `host.permissions` via `md_management_call` on an explicit socket. Reports lifecycle and permission status; older hosts still provide a basic snapshot. | Provisioning, FIPS readiness, host QR, start/stop and login registration. |
| Access | Request inbox count from the host snapshot, not policy or authority. | Inbox/remembered-client UI and exact scoped decisions; continue using the host CLI for changes. |
| Identity | Read saved `MdAccounts` metadata, client/host selections and backend. Same identity can fill both roles. Dumps no credentials, does not open a signer to list accounts. Keys show an `npub` and the first 16 hex characters in four groups; a fingerprint is for comparison, not authentication. | Role binding UI with live signer verification, signer status/actions, account CRUD. Existing CLI validates role/FIPS constraints. |
| Relays | Offline `identity_resolver` and `relay_plan` preview for a supplied key and explicit bootstrap URL; does not consume nprofile hints or discover metadata. No default relay. | Reuse the shared controller for HTTPS NIP-05/TOFU, relay discovery, delivery/write planning and status. |
| Activity | Reports whether the host's session slot is occupied; not proof of a bound stream. | Session management, live subscription and local history. |

AppKit owns the main thread. Explicit actions dispatch blocking account, signer, store and IPC work to a serial worker. Each cache read closes the owner lock and role binding before returning; no decrypted document is persisted by the frontend. Reading the cache does not synchronize its documents with relays, but the configured signer may need its own network and user approval. There is no Metadesk password: unlock in the external signer, start a missing socket signer, or handle remote signer approval there. Live signer lock/transport state is not inferred from saved metadata. No new Keychain handling is introduced.

## Permission onboarding

1. Start the already provisioned host as its intended user-session identity using the CLI or configured supervisor. Enter its [management socket](../host-management.md) and select **Recheck Host**. Only that authenticated IPC peer reports permissions; the GUI never checks its own grant and attributes it to the host.
2. **Allowed** is a positive preflight; **Not allowed** combines never-granted, denied and revoked because the APIs do not distinguish them. **Unknown** means no trusted, supported report, not absence of permission. Values are snapshots, not monitored status. The ScreenCaptureKit and AX backends and IPC use the same `CGPreflightScreenCaptureAccess` and `AXIsProcessTrusted` checks. These do not request consent. AX trust is not a probe of Quartz event delivery; the CGEvent backend's event creation is not used as a permission detector.
3. The corresponding button opens the Screen Recording (Screen & System Audio Recording on newer releases) or Accessibility pane. If the link fails, use **System Settings › Privacy & Security**. The URLs are best-effort OS integration; the operator grants the *responsible host identity*, not the manager. Terminal-launched and bundled agents may have different responsible identities. Restart the host if macOS requires it, then select **Recheck Host**. Neither a grant nor opening Settings enables sharing or authorizes a remote client.

The prototype does not request permissions, start a capture to test them, query the accessibility tree, or post an input event. Headless model tests cover granted/missing/unknown transitions, pane URL selection, cache projection, and offline identity and bootstrap behavior. A real screen/Settings flow and signed helper attribution require separate interactive qualification.
