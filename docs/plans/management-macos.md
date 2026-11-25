# macOS management companion prototype (`md-pep3`)

This is the native AppKit counterpart to the [Linux manager](../manager.md), not a port of GTK. The host CLI and SDL overlay remain independent. The current companion never starts a host, capture, input, or a client session. Closing it does not stop the host. See the [macOS Manager guide](../manager-macos.md) for what the prototype exposes today. See the [management UX plan](management-ux-2026-11-14.md) and [host lifecycle](../host-lifecycle.md).

## Build and layout

With the normal macOS core dependencies and a built, pinned nostrc tree:

```sh
meson setup build-mac-manager -Dmacos_manager=true -Dclient=false \
  -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dnostrc_root="$NOSTRC" -Dnostrc_build_dir="$NOSTRC/_build"
meson compile -C build-mac-manager
meson test -C build-mac-manager --suite macos_manager --print-errorlogs
```

`-Dmacos_manager` is off by default and macOS-only. It enables no GTK or libadwaita dependency. The prototype bundle requires macOS 13 or later for its System Settings pane links; this does not change the host's OS floor. The host-side `host.permissions` v1 wire extension and shared ScreenCaptureKit/AX preflights ship in **every macOS host build**, independently of this option. The build produces `build-mac-manager/src/macos_manager/Metadesk Manager.app` (`Contents/{Info.plist,MacOS/metadesk-macos-manager}`). It uses build-tree libraries; it is not installed, relocatable, or added to the release DMG. The [packaging plan](packaging-2026-11-21.md) keeps the shared private library closure and existing host/client products; release integration, Developer ID, notarization, and the stable host helper/TCC identity are separate (`md-wdi3`). The prototype is not a gate on the current host/client DMG.

## Pages and core boundaries

| Page | Prototype | Follow-up |
| --- | --- | --- |
| Hosts | Explicit read from an existing private state directory via `MdManagedStore`, with the saved client role and its verified signer. Shows the committed cached membership as selectable `npub` and fingerprints. Pending, partially acknowledged and conflicting drafts are flagged separately. Missing cache is not empty relay history. | Adopt the GTK-free `md-vb18` controller for live sync, metadata, favorites/tags, trust review, and client launch. This prototype owns no relay bridge, outbox transport, or connect workflow. |
| This Computer | One-shot v1 same-user `host.permissions` via `md_management_call` on an explicit socket. Reports lifecycle and permission status; older hosts still provide a basic snapshot. | Provisioning, FIPS readiness, host QR, start/stop and login registration. |
| Access | Request inbox count from the host snapshot, not policy or authority. | Inbox/remembered-client UI and exact scoped decisions; continue using the host CLI for changes. |
| Identity | Read saved `MdAccounts` metadata, client/host selections and backend. Same identity can fill both roles. Dumps no credentials, does not open a signer to list accounts. Keys show an `npub` and the first 16 hex characters in four groups; a fingerprint is for comparison, not authentication. | Role binding UI with live signer verification, signer status/actions, account CRUD. Existing CLI validates role/FIPS constraints. |
| Relays | Offline `identity_resolver` and `relay_plan` preview for a supplied key and explicit bootstrap URL; does not consume nprofile hints or discover metadata. No default relay. | Reuse the shared controller for HTTPS NIP-05/TOFU, relay discovery, delivery/write planning and status. |
| Activity | Reports whether the host's session slot is occupied; not proof of a bound stream. | Session management, live subscription and local history. |

AppKit owns the main thread. Explicit actions dispatch blocking account, signer, store and IPC work to a serial worker. Quit waits for queued reads to complete. Each cache read closes the owner lock and role binding before returning; no decrypted document is persisted by the frontend. The returned C plaintext buffer is wiped, but Foundation's decoded strings and the displayed keys live in process memory and are not explicitly zeroed. Reading the cache does not synchronize its documents with relays, but the configured signer may need its own network and user approval. There is no Metadesk password: unlock in the external signer, start a missing socket signer, or handle remote signer approval there. Live signer lock/transport state is not inferred from saved metadata. Local NIP-46 credential errors are reported separately from remote signer failures: an unsigned build without access to the saved item's Keychain ACL can fail before reaching the signer. The core lookup does not request Keychain UI. No new Keychain handling is introduced.

## Private client channel (`md-qgxe`, transport only)

The Darwin client now accepts `--managed-session-fd` as its only option in a managed launch. A future AppKit Hosts controller can use `md_managed_launch_spawn` on its worker with an absolute client executable path, then send the reviewed config and consume versioned progress and outcomes. Darwin uses a same-user Unix stream: a bounded big-endian configuration length followed by JSON, then single-byte versioned status/command messages. Linux keeps Unix `SOCK_SEQPACKET` and its existing wire format. Windows still has no managed launch channel.

Darwin's spawn explicitly maps the child endpoint to descriptor 3, attaches standard streams to `/dev/null`, and uses close-on-exec by default for the remaining descriptors. The environment forwards only runtime locations; no signer secret or pairing goes through the channel, arguments or environment. The client validates local peer credentials and independently restores and checks the saved signer. Callers own the parent endpoint and must reap children, including failed launches. The worker must remain responsive to cancellation and distinguish an unfinished launch from an already connected session before tearing down the channel.

There is no AppKit Connect action yet. The next Hosts slice must implement the shared controller, pin review and launch lifecycle together; neither the cache viewer nor the offline preview is sufficient authorization to launch. AppKit approvals/policy edits, session kick, role binding and host-agent controls also remain open. Do not use a standalone `launchctl bootstrap` of the packaged `BundleProgram` plist as a substitute for the signed registration flow in [host lifecycle](../host-lifecycle.md#macos-bundle).

## Permission onboarding

1. Start the already provisioned host as its intended user-session identity using the CLI or configured supervisor. Enter its [management socket](../host-management.md) and select **Recheck Host**. Only that authenticated IPC peer reports permissions; the GUI never checks its own grant and attributes it to the host.
2. **Allowed** is a positive preflight; **Not allowed** combines never-granted, denied and revoked because the APIs do not distinguish them. **Unknown** means no trusted, supported report, not absence of permission. Values are snapshots, not monitored status. The ScreenCaptureKit and AX backends and IPC use the same `CGPreflightScreenCaptureAccess` and `AXIsProcessTrusted` checks. These do not request consent. AX trust is not a probe of Quartz event delivery; the CGEvent backend's event creation is not used as a permission detector.
3. The corresponding button opens the Screen Recording (Screen & System Audio Recording on newer releases) or Accessibility pane. If the link fails, use **System Settings › Privacy & Security**. The URLs are best-effort OS integration; the operator grants the *responsible host identity*, not the manager. Terminal-launched and bundled agents may have different responsible identities. Restart the host if macOS requires it, then select **Recheck Host**. Neither a grant nor opening Settings enables sharing or authorizes a remote client.

The prototype does not request permissions, start a capture to test them, query the accessibility tree, or post an input event. Headless tests cover the host wire contract with stubbed probes, permission state and pane selection, saved role/identity display with an isolated accounts directory, cache messages with a fake store, and offline identity and bootstrap behavior. A real screen/Settings flow and signed helper attribution require separate interactive qualification.
