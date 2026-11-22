# Sandbox permissions

Both manifests intentionally omit a host, uinput, capture portals, host filesystem,
system/session bus sockets, AT-SPI bus access, audio, USB and `--device=all`.
The shared core links HAL libraries, but no host frontend is installed or launched.

| Argument | Reason |
| --- | --- |
| `--share=network` | Client remote desktop TCP and Nostr relays; manager remote signer/relay setup. |
| `--socket=wayland` | Native Wayland client/GTK windows. |
| `--socket=fallback-x11` | X11 only when Wayland is unavailable. |
| `--share=ipc` | SDL/X11 shared-memory rendering compatibility; not a bus grant. |
| `--device=dri` | SDL/GTK accelerated rendering; no input-device grant. |
| `--talk-name=org.freedesktop.secrets` | Existing libsecret account credential storage. |

App configuration is private under `~/.var/app/APP_ID`; no home-wide access.
The manager's application ID matches its existing GtkApplication ID.
NIP-5F arbitrary host socket paths and the native manager host-control IPC are
**not granted by default**. A user can opt in to a specific runtime directory
using `flatpak override --user --filesystem=xdg-run/EXACT_DIRECTORY APP_ID`,
after checking the signer/host's socket permissions. Cross-app account/config
sharing and NIP-55L are not implied by these permissions. Owner must qualify
and document a narrow stable signer/IPC interface before advertising those
features in the sandbox. NIP-46 is compiled in; no online signer success is
claimed by an offline help/decode smoke.

Permission baseline: <https://docs.flatpak.org/en/latest/sandbox-permissions.html>.
No broad permission is added just to turn a runtime smoke green.
