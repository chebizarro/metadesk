# macOS Manager (preview)

Metadesk Manager is an optional native AppKit companion. It is a read-only
preview, not yet equivalent to the [Linux Manager](manager.md). It does not
start `metadesk-host`, enable sharing, or launch `metadesk-client`. Quitting
it does not stop a host that is already running.

## Build

With the normal macOS dependencies and a built, pinned nostrc tree:

```sh
meson setup build-mac-manager -Dmacos_manager=true -Dclient=true \
  -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dnostrc_root="$NOSTRC" -Dnostrc_build_dir="$NOSTRC/_build"
meson compile -C build-mac-manager -j2
meson test -C build-mac-manager --suite macos_manager --print-errorlogs
open 'build-mac-manager/src/macos_manager/Metadesk Manager.app'
```

The preview targets macOS 13 or later; your development dependencies may
require a newer version. The bundle uses build-tree libraries. It is not an
installed or signed release and cannot qualify the packaged host's permission
identity.

## What you can do

- **Hosts:** read previously cached host keys with the saved client-role signer.
  A missing cache does not mean the relay address book is empty. There is no
  live address-book sync, editing, import/export, or Connect button yet.
- **This Computer:** enter the running host's absolute [management socket
  path](host-management.md#endpoint-and-peer-trust) and select **Recheck Host**.
  This is an authenticated snapshot, not a background monitor. Screen Recording
  and Accessibility report the *host's* preflight state, not the manager's.
  **Unknown** means the host did not provide a trusted permission report.
- **Access and Activity:** see the inbox count and session-slot status. Use
  the [host CLI](host-management.md#startup-and-cli) to review or decide a
  request, or to end a session. A reserved slot is not proof of a live stream.
- **Identity:** read saved public account metadata and role selections. Use
  the [account CLI](accounts.md#headless-cli) to verify and bind roles. There
  is no Metadesk password; unlock or authorize your external signer there.
- **Relays:** preview a public key and an explicit bootstrap URL offline. This
  does not resolve NIP-05, discover relays, or contact the host.

For an already provisioned host, the permission buttons open the appropriate
System Settings pane; they do not request permission or turn on sharing. Grant
permissions only to the intended host identity. Follow the documented
[host lifecycle](host-lifecycle.md#macos-bundle) separately; this preview does
not register, start, or stop a LaunchAgent.
