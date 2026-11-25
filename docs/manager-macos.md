# macOS Manager

Metadesk Manager is an optional AppKit companion. **Hosts** syncs and connects;
**This Computer** and **Access** manage a running local host; **Identity** manages
saved role assignments. The manager does not start a host or enable sharing.
Quitting it does not stop a host.

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

The app targets macOS 13 or later; development dependencies may require a
newer version. The bundle uses build-tree libraries. It is not a signed release
and cannot qualify the packaged host's permission identity. `metadesk-client`
is resolved beside the manager executable, inside its bundle, then on `PATH`.

## Hosts

1. Assign a client role in **Identity**, then open **Hosts** to load its encrypted
   cache. Enter explicit bootstrap relay URLs separated by spaces and select
   **Save Relays and Refresh Address Book** to sync. Both managers use the same
   per-user `$XDG_CONFIG_HOME/metadesk/manager-relays.ini` file (default
   `~/.config/metadesk`). The app loads the selection on entry but does not sync
   until you refresh. Clear the field and refresh to remove all choices; none
   is preselected or used as a fallback. `ws://` is unencrypted.
2. Enter an `npub`, `nprofile`, lowercase hex public key, or NIP-05 name. Enter
   an IPv4 or IPv6 address only as a route hint for the reviewed key. Verify the
   full npub, short fingerprint, NIP-05 status and requested access before saving
   or connecting. A changed NIP-05 pin blocks connection. Keep the old key, or
   verify the new one with the host and type its exact fingerprint to replace
   it. Review again before connecting. **Scan QR Image** reads one QR code from
   a local image and sends its payload through the same review as typed text.
   `nsec` and `ncryptsec` are cleared and refused, including when scanned. No
   camera or camera permission is used.
3. Filter saved hosts by text or tag. Edit labels, notes, favorites and tag
   membership, or switch to **Quick Connect** for favorites followed by local
   recent attempts. Last-connected times are not synced. A new client key does
   not inherit the old key's grants; request access again from host admins.
4. Import `metadesk-hosts/1` files by reviewing and selecting key rows with exact
   requested capabilities. Existing pins are not overwritten. The explicit
   plaintext export contains public keys, labels and notes, not credentials or grants.

Cached hosts remain viewable without a relay if the signer is available. Connect
requires catch-up on connected fetch routes; edits require all required fetch
routes. Resolve conflicts by reviewing both versions, then choosing **Keep Remote**
or **Reapply Draft**. Unknown schema and unreadable data are not treated as an
empty book. Separate collections aren't atomic; review and retry any pending
collections after partial acknowledgement.

The separate video client reports launch phases and typed relay AUTH, CLOSED,
and publication rejection outcomes. **Cancel Connection** stops an unfinished
launch; **Disconnect** ends a connected session. Quitting the manager cancels
an unfinished launch and leaves a connected video window running.

## This Computer and Access

**Show Host QR Code** displays the saved host role's public `nprofile` and
fingerprint. It does not contact the signer or check whether the host is running.

Enter the running host's absolute [management socket path](host-management.md#endpoint-and-peer-trust)
and select **Connect to Local Host**. Status comes from an authenticated live
subscription. Reconnect explicitly after a host restart or loss of the socket.
A reachable managed process is not proof of LaunchAgent registration, permission,
or capture readiness. If unreachable, the agent's process state is unknown.
**Disconnect Session** ends the slot without changing remembered access; an
occupied slot is not proof of a bound stream.

**Recheck Permissions** asks the authenticated host to report Screen Recording
and Accessibility preflights. **Unknown** means no trusted report. Open the
matching System Settings pane and grant access to the responsible host identity,
not the manager. These buttons don't request permissions or enable sharing.

In **Access**, review the exact request's public key, fingerprint, requested
permissions and lifetime. Only requested and supported screen viewing is selected
initially. Approve once, remember exactly the selected capabilities, or deny.
Expired, cancelled, or stale reviews cannot approve. Refresh private policy
before editing remembered access. **Revoke** removes the grant and ends that
client's admission. **Kick and Revoke** uses the same host operation for the
selected active client. A failed reply does not prove the grant is gone.

A signed app may post an open-to-review notification only when already authorized.
Notifications cannot approve and are withdrawn on expiry, cancellation and other
inbox removal. The manager does not request notification authorization. Unsigned
builds use the inbox only.

### Starting and stopping the host

The [macOS lifecycle contract](host-lifecycle.md#macos-bundle) defines no in-app
start/stop command for an already-registered agent. Sign-in to the Aqua session
starts an opted-in, registered agent; logout ends it. Registration and approval
belong to the installed app (`md-wdi3`). For manual use, provision the host role
and policy first. Then run the host in a terminal with its usual relay/routing
flags and a canonical private socket path, for example
`metadesk-host --managed --management-socket "$SOCKET" --relay "$RELAY"`.
Add `--attended` only to receive unlisted requests. Stop the foreground process
with Control-C. Do not run a second host for the same endpoint or manually
bootstrap the bundled `BundleProgram` plist.

## Identity and roles

Use **Add Signer Account** to save the public key and optional public label of
an existing external signer account. Use **Edit Label** to rename it without
changing either role. The picker offers compiled signer adapters: NIP-5F (local
socket), NIP-46 (remote), and NIP-55L (D-Bus only in builds that enable it).
Direct-key accounts are not offered for new managed roles.

Select a saved public account and choose **Use for Client** or **Use for Host**.
The signer must prove the selected key. FIPS hosting compares that key to the
local daemon's `show_status` npub; deselect the FIPS check only for direct-IP
hosting. Unassign roles without deleting accounts. A client change drains and
reloads Hosts under the new owner. Restart an already-running host to adopt its
new role.

Metadesk has no password. Unlock in the external signer, start a missing local
socket signer, or approve verification in the remote signer. Cancelling pending
verification prevents saving, but the backend call may need to drain. The existing
Keychain backend restores saved NIP-46 pairing without a prompt; unlock Keychain
externally if needed. Removing a signer account removes its assignments and any
Metadesk-held pairing, never the external signer's key. To restore an already
paired remote signer, select its saved public NIP-46 account and use **Import
Pairing**. Enter the single-line [pairing JSON](accounts.md#headless-cli) from a
trusted source in the masked field, without using the clipboard. The pairing
must match the selected public key. It replaces the account's Keychain pairing
without binding a role; role assignment still requires live verification.
Create and manage signing keys only in the external signer.

## Differences from the Linux Manager and remaining limits

- No relay discovery/authentication status panel. The Relays tab is an offline
  preview; edit bootstrap choices in Hosts.
- No Grotto Control1 status panel or separate Activity history; live sessions
  are in This Computer.
- No LaunchAgent registration or start/stop. No sharing/unattended setters exist
  in host IPC v1. macOS notification delivery has not been qualified.

## Signed-build qualification checklist

After `md-wdi3`, verify the packaged app and helper identity. Register the bundled
agent and observe login start and logout stop. Verify the privacy pane links,
deny/grant/recheck both TCC permissions on the responsible host, then exercise
actual capture and authorized input. With fixture identities first, verify native
Hosts review/edit/conflict sheets, HTTPS NIP-05, a real video launch and cancellation,
and that quitting leaves a connected client running. Exercise IPC restart/reconnect,
exact approvals and revocation, signed notification open and withdrawal, signer
lock and remote approval/cancel, Keychain ACLs and no fallback, FIPS match/mismatch,
and role switching. Save bootstrap choices, relaunch to check that they are
restored, and confirm clearing them leaves the address book offline without a
fallback relay. Display and scan the packaged app's host QR on another device,
compare the fingerprint, confirm the scan opens the normal trust review, and
verify that scanning a secret-key fixture refuses it without a camera prompt.
Add and relabel a public account with each supported signer backend. Import a
matching NIP-46 fixture pairing, refuse a mismatched pairing, and verify that
assigning a role still requires the signer to prove the account key.
The `macos_manager` suite drives the AppKit controllers off-screen with
fixture models: host trust and pin replacement, access defaults and stale
requests, wrong-key signer refusal, and quit while a worker is active. It uses
no synthetic input or permissions. The separate `account_keychain` test uses
an explicit private file Keychain, not the login Keychain or its search list.
Neither fixture qualifies the signed or on-screen workflows above.
