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
