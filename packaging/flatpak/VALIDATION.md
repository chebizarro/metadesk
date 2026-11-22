# W14 Flatpak qualification — 2026-11-22

Worktree `md/w14-appimage-flatpak`, base
`e0abbc06555f174acc5c2fe28622cf7cff14832d`.
Only `packaging/appimage/`, `packaging/flatpak/` and documentation changed.
Neither bead is closed; no push, merge, Flathub submission or public release.

## Real ARM64 artifacts

Built sequentially on Ubuntu 24.04 aarch64 using **flatpak-builder 1.4.2**,
GNOME Platform/SDK **49**, GCC **15.2.0**, CMake **4.4.3**, Meson **1.9.2**,
`--user` installation and **two build jobs**. No host frontend is staged.
Runtime commit `c3daeced6f88f9c4ffc4f88c798c48fffd395d873fc0d36b7fc73b753d7f59c9`;
SDK `fff4a1de0b4cef7dc8163dcad095aac1fe076639a097de0b945687e8cb4b3806`.
The source is a development directory export, not a published release pin.
Pinned nostrc: `cfcb86296068c1e4090bc5ce8a486392dc74eb11`.

| Local unsigned bundle (ignored `output/`) | SHA256 |
| --- | --- |
| `org.metadesk.Client.flatpak` | `70f0d0f602f09f96bca006f98391f5b09c1e5d21374dd6a9904c614d16d7b6c1` |
| `org.metadesk.Manager.flatpak` | `f39475846d9f0bd7e239baf617ae64dc41809d41e01aa2719709a90dff869083` |

Client app commit:
`c37285ab65e58f714b6bdbbebd03514188d626db34eb0cc6c67143e618e1545c`.
Manager app commit:
`ba44fbfcbb30894fb03bd815f3dfe064638d8ef77b5b3a7afdce13079b01fc13`.
Local branch is `master`; these are not Flathub-signed stable artifacts.

Both actual installed apps:

- **Build/export/install PASS**; `flatpak run --user APP_ID --help` **PASS**.
- Actual packaged inventory probe **PASS**: LGPL-2.1-or-later, exactly one H.264
  decoder/parser, **h264_nvenc, h264_amf, h264_vaapi**, no QSV on ARM64.
- Packaged decoder smoke **PASS**: the shipped shared core decodes the synthetic
  Annex-B input through FFmpeg/libyuv into exactly one **16x16 RGBA frame**.
- Own minimal FFmpeg 8.1.2 module, not a full-codec runtime substitution.
  No bundled OpenH264, x264 or x265. NV/AMF headers are build inputs only.
- Tests did not invoke host services, uinput, capture or accessibility
  operations, live relay/signers or physical GPU encoders. Standard declared
  Flatpak display permissions were present; no extra host bind mounts were used.

Manager window: **NOT RUN — GNOME Wayland session 1493 reported
`LockedHint=yes` both before and after builds**. No unlock attempt or wait;
no window was opened, so none was left behind. GNOME/Hyprland client rendering,
real remote connection/signers, x86_64 and hosted Flathub CI remain **NOT RUN**.

## Root-cause corrections found by real builds

- Codeload URLs need explicit archive types; checksums alone do not tell this
  builder how to extract tag/commit URLs without a filename suffix.
- Old dependency CMake policy floors require the documented CMake 4 external
  compatibility minimum; native SDK installs explicitly target `/app/lib`.
  SDL requires an out-of-tree build. Host `elfutils` is needed for builder's
  debug splitting, rather than disabling stripping/debug verification.
- The simple nostrc module needs `CMAKE_PREFIX_PATH=/app`. The application's
  Meson `find_library` probes need the compiler's `LIBRARY_PATH=/app/lib`;
  a sandbox diagnostic confirmed this, not just a guessed timeout/retry.
- Meson linked the core to private avcodec **62** correctly, but the original
  direct C inventory probe selected SDK codecs-extra avcodec **61** and failed
  the LGPL gate. Probe compilation now prepends the private pkg-config path,
  keeps explicit include/library flags and runs the exact inventory immediately.
  Runtime launchers scope `/app/lib` first while retaining driver paths; both
  installed probes now pass with private LGPL libraries. The earlier GPL result
  is a real caught qualification failure, not counted as a pass.
- Builder `--install` unexpectedly updated three existing shared extensions.
  Installation is now separate with **`--no-related --no-deps`**, and all three
  pre-existing commits were restored from the recorded Flatpak history and
  independently checked with `flatpak info --show-commit`. Runtime uninstall
also automatically removed the pre-existing codecs-extra ref; it was explicitly
reinstalled and returned to the original commit. Cleanup instructions now
require **`uninstall --no-related`** as well. Final checks compare both the
original ref set and all three commit identities, not just app absence:

| Existing ref | Restored original commit |
| --- | --- |
| GL.default / 25.08 | `ae808301ba0e5d8366326d9e80594f75600e8c6deee5f301c66e794998bfb4af` |
| GL.default / 25.08-extra | `331f0e5700c561a46dca8812693956a4a03218ab3758d444a690359fc8f60142` |
| codecs-extra / 25.08-extra | `3b594ed9ba51cb7c7003535a8d8c0c1b38ecd147b93546b312f26e2bf153d305` |

No product assertions, protocol behavior, failure watchdogs or sandbox grants
were relaxed. No application-source patch was used to make packaging pass.

## Validation tools and owner gates

- `appstreamcli validate --no-net` **PASS** for both metainfo files.
  Pedantic mode reports exactly **one non-error per app**:
  `cid-contains-uppercase-letter`, matching the existing manager application ID.
- Both manifests resolve with `flatpak-builder --show-manifest` **PASS**.
- Six packaging policy/wiring tests **PASS** on macOS and Ubuntu; shell syntax
  and ShellCheck **PASS** for all new scripts. No blanket lint suppression.
- Oracle reviewed supplied script/wiring excerpts only after snapshot routing
  failed, not the full worktree; no actionable finding, not release approval.

Owner prerequisites remain the README checklist: immutable metadesk release
sources, domain/application identity control, real screenshots and store copy,
argument-free client connection UI, GNOME/Hyprland and architecture/hosted CI
matrices, narrowly qualified native signer/control socket interoperability, and
complete dependency license/source/replacement-rights material. No Flathub
submission or exception request was made.

## Flathub lint — executed after freeing SDK space

Used the official ARM64 `flathub/flatpak-builder-lint` OCI manifest
`sha256:9a84355018b712113d66e912b1372220dbc0871970880a9dabe675802a9aad71`,
config `4bbe004595544e3f4d74d7dee5e3d1bec528e5ab37c463d14c8e7ae65241a9af`.
The optional pull was initially stopped to prioritize the SDK/disk budget; after
SDK removal the image was completed/loaded from SHA256-verified cached OCI blobs.
No linter exception, error suppression or invented screenshot was used.

| Check | Exit | Exact result |
| --- | --- | --- |
| Client manifest | 0 | No errors; `runtime-update-available-to-org.gnome.Platform-51` warning |
| Manager manifest | 0 | Same warning, no errors |
| Client build directory | 1 | `metainfo-missing-screenshots`; same runtime warning |
| Manager build directory | 1 | `metainfo-missing-screenshots`; same runtime warning |
| Combined private repository | 1 | `appstream-screenshots-not-mirrored-in-ostree`, `metainfo-missing-screenshots`; same runtime warning |

This is **not lint-clean or submission-ready**. Real screenshots need an unlocked
desktop and owner review; Flathub publication must mirror them into AppStream.
GNOME 49 is the measured candidate, not the latest 51 runtime. Rebase/requalify
against the owner-selected current runtime before submission. The first container
manifest-lint invocation could not create its state directory at `/`; setting a
private writable `/tmp` working directory fixed the harness, not the app checks.

Tool context: Jev `find-lines` narrowed Meson dependency discovery and the pinned
nostrc build flags (presence 0.85 each, 7 requests, 50,509 input tokens,
about USD 0.00212); load-bearing findings were checked directly. No threshold
misfire was used to approve an edit or qualification claim.

## Final cleanup receipt

- Disk free: **8.6 GB before**, lowest observed **1.9 GB**, **8.3 GB after**.
- Both bundles and the AppImage copied out and SHA256 checked in the assigned
  worktree's ignored `output/` directories before deletion of VM resources.
- W14 apps/data, GNOME Platform/Locale/SDK 49, builder state, private OSTree repo,
  source/build trees, linter OCI staging/archive and **`~/md-w14` absent**.
- Original five visible Flatpak refs restored exactly; all three original shared
  extension commit IDs above reverified after the final codecs restoration.
  Only the pre-existing `flathub` remote remains. No global unused-ref prune.
- **Zero W14 Podman containers/images/volumes/networks**; both fetched images and
  their known task-owned temporary blob directories removed individually.
  The pre-existing Ubuntu image `50ec65424811` remains. No global Podman prune.
- W14 app-data directories and builder/GPG/Podman processes absent; temporary
  signing private home/key deleted. No manager window was opened or left behind.
- **`/private/tmp/md-w14-*` absent**, independently checked after local cleanup.
- Apt additions retained and recorded, not a byte-for-byte VM rollback:
  `flatpak-builder 1.4.2-1build2`, `elfutils 0.190-1.1ubuntu0.1`,
  `zsync 0.6.2-5build1`, `libva-dev 2.20.0-2ubuntu0.2` and new dependencies
  `appstream-compose`, `gir1.2-flatpak-1.0`, `libappstream-compose0`, `libflatpak0`,
  `libset-scalar-perl`, `libva-glx2`, `ostree`, `libasm1t64`.
  No existing apt package was upgraded or removed. Patchelf was already present.

Neither local help/decode evidence nor the cleanup receipt is a substitute for
live desktop, hosted CI, hardware, source-offer/legal or Flathub acceptance.
