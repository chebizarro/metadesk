# Flatpak client and manager (md-lxxz)

`modules.json` uses Flatpak Builder's Git sources with full commit IDs for
dependencies without published release assets. Where a tag is also listed,
it must resolve to the pinned commit. SHA-256 archive pins are for published
release assets, not auto-generated Git snapshots.

Two independent application manifests, GNOME Platform/SDK 49:
`org.metadesk.Client.json` and `org.metadesk.Manager.json`. The manager consumes
merged md-fbg4 with `-Dmanager=true`; it does not embed or start a host daemon.

Dependencies are pinned in `modules.json`. nostrc
`6f7a8d025768d497bef700fe55eea1c687dac328` is installed to a private SDK
prefix during the build. The runtime SDK supplies GNOME/GLib, PipeWire,
AT-SPI headers, OpenSSL and system integration libraries. Own modules supply
SDL2, libyuv, cJSON, libb2, Jansson, secp256k1, nsync, libwebsockets and nostrc.
Legacy dependency CMake projects explicitly use the CMake 4 compatibility
floor `CMAKE_POLICY_VERSION_MINIMUM=3.5` and `/app/lib` as install libdir.
The FFmpeg module **consumes** `packaging/ffmpeg`'s minimal LGPL policy, not the
runtime's unverified full-codec extension. It enables H.264 decoder/parser and
NVENC/AMF/VAAPI; libvpl/QSV is x86_64-only. AMF/NV headers are build inputs, not
GPU binary runtimes. Cisco OpenH264 is never fetched or bundled. User-controlled
separate runtime acquisition/ABI validation remains the encoder loader's concern.

## Private user build

Install `flatpak-builder` if needed (Ubuntu: `sudo apt-get install flatpak-builder elfutils`).
Install GNOME runtime/SDK **as the user**, checking disk space first:

```sh
flatpak install --user --no-related flathub org.gnome.Platform//49 org.gnome.Sdk//49
sh /absolute/metadesk/packaging/flatpak/build.sh Client /absolute/new-client-work
sh /absolute/metadesk/packaging/flatpak/build.sh Manager /absolute/new-manager-work
flatpak run --user org.metadesk.Client --help
flatpak run --user org.metadesk.Manager --help
# Only in an unlocked, authorized GNOME session; close the window afterwards.
flatpak run --user org.metadesk.Manager
```

Builds use `-j2`, a private builder state/repository, `--user` installation,
explicit frontend targets and selective staging (never `meson install`, which
would install the host). Installation is a separate `flatpak install --user
--or-update --no-related --no-deps` transaction so existing GL/codecs extensions
are not silently updated. Frontend/probe launchers prepend `/app/lib` while
preserving driver paths: a preinstalled codecs-extra extension must not preempt
the private, verified LGPL FFmpeg. For a constrained builder, share a private
`--state-dir` across the two explicit flatpak-builder invocations to reuse common
modules and build sequentially. No automatic runtime installation or privileged
Flatpak install is hidden in the scripts. State/repo/app trees and downloaded
sources can consume several GB: remove task-owned build state after copying
artifacts, and uninstall **only refs created by the task** with
`flatpak uninstall --user --no-related` (otherwise cleanup can remove a
pre-existing related codecs extension), not blanket
`flatpak uninstall --unused` or global Podman pruning.

`finish-args.md` justifies every permission and describes deliberately unavailable
native socket/control features. The package includes matching desktop files,
unique CC0 SVG icons, MIT project metadata and CC0 AppStream metainfo. IDs match
the existing manager GtkApplication identity; domain ownership is an owner gate.

## Validation and Flathub owner checklist

```sh
appstreamcli validate --no-net assets/org.metadesk.Client.metainfo.xml
appstreamcli validate --no-net assets/org.metadesk.Manager.metainfo.xml
flatpak-builder --show-manifest org.metadesk.Client.json
# If installed, from each manifest/repository directory:
flatpak-builder-lint manifest org.metadesk.Client.json
flatpak-builder-lint repo /absolute/private/repo
```

Before submitting (nothing was submitted by W14):

- Replace each development `type: dir` metadesk source with an immutable release
  Git commit/archive+SHA256 including this packaging directory; retain pinned
  dependency sources. Resolve the local canonical FFmpeg/helper/test-fixture
  file sources in the standalone submission repository (or fetch them from the
  pinned release), rather than referencing a developer checkout. Local directory
  sources are intentionally not release pins.
- Establish control over the `org.metadesk` domain/IDs, or coordinate an upstream
  GtkApplication/desktop/account-storage identity migration to owner-controlled
  IDs. Packaging must not silently mismatch the manager's application ID.
- Update to the owner-supported GNOME runtime, run x86_64/ARM64 Flathub CI, both
  help checks and real GNOME + Hyprland client connection/decoder/signers tests.
- Upstream the client needs argument-free connection setup before advertising
  desktop-click launch: the current SDL client requires CLI host/npub arguments.
- Add real screenshots, accurate release history, verified developer identity,
  translations and store descriptions; do not fabricate screenshots/ratings.
- Run current Flathub manifest/repo lint and resolve every diagnostic. No
  exception request or submission is generated by these scripts. Review minimal
  permission requirements and native socket interoperability explicitly.
- Reconcile the installed app's private libraries against the reviewed pins
  (GNOME Platform is a separate download). Retain private module notices under
  `/app/share/licenses` and the per-bundle exact FFmpeg corresponding source and
  configure receipt. See [replacement instructions](../README.md#lgpl-library-replacement).
- Owner creates separate Flathub app repositories/submissions and arranges
  release maintenance/signing; Flathub signs published OSTree artifacts.

References: [manifests](https://docs.flatpak.org/en/latest/manifests.html),
[permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html),
[Flathub lint](https://docs.flathub.org/docs/for-app-authors/linter).
See `VALIDATION.md` for actual results versus outstanding gates.
