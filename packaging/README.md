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

Cross-platform release automation (dry-run matrix, signing and publishing) is
tracked by `md-6mqr`.

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
