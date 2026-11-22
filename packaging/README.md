# Packaging

Distribution packaging for metadesk (`md-kb7`). Design, channel choices and the
rollout plan: [`docs/plans/packaging-2026-11-21.md`](../docs/plans/packaging-2026-11-21.md).

Producer rules follow the container runtime-bundle contract
([`deploy/containers/reference/bundle-contract.md`](../deploy/containers/reference/bundle-contract.md)):
base-owned vs privately bundled libraries, no producer rpaths, dlopen features
declared and retested, no keys/tokens/config in any artifact.

## macOS prototype (`macos/`)

Builds a self-contained, ad-hoc-signed `Metadesk.app` (client as the bundle
executable, host alongside it) and optionally a `.dmg`.

```sh
# nostrc build with the libraries metadesk links (see docs/build-signers.md);
# it must be outside this source tree.
MD_DMG=1 packaging/macos/build-app.sh /path/to/nostrc /path/to/nostrc-build /tmp/md-out

# re-verify any bundle; MD_MACOS_MIN enforces a release floor
MD_MACOS_MIN=13.0 packaging/macos/verify-app.sh /tmp/md-out/Metadesk.app
```

| Script | Role |
|---|---|
| `build-app.sh` | meson release build → `meson install --destdir` → bundle → verify → DMG → `SHA256SUMS` |
| `bundle_app.py` | Mach-O closure walk, copy to `Contents/Frameworks`, rewrite install names, replace rpaths, `Info.plist`, `Resources/metadesk-bundle.json`, inside-out codesign |
| `verify-app.sh` | static closure/rpath/codesign gates; clean-environment runs recording every image dyld loads (covers dlopen) |
| `make-dmg.sh` | compressed DMG with an `/Applications` link (signs it when `MD_CODESIGN_IDENTITY` is set) |

`MD_CODESIGN_IDENTITY` selects a Developer ID identity (default `-`, ad-hoc);
a non-ad-hoc identity enables the hardened runtime. Notarization is not
scripted yet (`md-wdi3`).

**Prototype limits:** it bundles whatever the producer has installed. With
Homebrew that means GPL FFmpeg codecs and dylibs whose minimum macOS is the
builder's OS, so the result is a developer artifact, not a release
(`md-ouw3`, `md-2exc`).

Other platforms are documented in the plan and tracked as follow-up beads.

## LGPL codec builds and notices

License/codec policy: [third-party notices](../THIRD_PARTY_NOTICES.md),
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


`MD_FFMPEG_PREFIX` only selects avcodec/avutil for the macOS producer; it does
not validate the rest of the app closure or install legal files. Until
`md-ouw3` rebuilds/pins the other dependencies, fixes OS floors, stages legal
texts and enforces the release closure, this remains a developer prototype.
OpenH264 is never included in this build or bundle.

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
