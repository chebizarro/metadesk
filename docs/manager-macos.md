# macOS Manager (Hosts preview)

Metadesk Manager is an optional native AppKit companion. **Hosts** can sync,
edit and connect. The other pages are still read-only previews, not yet
equivalent to the [Linux Manager](manager.md). The app does not start
`metadesk-host` or enable sharing. Quitting it does not stop a running host.

## Build and run

With the normal macOS dependencies and a built, pinned nostrc tree:

```sh
meson setup build-mac-manager -Dmacos_manager=true -Dclient=true \
  -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dnostrc_root="$NOSTRC" -Dnostrc_build_dir="$NOSTRC/_build"
meson compile -C build-mac-manager -j2
meson test -C build-mac-manager --suite macos_manager --print-errorlogs
PATH="$PWD/build-mac-manager:$PATH" \
  'build-mac-manager/src/macos_manager/Metadesk Manager.app/Contents/MacOS/metadesk-macos-manager'
```

The preview targets macOS 13 or later; your development dependencies may
require a newer version. The bundle uses build-tree libraries. It is not an
installed or signed release and cannot qualify the packaged host's permission
identity. The manager searches for `metadesk-client` beside its own executable,
then in the app bundle, then in absolute directories on `PATH`.

## Hosts

1. Assign and verify a client role using the [account CLI](accounts.md#headless-cli).
   Open **Hosts** to load its encrypted cache. For live sync, enter explicit
   bootstrap relays separated by spaces and select **Refresh Address Book**.
   Relays are session-local; none is preselected. `ws://` is unencrypted.
2. Enter an `npub`, `nprofile`, lowercase hex public key, or NIP-05 name.
   Enter a direct IPv4 or IPv6 address only in the optional route field; it
   cannot substitute for the host key. Review the full npub, short fingerprint,
   NIP-05 status and requested access before saving or connecting. A changed
   NIP-05 pin blocks connection. Keep the old key, or verify the new key with
   the host and type its exact fingerprint to replace it. Review again before
   connecting. Private-key inputs (`nsec`, `ncryptsec`) are cleared and refused.
3. Filter saved hosts by text or tag, edit labels, notes, favorites and tag
   membership, or switch to **Quick Connect** for favorites followed by local
   recent attempts. Last-connected times are not synced. A new client key does
   not inherit the old key's grants; refresh after changing it and re-request
   access from host administrators.
4. Import `metadesk-hosts/1` files by reviewing and selecting individual
   public keys and requested capabilities. Existing pins are not overwritten.
   Export only to a location you trust: the explicit export is plaintext and
   contains public keys, labels and notes, but no credentials or access grants.

With no reachable relay, cached hosts can still be viewed with the correct
signer. Connect requires catch-up on connected fetch routes; edits require all
required fetch routes. A newer remote version may conflict with a pending local
draft. Review both, then choose **Keep Remote** or **Reapply Draft**. An unknown
schema or undecryptable data is not treated as an empty book and cannot be
silently overwritten. Separate address-book collections are not atomic across
relays. If a multi-collection edit is partially acknowledged, review the
remaining pending collections and retry them explicitly.

Connect opens a separate client window. The manager shows launch progress,
including typed relay authentication, subscription-closed and publication
rejection messages. Use **Cancel Connection** before connection completes or
**Disconnect** once connected. Closing the manager cancels an unfinished launch;
a connected video window is left running. No QR feature is included.

## Other pages

- **This Computer:** enter the running host's absolute [management socket
  path](host-management.md#endpoint-and-peer-trust) and select **Recheck Host**.
  This is an authenticated snapshot, not a background monitor. Screen Recording
  and Accessibility report the *host's* preflight state, not the manager's.
  **Unknown** means the host did not provide a trusted permission report.
- **Access and Activity:** see the inbox count and session-slot status. Use
  the [host CLI](host-management.md#startup-and-cli) to decide requests or end
  a session. A reserved slot is not proof of a live stream.
- **Identity:** read saved public account metadata and role selections. Use
  the [account CLI](accounts.md#headless-cli) to verify and bind roles. There
  is no Metadesk password; unlock or authorize your external signer there.
- **Relays:** preview a public key and an explicit bootstrap URL offline. This
  tab does not change the bootstrap relays entered on Hosts.

The permission buttons open System Settings; they do not request permission or
turn on sharing. Follow the [host lifecycle](host-lifecycle.md#macos-bundle)
separately. The manager does not register, start, or stop a LaunchAgent.
