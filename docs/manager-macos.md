# macOS Manager

Metadesk Manager is an optional AppKit companion. Its sidebar has five sections:
**Hosts** syncs and connects; **This Computer** and **Access** manage a running
local host; **Identity** manages saved role assignments; **Relays** is an
offline route preview. The manager does not start a host or enable sharing.
Quitting it does not stop a host.

## Window, menus and editing

The window remembers its size and position (only its frame; page content is
never restored). Use the sidebar or View > Hosts … Relays (Command-1 to
Command-5) to move between sections; View > Refresh (Command-R) or the toolbar's
Refresh syncs Hosts, rechecks permissions in This Computer, reloads remembered
clients in Access and reloads accounts in Identity. Edit > Find > Find Hosts
(Command-F) searches saved hosts. File has Connect to Host (Command-K), Scan QR
Code, Import Hosts and Export Hosts.

Cut, Copy, Paste, Delete, Select All, Undo and Redo work in every field and in
the contextual menu. With a saved host selected, Edit > Copy copies its public
npub. The manager writes to the pasteboard only when you choose Copy, Cut or
Copy Public Key. Fields that take a key, fingerprint, name, address or path,
including Search Hosts, accept text literally: no smart spaces, substitutions,
correction, case changes or Writing Tools, and the contextual menu doesn't offer
them, or Look Up, Translate, Search With Google or Share. They refuse any text
containing a private key (`nsec`, `ncryptsec`, also after `nostr:`) before it
is entered, however it arrives — typing, Paste, drag and drop or a Service — so
it is never shown, stored or recoverable with Undo or Redo; the page says why.

One field is deliberately typed only: **Confirm** in the changed-key sheet
refuses Paste, drag and drop and Services, and the new fingerprint above it
can't be selected. Typing it yourself, as the host's owner reads it to you, is
the check; copying it from the same sheet would prove nothing.

Sheets put the safe choice on Return and Cancel on Escape; destructive buttons
are never the default. Trust decisions have no Return default at all: reviewing
a new host, a changed host key and approving a request must be clicked.

## Install from the macOS DMG

Copy `Metadesk Manager.app` out of the arm64 DMG. It includes its own video
client and private libraries; Connect does not need `metadesk-client` on
`PATH` or `Metadesk.app` alongside it. For manual local hosting, use
`Metadesk.app/Contents/MacOS/metadesk-host` or your installed CLI on `PATH`.
The Manager is ad-hoc signed like `Metadesk.app` in the default unsigned DMG.
Expect each downloaded app to require its own
[first-launch approval](https://support.apple.com/en-us/102445). With ad-hoc
signing, expect Keychain items to be tied to the exact Manager build that
created them. After an update, macOS may prompt or refuse access, and pairings
may need to be re-imported. These are expected behaviors, not tested results.

Coverage is limited to fixture tests and an offline smoke of the packaged
`dev.metadesk.Manager` executable. The smoke opens the real AppKit window and
quits normally without exercising connections, signers, Keychain, host IPC,
or user data. Signed on-screen qualification is still open (`md-8xlg`, `md-wdi3`).

## Build and run from source

With the normal macOS dependencies and a built, pinned nostrc tree:

```sh
meson setup build-mac-manager -Dmacos_manager=true -Dclient=true \
  -Dsigner_nip5f=true -Dsigner_nip46=true \
  -Dnostrc_root="$NOSTRC" -Dnostrc_build_dir="$NOSTRC/_build"
meson compile -C build-mac-manager -j2
meson test -C build-mac-manager --suite macos_manager --print-errorlogs
PATH="$PWD/build-mac-manager:$PATH" \
  build-mac-manager/src/macos_manager/metadesk-macos-manager
```

The app targets macOS 13 or later; development dependencies may require a
newer version. The bundle uses build-tree libraries. It is not a signed release
and cannot qualify the packaged host's permission identity. An app bundle
requires an executable `metadesk-client` inside `Contents/MacOS` or
`Contents/Helpers`; if neither works, Connect fails without looking at `PATH`.
Only unbundled development runs search beside the executable and then `PATH`. GLib is not linked by the Manager. If `gio-2.0` is
available when configuring, the suite also runs the GTK store against the
same relay vectors and exchanges real files between both implementations.

## Hosts

1. Assign a client role in **Identity**, then open **Hosts** to load its encrypted
   cache. Until then Hosts shows what's missing and the next step: add a signer
   account, assign the client role, unlock the signer, re-import a remote
   signer pairing, or free the private cache. Relay setup lives in the
   **Relays** section (Hosts links to it): enter explicit bootstrap relay URLs
   separated by spaces and choose **Save and Sync**. The Relays section shows
   the saved list each time it appears, whether or not Hosts has been opened,
   and **Save and Sync** stays unavailable until it has loaded. Save writes
   exactly the validated list shown — an empty list is saved and nothing
   connects — and its confirmations and results appear in the Relays
   section. View > Refresh is separate: it syncs the saved list and never
   writes or offers to replace the file. Without relays, or while they are unreachable or catching up,
   Hosts shows cached hosts and explains why reviewing and connecting wait. Both managers use the same
   per-user `$XDG_CONFIG_HOME/metadesk/manager-relays.ini` file (default
   `~/.config/metadesk`). GTK accepts international relay hostnames and writes
ASCII forms. macOS accepts only ASCII hostnames on input and on load; for an
international hostname, enter its `xn--` punycode form. macOS does not perform
IDNA conversion. GTK's saved ASCII URLs can still be read by both. The app
loads the selection on entry but does not sync
   until you refresh. A client-role change, made in Identity or by another
   program, closes and reopens Hosts for the new key; a label edit doesn't. Replacing unreadable
preferences requires confirmation. Clear the field and save to remove all choices; none
   is preselected or used as a fallback. `ws://` is unencrypted.
2. Enter an `npub`, `nprofile`, lowercase hex public key, or NIP-05 name in
   **Host**. Enter an IPv4 or IPv6 address only in **Route hint**. **Review and
   Connect…** opens **Verify This Host**: check the fingerprint, full npub,
   NIP-05 status and requested access, then choose **Connect** or **Save
   Host**. A changed NIP-05 pin blocks connection: choose **Keep Old Key**, or
   verify the new key with the host and type its exact fingerprint to enable
   **Replace Key**. Review again before connecting. **Scan QR Code…** reads one QR code from
   a local image, accepts only a valid public `nprofile`, and sends it through the same review as typed text.
   `nsec` and `ncryptsec` are cleared and refused, including when scanned. No
   camera or camera permission is used.
3. Search saved hosts from the toolbar, filter by tag, or switch the list to
   **Quick Connect** for favorites followed by local recent attempts. Sort by
   any column. Select a host and choose **Connect…** (or double-click it or
   press Return); **Edit…** changes its label, notes, favorite and tags, and
   removes it. Last-connected times are not synced. A new client key does
   not inherit the old key's grants; request access again from host admins.
4. Import `metadesk-hosts/1` files (File > Import Hosts) by checking only hosts
   whose fingerprints you recognize; the exact requested capabilities are shown. Existing pins are not overwritten. The explicit
   plaintext export contains public keys, labels and notes, not credentials or grants.

Cached hosts remain viewable without a relay if the signer is available. Connect
requires catch-up on connected fetch routes; edits require all required fetch
routes. Resolve conflicts from the sync-issue row by reviewing both versions,
then choosing **Keep Newer Version** or **Reapply My Edit**. Unknown schema and unreadable data are not treated as an
empty book. Separate collections aren't atomic; review and retry any pending
collections after partial acknowledgement.

The separate video client reports launch phases and typed relay AUTH, CLOSED,
and publication rejection outcomes. **Cancel Connection** stops an unfinished
launch; **Disconnect** ends a connected session. Quitting the manager cancels
an unfinished launch and leaves a connected video window running.

## This Computer and Access

**Show Host QR Code** displays the saved host role's public `nprofile` and
fingerprint. It does not contact the signer or check whether the host is running.

When you open This Computer or Access, the manager connects to the saved host
role's standard [management socket](host-management.md#endpoint-and-peer-trust)
automatically, and again whenever the host role is assigned, switched or
removed in Identity (removing it disconnects). On a host-role change the old
connection, its pending requests, remembered clients and any open approval
or confirmation sheet are discarded before the new socket is looked up.
**Use a custom management socket** takes an absolute path instead; it is an
explicit choice and works with or without a host role, over the same
authenticated same-user connection, so a role change leaves it connected.
Clearing the field and choosing **Connect** returns to the standard socket,
looked up again for the current host role.
Status comes from an authenticated live subscription. Choose **Try Again** (or
View > Refresh) after a host restart or loss of the socket. Without a host role,
the page says so and links to Identity.
A reachable managed process is not proof of LaunchAgent registration, permission,
or capture readiness. If unreachable, the agent's process state is unknown.
**Disconnect Session** ends the slot without changing remembered access; an
occupied slot is not proof of a bound stream.

**Recheck Permissions** asks the authenticated host to report Screen Recording
and Accessibility preflights. **Unknown** means no trusted report. Open the
matching System Settings pane and grant access to the responsible host identity,
not the manager. These buttons don't request permissions or enable sharing.

In **Access**, select a pending request and choose **Review Request…**. Check the
request's public key, fingerprint, requested permissions and remaining time. Only
requested and supported screen viewing is selected initially. Choose **Approve
Once**, **Approve and Remember** (exactly the checked permissions) or **Deny**.
Expired, cancelled, or stale reviews cannot approve. A notification opens the
review of exactly the request it announced, or nothing if that request has
gone. Remembered clients load when the host connects; View > Refresh reloads
them before editing with **Edit Access…**. **Revoke…**
removes the grant and ends that client's admission. **Disconnect and Revoke…**
uses the same host operation for the selected active client. A failed reply does not prove the grant is gone.

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

Use the **+** button under the accounts list (Add Signer Account) to save the
public key and optional public label of an existing external signer account.
Use **Edit Label…** to rename it without
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
Pairing**. Choose a trusted [pairing JSON](accounts.md#headless-cli) file (regular
file, at most 64 KiB; symbolic links are refused). Metadesk reads it on a worker,
wipes its read buffer after use, and does not modify or delete the source file.
Protect that file as a credential. The pairing
must match the selected public key. It replaces the account's Keychain pairing
without binding a role; role assignment still requires live verification.
Create and manage signing keys only in the external signer.

## Differences from the Linux Manager and remaining limits

- No relay discovery/authentication status panel. The Relays section holds the
  bootstrap relay choices and an offline identity preview.
- No Grotto Control1 status panel or separate activity history; live sessions
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
Add and relabel a public account with each supported signer backend.

Menus and editing, by hand with the keyboard and the menu bar: check that the
menu bar shows the application, File, Edit, View, Window and Help menus. In the
Host field, paste an npub with Command-V, select it with Command-A, copy and
cut it with Command-C and Command-X, undo and redo with Command-Z and
Shift-Command-Z, and use the same commands from the contextual menu. Type,
paste, drag in and insert with a Service an `nsec` fixture into Host, Route
hint, Relays > Host identity, Add Signer Account's public key and the
changed-key Confirm field: each refuses it, shows no part of it, and neither
Undo nor Redo restores it. Confirm that the changed-key Confirm field refuses
pasting its fingerprint and accepts it typed. Confirm the contextual menu of a
key field offers no Substitutions or Transformations.
Select a saved host and confirm Edit > Copy copies its npub; confirm nothing
reaches the pasteboard otherwise. Check Command-1 to Command-5, Command-R,
Command-F, Command-K, Command-W, Command-M and Command-Q, Hide and Show All,
Bring All to Front, and that items are disabled when they do not apply (for
example Export Hosts before the address book opens). Tab through every section
with Full Keyboard Access on; confirm Return and Escape in each sheet (no Return
default in Verify This Host, the changed-key sheet or Allow This Client to
Connect), VoiceOver labels on the sidebar, tables, image buttons and status
lines, Dark Mode and Increase Contrast, and that the window reopens at the size
and position it was closed at. Import a
matching NIP-46 fixture pairing, refuse a mismatched pairing, and verify that
assigning a role still requires the signer to prove the account key.
The `macos_manager` suite drives the AppKit controllers off-screen with
fixture models: the menu bar's items, actions and key equivalents; editing in
the connect field through the Edit menu with private pasteboards (never the
general pasteboard), including refusing a pasted `nsec`; host trust and pin
replacement and the trust sheets' button roles; access defaults and stale
requests; wrong-key signer refusal; and quit while a worker is active.
`meson compile -C BUILD macos-manager-snapshots` renders every section, sheet
and first-run state offscreen into a fresh `BUILD/snapshots/run-XXXXXX/out`
for design review; it refuses any root but a canonical meson build directory
owned by you, and keeps its home, configuration, state and runtime
directories inside that run. It opens the root one component at a time
without following links, validates the Meson marker and creates the run
directories through those descriptors, and refuses any path that would be
truncated. Paths it hands to Foundation and AppKit are re-resolved by them;
the run directory's 0700 mode is what protects those. The suite's renderer
guard checks refusals (including an overlong root and a symlinked ancestor),
performs one isolated render and checks every artifact; without a window
server it reports the test as skipped. It uses
no synthetic input or permissions. The separate `account_keychain` test uses
an explicit private file Keychain and checks it never appears in the search list.
Neither fixture qualifies the signed or on-screen workflows above.
