# Packaging

Distribution packaging for metadesk (`md-kb7`). Design, channel choices and the
rollout plan: [`docs/plans/packaging-2026-11-21.md`](../docs/plans/packaging-2026-11-21.md).

Producer rules follow the container runtime-bundle contract
([`deploy/containers/reference/bundle-contract.md`](../deploy/containers/reference/bundle-contract.md)):
base-owned vs privately bundled libraries, no producer rpaths, dlopen features
declared and retested, no keys/tokens/config in any artifact.

## Producers and release gates

Status reflects recorded qualification, not published releases.

| Channel | Producer and artifact | Verified status | Open release work |
|---|---|---|---|
| macOS | [`build-release.sh`](macos/build-release.sh) builds pinned dependencies and `Metadesk.app`; `MD_DMG=1` invokes [`make-dmg.sh`](macos/make-dmg.sh). | The [arm64 app](macos/README.md) passed closure and Launch Services startup checks with ad-hoc signing. All bundled Mach-O images target 13.0; runtime on macOS 13 itself was not tested. The default DMG is unsigned. | `md-wdi3`: Developer ID signing, notarization, stapling and host helper/TCC identity. `md-c17b`: Intel/universal2. `md-sit3`: Homebrew cask; `md-pjmz`: Sparkle updates. |
| Windows | [`build-arm64.sh`](windows/build-arm64.sh) and [`build-msi.ps1`](windows/build-msi.ps1) produce the WiX MSI; [`write-winget.ps1`](windows/write-winget.ps1) generates manifests. [Build guide](windows/README.md). | [Windows 11 ARM64](windows/VALIDATION.md): install, installed help, upgrade, downgrade rejection and uninstall passed. Winget schema validation passed; hosted install and trusted signing were not qualified. | `md-jsrk`: trusted signing and winget release; `md-sfxh`: native x64 qualification. |
| Linux native | [`build.sh`](linux/build.sh) and [`produce.py native`](linux/produce.py) stage the native tree; [`build-deb.sh`](linux/debian/build-deb.sh) and [`apt-repo.sh`](linux/debian/apt-repo.sh) produce the .deb and apt layout. The [RPM spec](linux/rpm/metadesk.spec) builds RPM/SRPM; the [AUR PKGBUILD](linux/aur/PKGBUILD) is a template. [Build guide](linux/README.md). | [Ubuntu 24.04 arm64 and Fedora 44 aarch64](linux/VALIDATION.md) passed clean-container install and host/client help. The apt layout is unsigned; Arch was syntax-checked only. | `md-xs90`: publishing/signing, remaining distro/arch validation and lint review; `md-wx68`: Omarchy/Hyprland validation. |
| Linux OCI | [`Containerfile.producer`](linux/Containerfile.producer) and [`produce.py oci`](linux/produce.py) emit the contract runtime tree for the agent OCI image; [`publish.py`](linux/publish.py) handles opt-in signing and publication by digest. [Build guide](linux/README.md#oci-producer-and-runtime-consumer). | [ARM64 production image](linux/VALIDATION.md#production-arm64-oci-result) passed local gates 1–4. Desktop gate 5 was not run; gate 6 was configuration-only. Loopback signing used a separate smoke image. | `md-x7wk`: hosted x86_64/aarch64 CI, live desktop gates and owner registry/signing. |
| Flatpak | [`build.sh`](flatpak/build.sh) exports client and manager `.flatpak` bundles; no host. [Build guide](flatpak/README.md). | [Local ARM64](flatpak/VALIDATION.md) builds, user installs, help, codec inventory and decode smoke passed. Manifest lint warned about the runtime; build/repo lint failed on missing screenshots. | `md-lxxz`: release source pins, app-ID ownership, screenshots, desktop qualification and Flathub submission. |
| AppImage | [`assemble.py`](appimage/assemble.py) stages the client AppDir; [`make-image.sh`](appimage/make-image.sh) produces the Type 2 AppImage. [Build guide](appimage/README.md). | [ARM64](appimage/VALIDATION.md): Ubuntu extract-and-run help and clean Fedora 44 help/decode passed. [`dry-sign.sh`](appimage/dry-sign.sh) demonstrated disposable GPG signing and zsync only. | `md-pqc7` is closed; no separate open AppImage release bead was identified. Hosted x86_64, remaining distro/desktop checks, legal/source review and production signing/update setup are outstanding. |

## GitHub pre-release workflow

`Pre-release packages` (`.github/workflows/release.yml`) runs after a `v*`
Git tag push, or manually once the workflow is on the default branch. Manual
runs always create a draft, never a Git tag. The default draft name is
`v0.1.0-alpha.1`; choose an unused version tag name. The workflow replaces
only its own previous draft with the same name, never a published release.
A tag push builds again from the tag commit; it cannot publish the assets
from a draft built at a different commit. Check the corresponding-source
attachments and review transitive licenses before pushing a release tag.

Linux installers are unsigned. An unsigned macOS DMG contains an ad-hoc
signed app. On a tag push only, a complete set of existing Apple credentials
opts into Developer ID signing using the normal app/DMG producers, followed by
notarization and stapling. Partial or absent credentials leave the app ad-hoc.
`APPLE_CERTIFICATE` and `APPLE_NOTARY_KEY` are base64 P12 and P8 respectively;
other inputs are `APPLE_CERTIFICATE_PASSWORD`, `APPLE_SIGN_IDENTITY`,
`APPLE_NOTARY_KEY_ID`, and `APPLE_NOTARY_ISSUER`.

Windows (`md-fjgl`, `md-sfxh`, `md-jsrk`) and Intel macOS (`md-c17b`) are
outside the required matrix. The Windows producer's existing opt-in
Authenticode path remains available for its separately qualified MSI.

### LGPL library replacement

Use the **matching asset's** source attachments and manifest to build
ABI-compatible replacements with the same architecture and SONAME. Preserve
the shared-library links, and test the modified build locally. Native Linux
packages use distro libraries rather than distributing copies.

- **AppImage, shared libraries:** extract with `./Metadesk-….AppImage
  --appimage-extract`. In the closure source tar, find the exact
  `ubuntu/<source>_<version>/*.dsc` named by the asset's library record.
  Keep all files beside the descriptor, run `dpkg-source -x FILE.dsc` and
  build with the matching Noble development dependencies (the included
  `debian/rules` and `debian/control` describe the build; e.g. use
  `dpkg-buildpackage -b -us -uc`). Extract the resulting binary package
  with `dpkg-deb -x` and copy the compatible DSO over the corresponding
  file under `squashfs-root/usr/lib`. For FFmpeg, instead use the separate
  FFmpeg source/build-receipt attachment. Run `squashfs-root/AppRun` directly.
- **AppImage, statically linked libfuse:** the closure tar's `runtime/` holds
  the pinned type-2 runtime program source (the complete work that links
  libfuse), libfuse 3.15.0 and squashfuse 0.5.2. Unpack the runtime and use its
  `BUILD.md` and `scripts/docker/build-with-docker.sh` with the matching
  `ARCH` on Alpine 3.21. Its `scripts/common/install-dependencies.sh`
  checks the dependency archives and applies `patches/libfuse/mount.c.diff`.
  To relink, change libfuse in that build, install the modified static archive,
  and rebuild the runtime with the same recipe. Use
  `packaging/appimage/make-image.sh APPDIR NEW.AppImage APPIMAGETOOL NEW_RUNTIME`
  to combine it with the extracted AppDir; this does not retain original
  signatures. The extracted `AppRun` also works without any FUSE runtime.
- **Flatpak:** unpack the release project source and edit a private copy of
  `packaging/flatpak/modules.json` to build the modified FFmpeg source with
  the matching configuration and local patch source. Build/export with
  `packaging/flatpak/build.sh Client|Manager NEW_WORK_DIR` and run that
  user-installed build. Do not modify the immutable OSTree object store.
- **DMG:** copy `Metadesk.app` to a writable directory, replace compatible
  codec dylibs in `Contents/Frameworks`, and retain the bundle-relative
  install names. Re-sign the modified dylibs, then the app, with
  `codesign --force --sign -`. Modified copies are not notarized.

The owner must check that the measured manifest maps every redistributed
library to its license and source where required, and verify all downloaded
attachments against `SHA256SUMS`. Review the actual tag commit; a draft at a
different SHA is not evidence for the tag. Do not publish an incomplete set.

## LGPL codec builds and notices

Metadesk is [MIT-licensed](../LICENSE). Bundled FFmpeg is minimal LGPL;
native Linux packages use system FFmpeg. License/codec policy: [third-party notices](../THIRD_PARTY_NOTICES.md),
[per-artifact flags](../docs/plans/packaging-2026-11-21.md#71-ffmpeg-configure-policy-per-artifact),
[release-notes template](RELEASE_NOTES_TEMPLATE.md).

```sh
# Fetch/verify an explicitly pinned upstream source archive separately.
# Both output directories must be new and outside the source worktree.
MACOSX_DEPLOYMENT_TARGET=13.0 ./packaging/ffmpeg/build-minimal-lgpl.sh \
    macos /absolute/ffmpeg-source /private/tmp/my-minimal-ffmpeg
MD_FFMPEG_PREFIX=/private/tmp/my-minimal-ffmpeg/install \
    ./packaging/macos/build-app.sh /absolute/nostrc /absolute/nostrc-build /private/tmp/my-app
```

The FFmpeg script never downloads sources, modifies its input tree, or accepts
arbitrary configure arguments. `windows`, `container` and `flatpak` profiles
require the headers/SDKs/dispatchers listed in the plan; native distro packages
use system FFmpeg and do not run this script. Configure arguments and logs
are retained in its output root; build/config files remain in `build/`.
Archive the exact source, patches and these records with each release.
The Windows profile applies `ffmpeg/patches/0001-mf-low-latency-vt-bool.patch`
to a private `OUTPUT/source` copy; it retains `OUTPUT/patches/` and
`OUTPUT/patches.txt`. Patch drift or an already-applied fix fails closed before
configure (never reverse-applied); review the patch when updating the FFmpeg pin.
The caller's source is unchanged. Windows also enables D3D11VA for MF SDK types.

`MD_FFMPEG_PREFIX` selects avcodec/avutil only; use
[`build-release.sh`](macos/build-release.sh) for the full source-pinned macOS
closure and 13.0 floor check. OpenH264 is runtime-loaded, not bundled.

Runtime codec inventory gate (use the profile's full encoder list):

```sh
export PKG_CONFIG_PATH=/private/tmp/my-minimal-ffmpeg/install/lib/pkgconfig
cc -std=c17 -Wall -Wextra -Werror $(pkg-config --cflags libavcodec libavutil) \
    packaging/ffmpeg/verify-codecs.c $(pkg-config --libs libavcodec libavutil) \
    -Wl,-rpath,/private/tmp/my-minimal-ffmpeg/install/lib -o /private/tmp/verify-codecs
/private/tmp/verify-codecs h264_videotoolbox
```

This native-library probe checks the license of avcodec/avutil, enumerates
codecs/parsers, rejects extras, and requires every requested encoder. It does
not promise hardware encoder availability or exercise the runtime driver.
