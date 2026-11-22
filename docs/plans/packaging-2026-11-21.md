# Cross-platform distribution packaging — survey, prototype, recommendation

**Spike:** `md-kb7` · **Consumes:** `md-ah9.13` (runtime-bundle contract, `deploy/containers/reference/bundle-contract.md`) · **Coordinates with:** `md-70v` (toolkit/UX), `md-5zi` (per-platform E2E), `md-pw1q` (host lifecycle)

**Deliverable:** this document, the macOS prototype under `packaging/macos/`, and the follow-up beads in §12. The bead asked for `docs/designs/`; this repo keeps spike deliverables in `docs/plans/` (same decision as `md-70v`, see `docs/plans/management-ux-2026-11-14.md` §7.2).

## 1. Recommendation (summary)

1. **One producer, many wrappers.** CI builds a *relocatable runtime tree* per OS/arch from a tagged commit: `bin/` + private `lib/` closure + manifest + `SHA256SUMS`. It uses the **same rules as the container bundle contract** (base-owned vs privately bundled, no producer rpaths, explicit dlopen list, no secrets). Every channel (DMG, MSI, deb/rpm/AUR, Flatpak, OCI) wraps that tree. Nothing is compiled on the user's machine.
2. **Channels, in rollout order:**
   - **macOS:** Developer ID–signed and notarized `.dmg`, then a Homebrew **cask** (own tap first).
   - **Linux host:** native packages (**AUR `metadesk-bin`** for Arch/Omarchy, **.deb** apt repo, **.rpm** via Copr), plus the **OCI image** from the contract.
   - **Linux client/manager:** **Flatpak on Flathub**.
   - **Windows:** signed **MSI** + **winget**.
   - **Later:** AppImage, a portable client only.
3. **Vendor what the ABI story can't guarantee, use the system for what distros do well.**
   - Always bundle on macOS/Windows.
   - On Linux native packages, use distro FFmpeg/SDL2/PipeWire/OpenSSL/GLib.
   - Link nostrc statically from a **pinned nostrc revision** (the build already links nostrc almost entirely as static archives; §3).
4. **Ship our own minimal LGPL FFmpeg** (H.264 only) wherever we bundle it. Homebrew/most distro FFmpeg builds are GPL and drag in ~15 codec libraries metadesk never uses (§3). Whether a GPL `libx264` fallback ships, and in which artifacts, is an explicit open decision (§13).
5. **Updates follow the channel:**
   - Package managers update everything except the DMG.
   - The DMG gets **Sparkle 2** (EdDSA-signed appcast).
   - Release events are additionally announced as signed Nostr **NIP-94** file-metadata events, so Nostr-native clients can *subscribe* for updates rather than poll (§8).
6. **FIPS is not bundled.** Packages *depend on* or *recommend* the separately packaged `fipsd` (nostrc already ships deb/rpm/Arch/launchd/systemd packaging for it; §9).

## 2. Scope and products

| Product | What it is | Ships on |
|---|---|---|
| `metadesk-host` | CLI/daemon: capture, encode, input, a11y, MCP, signaling | macOS, Linux, Windows (L2), OCI |
| `metadesk-client` | SDL2 + Dear ImGui video client | macOS, Linux, Windows |
| `libmetadesk` | shared core (both binaries link it) | inside every artifact; not a public SDK yet |
| `metadesk-manager` | **future** GTK4/libadwaita companion (`md-70v` P4.10, `md-fbg4`) | Linux only (Flatpak-first); macOS gets a native frontend later (`md-pep3`) |

The `md-70v` toolkit choice matters for packaging in three ways:
- GTK stays **out** of host/client/core builds, so the macOS/Windows bundles never carry GTK.
- The manager is Linux-only and maps naturally to the GNOME Flatpak runtime.
- The overlay remains SDL2/ImGui.

## 3. Current state — measured, not assumed

Evidence from the macOS prototype run on 2026-11-21:
- **Machine:** macOS 27.0.1 arm64, Homebrew dependencies.
- **Source:** metadesk `36119390a0a9`, nostrc `ea5ed954267c` (private Release build of the documented target set, `docs/build-signers.md`).
- **Options:** `-Dclient=true -Dsigner_nip5f=true -Dsigner_nip46=true`.

| Finding | Evidence | Consequence |
|---|---|---|
| Source-only today; no CI in metadesk (`.github/` absent) | repo tree | the release pipeline is greenfield; nostrc's `release.yml` / `fips-packaging.yml` are the house precedent |
| nostrc is effectively **statically** linked | `otool -L` of `libmetadesk.0.dylib`: the only nostrc dylib is `libnostr-json.2.dylib`; libnostr, libgo, nip* come in as `.a`/objects | no runtime nostrc packaging problem, only a *build* pin; `libnostr-json` is the one shared nostrc library to bundle |
| nostrc NIP components are not installable | no `install(TARGETS …)` in `nips/nip51`, `nip17`, `nip11`, `nip98`, `nip46` CMakeLists | consumers must build nostrc from a pinned tree (vendored/static) until nostrc gains install rules (`md-kb7` follow-up) |
| A stale nostrc build silently breaks the link | shared `nostrc/_build` (Nov 17) lacks the NIP-42 AUTH symbols that master needs | CI must build nostrc at a **pinned revision** recorded in the manifest, never "whatever is in `_build`" |
| `meson install` output does not run in place on macOS | installed `libmetadesk.0.dylib` keeps `@rpath/libyuv.dylib` while install strips all rpaths | the bundler resolves through build-tree rpaths; the meson install bug is filed separately |
| Homebrew closure = **30 private dylibs, 41 MB `.app`, 19 MB DMG** | prototype manifest | includes `x264`, `x265` (GPL), `dav1d`, `SvtAv1Enc`, `vpx`, `opus`, `mp3lame`, `mpg123`, `sodium`, `glib`/`gobject`/`intl`/`pcre2`, … |
| metadesk needs **H.264 only** | `src/core/encode.c`/`decode.c`: `h264_videotoolbox`, `h264_nvenc`, `h264_amf`, `libx264` fallback; native H.264 decoder | a minimal LGPL FFmpeg (`--disable-everything` + H.264 decoder + HW encoders) removes most of the closure and the GPL question |
| Homebrew bottles carry the **builder's OS** as minimum | `LC_BUILD_VERSION minos 27.2` on `libwebsockets`, `SDL3`, `SvtAv1Enc` | the bundle's honest `LSMinimumSystemVersion` is 27.2; **Launch Services refuses to open it on this 27.0.1 machine (`-10825`)**. Release dependencies must be built with `MACOSX_DEPLOYMENT_TARGET=13.0` (ScreenCaptureKit floor) |
| `libSDL2` is **sdl2-compat**, which `dlopen`s SDL3 | `strings libSDL2-2.0.0.dylib` → `@loader_path/libSDL3.dylib` | the linker view misses a required runtime library; exactly the contract's "enumerate dlopen features" rule |
| OpenSSL comes from `openssl@4` via libwebsockets | closure | one TLS stack only; bundle it (macOS/Windows) or use the system one (Linux packages) |

## 4. Consuming the runtime-bundle contract (`md-ah9.13`)

`bundle-contract.md` is the L3-owned handoff: `throwaway-bundle.sh` is explicitly *not* inherited, the contract is. Packaging adopts the contract as the **cross-platform producer rule set** and keeps publishing in `md-kb7`:

| Contract rule | Linux OCI (Tier A/B) | Linux native packages | macOS `.app` (prototype) | Windows |
|---|---|---|---|---|
| Producer on pinned base, same arch; image never compiles | CI job on pinned Ubuntu 24.04 digest per arch | distro build roots (deb/Copr/AUR CI) | CI `macos-14+` runner, `MACOSX_DEPLOYMENT_TARGET=13.0` | CI Windows runner, MSVC/MinGW per L2 |
| Layout `opt/metadesk/{bin,lib}` + `SHA256SUMS` | **as specified** | FHS paths (`/usr/bin`, `/usr/lib/metadesk/`) | `Contents/{MacOS,Frameworks}` + `Resources/metadesk-bundle.json`; `SHA256SUMS` beside the artifact (inside would break the code seal) | `Program Files\Metadesk\{bin}` + side-by-side DLLs |
| Base-owned set | glibc/loader, libgcc, libstdc++, CA certs | everything the distro provides (declared `Depends:`) | `/usr/lib`, `/System` | OS + MSVC runtime (merge module / app-local) |
| Private non-base closure, dereferenced | `ldd` walk + copy | none (system libs) except the static nostrc pin | `otool` walk + copy (`bundle_app.py`) | `dumpbin /dependents` walk |
| **No producer RPATH** | `patchelf --remove-rpath` + `ld.so.conf.d` | none needed | all `LC_RPATH` deleted; only `@executable_path/../Frameworks` / `@loader_path` | n/a (DLL search order; `SetDefaultDllDirectories`) |
| dlopen features enumerated and retested, not trusted to the linker view | PipeWire/SPA, portals, NSS (Tier B, video-validation.md) | distro packages | SDL3 via sdl2-compat (`--dlopen`), proven loaded by `verify-app.sh` | Media Foundation/AMF/NVENC runtime DLLs |
| No keys, tokens, config, sockets, desktop state, restore tokens | same | same; config under XDG paths at runtime | same; verified by layout | same |
| Consumer gates (hashes, `readelf`/no `not found`, no-desktop fails nonzero, SIGTERM) | contract gates 1–6 in CI | `lintian`/`rpmlint`/`namcap` + smoke | `verify-app.sh`: load-command closure, rpaths, codesign, clean-env `DYLD_PRINT_LIBRARIES`, bare host fails nonzero with a diagnostic, optional `MD_MACOS_MIN` floor | `signtool verify` + clean-VM smoke |
| No publishing in the contract | **md-kb7 publishes**: GHCR, cosign-signed, by digest | repos | notarized DMG + cask | MSI + winget |

**md-ah9.13 status:** satisfied by this plan. The handoff is acknowledged here; publishing stays in `md-kb7`; the contract's rules are adopted as the producer rule set. The macOS prototype is the first implementation of those rules on a non-container target. The production Linux producer that replaces `throwaway-bundle.sh` is follow-up `md-kb7` work (§12), not a change under `deploy/containers/`.

### 4.1 macOS prototype results (2026-11-21)

`packaging/macos/build-app.sh` (see `packaging/README.md`) produced `Metadesk.app` and `Metadesk-0.1.0.dmg` from the build in §3:

- `bundle_app.py`: 30 private dylibs plus declared dlopen `libSDL3.dylib`; every `LC_RPATH` replaced; inside-out ad-hoc signature; `Info.plist` with `LSMinimumSystemVersion` computed as the max `minos` of all images (27.2); manifest with source/nostrc revisions, arch, per-image `minos` and the origin of every private library.
- `verify-app.sh` **PASS**:
  - no load command outside `/usr/lib`, `/System` or bundled `@rpath`;
  - only bundle-relative rpaths;
  - `codesign --verify --strict --deep` passes.
- Clean-environment runs (`env -i`, system-only `PATH`, `DYLD_PRINT_LIBRARIES`): `metadesk-host --help` (exit 0), `metadesk-client --help` (exit 0), bare `metadesk-host` (exit 1 with a diagnostic, no hang) and a display-initializing client run (Cocoa video driver; exit 1 on refused connection).
  - Every loaded image came from the bundle, `/usr/lib` or `/System`.
  - The bundled SDL3 was the one dlopened.
- The same verification passes from the **read-only mounted DMG** (relocation check).
- `MD_MACOS_MIN=13.0` **fails** as intended (27.2 > 13.0), and `open Metadesk.app` is refused by Launch Services on this 27.0.1 host (`-10825`). This is the deployment-target finding that `md-ouw3` fixes.
- Not done (needs credentials or follow-up work): Developer ID signing, notarization, stapling, host helper app/SMAppService, Gatekeeper assessment on a clean machine (`md-wdi3`).

## 5. Channel per platform

| Platform | Primary (recommended) | Secondary | Why |
|---|---|---|---|
| **macOS** (13+, arm64 first, then universal2) | Notarized, Developer ID–signed `Metadesk.dmg` | Homebrew **cask** (`brew install --cask metadesk`), own tap → `homebrew/cask` once stable | A cask installs the same notarized `.app` (no second build); a formula would compile, which defeats the goal |
| **Windows** 10/11 x64 (L2 scope) | Signed **MSI** (WiX v4) | **winget** manifest pointing at the MSI; MSIX later if Store distribution is wanted | MSI is what IT/winget expect; MSIX sandboxing complicates desktop capture/input services |
| **Debian/Ubuntu** (Debian 13, Ubuntu 24.04+) | `.deb` in a signed apt repo (OBS or self-hosted `reprepro`) | PPA for Ubuntu-only users | System FFmpeg/SDL2/PipeWire; nostrc pin static |
| **Fedora/RHEL** (Fedora 41+, EL9+ via EPEL deps) | `.rpm` via **Copr** | — | Copr builds from SRPM per release; Fedora FFmpeg is `ffmpeg-free` (no GPL x264) — fine for the HW/LGPL path |
| **Arch + Omarchy** | AUR **`metadesk-bin`** (repackages the release tarball; no compile) | AUR `metadesk` (source) for users who want it | Omarchy is Arch + Hyprland; AUR via `yay` is its native path. `-bin` satisfies "no build from source" |
| **Linux, distro-agnostic** | **Flatpak** on Flathub: client + future manager | AppImage (client only, later) | GNOME runtime matches GTK4/libadwaita; portals give ScreenCast. Host-in-Flatpak is blocked on an input path (§6) |
| **Containers** | OCI image per the contract (GHCR, cosign) | — | Agent/headless hosting (`md-ah9`) |

## 6. Platform specifics and blockers

**macOS.**
- **One bundle, two TCC identities.** ScreenCaptureKit/Accessibility grants attach to the *responsible* code identity.
  - A `metadesk-host` started from Terminal is attributed to Terminal.
  - Ship the host as a nested helper app, `Contents/Library/LoginItems/Metadesk Host.app`, with its own bundle ID. Register it with **`SMAppService`** (macOS 13+) as a LaunchAgent, so grants are stable and survive updates.
  - The cask exposes `metadesk-host`/`metadesk-client` CLIs via `binary` stanzas pointing *into* the bundle.
- **Hardened runtime.**
  - No special entitlements are needed for SCK/AX; TCC prompts are enough.
  - All bundled dylibs are signed with the same Team ID, so library validation passes without `disable-library-validation`.
  - `DYLD_*` diagnostics are unavailable under the hardened runtime. `verify-app.sh` runs on the ad-hoc stage *before* the release re-sign, and CI re-verifies the signature after.
- **Finder launch.** Today `metadesk-client` with no arguments prints usage and exits, so the `.app` is CLI-first until the macOS frontend (`md-pep3`) or a minimal connect dialog exists. That is acceptable for an early cask; note it in release notes.

**Windows** (owned by L2 for code; packaging here).
- Capture/input must run in the interactive user session, so a session-0 service can't capture. Ship the host as a per-user logon task (or a service plus per-session agent, as RustDesk does, if login-screen access is in scope).
- The MSI installs `fipsd` only if a Windows FIPS package exists; otherwise it documents it.

**Linux native packages.**
- Ship a **systemd user unit** (`metadesk-host.service`, `--managed` once `md-pw1q` lands).
- Ship udev rules **only** if the uinput backend is enabled. Prefer the wlroots virtual-input backend (`-Dwayland_input=true`) on wlroots compositors.
- Ship portal-based capture everywhere.

**Flatpak.**
- ScreenCast portal works.
- AT-SPI works with `--talk-name=org.a11y.Bus`.
- Wayland virtual-input protocols work if the compositor exposes them.
- **uinput needs `--device=all`** (not acceptable on Flathub).
- metadesk has **no RemoteDesktop-portal/libei input backend** (none in `src/core/input_*`).
- So: Flatpak = **client + manager** now; **host** after a portal/libei input backend exists (follow-up bead).

**OCI.** Tier A (agent, no video) and experimental Tier B per `deploy/containers/reference/*` (L3). md-kb7 only adds: build the contract layout in CI, assemble images on pinned digests, sign with cosign, publish by digest.

## 7. Dependency bundling matrix

`B` = bundled privately (in the artifact) · `S` = system package dependency · `P` = pinned + statically linked at build time · `—` = not applicable

| Dependency | macOS DMG/cask | Windows MSI | deb / rpm / AUR | Flatpak | OCI | Notes |
|---|---|---|---|---|---|---|
| **nostrc** (libnostr, libgo, nip11/17/19/44/51/98, signer cores) | P | P | P | P | P | Already static except `libnostr-json`; pin by commit in the manifest. Long-term: nostrc install rules + `.pc` → `S` on Linux |
| `libnostr-json` (nostrc) | B | B | P (build static) | P | B | Only shared nostrc lib today |
| FFmpeg (`libavcodec`, `libavutil`) | B (own LGPL minimal build) | B (own LGPL build w/ NVENC/AMF headers) | S | Flatpak runtime ffmpeg extension or bundled module | B (distro `libavcodec` from base) | §13 Q1 decides GPL `libx264` fallback |
| `libx264` | optional (GPL) | optional (GPL) | S (distro choice) | — (HW/ VAAPI path) | S | Only fallback when no HW encoder |
| SDL2 (+ SDL3 if sdl2-compat) | B (+ dlopen SDL3) | B | S | runtime | — (host only) | Prefer building **real SDL2** or migrate to SDL3 directly to drop the dlopen indirection |
| Dear ImGui | vendored source (`imgui_vendor/`) | same | same | same | — | already in-tree |
| libyuv | B | B | S where packaged at a usable version, else P | P | B | verify per distro (§12) |
| secp256k1 | B | B | S | P (nostrc flatpak module exists) | B | |
| libwebsockets + OpenSSL | B | B | S | P (module exists) / runtime OpenSSL | S (base) / B | one TLS stack per artifact |
| cJSON, libb2, jansson, nsync | B | B | S where packaged (nsync often isn't → P) | P (modules exist for jansson/nsync) | B | |
| GLib/GIO (NIP-55L, AT-SPI) | B (if signer_nip55l on macOS; else omit) | — | S | runtime | B | glib in the macOS closure came via signer deps — drop on macOS if NIP-55L is Linux-only |
| PipeWire, AT-SPI2, D-Bus, udev, portals | — | — | S | runtime/portals | Tier B per contract | never bundled on Linux |
| GTK4/libadwaita (manager) | — | — | S | GNOME runtime | — | Linux only |
| **FIPS daemon** | not bundled | not bundled | `Recommends:`/`optdepends` `fipsd` | not bundled (host-side daemon) | Tier B/C mesh per md-ah9 | §9 |

## 8. Signing, notarization, reproducibility, updates

| Channel | Signing | Integrity/verification | Updates |
|---|---|---|---|
| DMG | Developer ID Application; hardened runtime; sign inside-out (dylibs → helpers → app), then DMG; `xcrun notarytool submit --wait`; `xcrun stapler staple` | `spctl -a -vvv`, `codesign --verify --strict --deep`, `verify-app.sh` (pre-sign stage) | **Sparkle 2** with EdDSA keys; appcast served from releases |
| Homebrew cask | (inherits notarized DMG) | cask `sha256` | `brew upgrade` (cask `livecheck` on GitHub releases) |
| MSI | Authenticode via **Azure Trusted Signing** (or EV cert in HSM); timestamped | `signtool verify /pa` | winget (manifest PR per release); optional in-app check |
| deb/rpm repos | repo metadata GPG-signed (key in CI secret / HSM); Copr signs its own repo | `apt`/`dnf` | distro package manager |
| AUR | PKGBUILD pins release tarball `sha256sums` + upstream `.sig` (minisign/GPG) | `makepkg` verification | `yay -Syu` |
| Flatpak | Flathub signs | OSTree | Flatpak updates |
| OCI | **cosign** keyless (GitHub OIDC) | `cosign verify` + digest pin | users pull by tag/digest |

Cross-cutting:
- **Checksums + SBOM:** each release publishes `SHA256SUMS` (signed with minisign or GPG), a CycloneDX SBOM generated from the manifest (`metadesk-bundle.json` already records every private library and its source), and the source tarball.
- **Reproducibility:** pinned toolchain images/runners, pinned nostrc and dependency revisions, `SOURCE_DATE_EPOCH`, `-ffile-prefix-map`, sorted archive members. Aim for *verifiable rebuilds of the Linux tarball/OCI tree* first. Byte-reproducible signed macOS/Windows artifacts are out of scope; their unsigned payloads can be compared.
- **Nostr-native release announcement (optional, recommended):** CI signs a **NIP-94 kind-1063** file-metadata event per artifact (`url`, `x` = sha256, `m`, `size`) with a project release key held by a signer (NIP-46 bunker, never a raw key in CI), and publishes it to configured relays.
  - Publishing waits for relay `OK`: a successful websocket send is not a successful publish.
  - Clients and the future manager subscribe with a tight `REQ` filter (`kinds:[1063]`, `authors:[release_pubkey]`, `#m`) and react to new events. That is an event-driven update notice instead of a polling updater.
  - The hash in the event must match the channel artifact; channels remain the install path.

## 9. External runtime dependencies (not bundled)

| Need | macOS | Windows | Linux |
|---|---|---|---|
| **FIPS daemon** | `fipsd` with nostrc's `fipsd/packaging/macos/com.fips.fipsd.plist`; document install; cask `depends_on` once a fipsd cask exists | document; package when a Windows fipsd release exists | nostrc ships `fipsd` deb/rpm/Arch + systemd packaging: metadesk packages `Recommends:` it (direct-IP + Nostr signaling works without it) |
| Screen capture permission | Screen Recording TCC for the **host helper app** | none (DXGI) | ScreenCast portal (GNOME/KDE/xdph), restore tokens handled by host |
| Input injection | Accessibility TCC for the host helper | none / UIPI limits for elevated windows | uinput (udev rule + group) or wlroots virtual input; portal/libei later |
| Accessibility tree | Accessibility TCC | UIA (no permission) | AT-SPI bus |
| Signer | Groundhog/Keychain NIP-46 credentials, NIP-5F socket | NIP-46 | Grotto (NIP-55L), NIP-5F, NIP-46 via Secret Service |

## 10. CI release pipeline sketch

Modeled on nostrc `release.yml` / `fips-packaging.yml`: tag-triggered, `workflow_dispatch` defaults to **dry-run**, and native runners per arch (no QEMU).

```text
on: push tags v*  |  workflow_dispatch(dry_run=true)

source        → git archive + pinned nostrc rev + SOURCE_DATE_EPOCH; upload source tarball
deps-macos    → build cached deps with MACOSX_DEPLOYMENT_TARGET=13.0 (minimal LGPL FFmpeg, SDL2, libyuv,
                secp256k1, libwebsockets+OpenSSL, cJSON, libb2, jansson, nsync) per arch; cache by hash
build-macos   → meson (release) → stage → packaging/macos/bundle_app.py (ad-hoc) → verify-app.sh
                MD_MACOS_MIN=13.0 → re-sign Developer ID + hardened runtime → DMG → notarize → staple
                → spctl assess → lipo universal2 (when both arch stages pass)
build-linux   → per arch on pinned Ubuntu 24.04 digest: contract-layout tree (replaces throwaway-bundle.sh)
                → contract gates 1–6 → tarball
pkg-deb       → debuild in Debian 13 / Ubuntu 24.04 chroots → lintian → install smoke in clean container
pkg-rpm       → SRPM → Copr build (Fedora N, N-1, EL9) → rpmlint → dnf install smoke
pkg-arch      → PKGBUILD (metadesk-bin) from tarball → namcap → pacman -U smoke in archlinux container
pkg-flatpak   → flatpak-builder client(+manager) → flatpak run --command=metadesk-client --help smoke
oci           → assemble Tier A image from the tree on pinned base digest → contract gates → cosign sign
build-windows → (L2) MSVC build → DLL closure → WiX MSI → signtool (Trusted Signing) → clean-VM smoke
release       → (not dry-run) GitHub release: artifacts, SHA256SUMS(.minisig), SBOM; push GHCR; Copr/apt
                repo publish; open winget + cask + Flathub + AUR update PRs; publish NIP-94 events (OK-gated)
e2e           → md-5zi matrix consumes the *installed* artifacts, not the build tree
```

Secrets live in protected environments (manual approval for non-dry-run): Apple notarization API key, Developer ID cert (or HSM), Trusted Signing credentials, repo GPG key, release NIP-46 bunker URI.

## 11. Omarchy / Hyprland validation checklist

Run on a real Omarchy install (or Arch + Hyprland + Omarchy defaults), from the AUR `metadesk-bin` package only. Tracked by follow-up `md-kb7` bead; results feed `md-5zi`.

1. `yay -S metadesk-bin` installs without compiling; `pacman -Qi` shows `optdepends` on `fipsd`, `xdg-desktop-portal-hyprland`.
2. `xdg-desktop-portal-hyprland` is the active ScreenCast backend (`busctl --user` / `XDG_CURRENT_DESKTOP=Hyprland`); host capture starts, the xdph share picker appears, and selecting an output/window streams frames.
3. Restore token reuse: second host start with the persisted token skips the picker where xdph supports it; a revoked token re-prompts (no silent black stream; cf. `md-3nv`).
4. Multi-monitor and fractional scaling: `--capture-output` per Hyprland monitor; input lands on the right output at 1.25/1.5 scale (cf. `md-w65`).
5. Input: wlroots virtual-pointer + virtual-keyboard (`-Dwayland_input=true` build) inject correctly. Keyboard layout follows the Omarchy default; no uinput or udev rule needed. If uinput is used instead, the udev rule/group grant works after re-login.
6. AT-SPI: tree snapshot works for a GTK4 app and an Electron/Chromium app with `--force-renderer-accessibility`.
7. Client on Hyprland: SDL2 Wayland video driver chosen (not XWayland), HiDPI correct, overlay hotkeys not captured by Hyprland binds.
8. Signers: NIP-46 credentials in `gnome-keyring`/Secret Service (Omarchy ships a keyring; verify), NIP-5F socket, Grotto if installed.
9. FIPS: `fipsd` from its Arch package, `metadesk-host` FIPS readiness and the npub/node-key check pass.
10. systemd user unit: `systemctl --user enable --now metadesk-host`; it survives Hyprland restart; logout behavior matches the `md-pw1q` decision.
11. Uninstall removes binaries/units, leaves user config/keyring entries untouched.

## 12. Phased rollout and follow-up beads

| Phase | Goal | Beads |
|---|---|---|
| 0 | This survey + macOS prototype (`packaging/macos/`) | `md-kb7`, `md-ah9.13` (contract consumed) |
| 1 | Release-ready macOS DMG: license decision, deps at 13.0 floor + minimal LGPL FFmpeg, Developer ID + notarization + host helper/SMAppService, meson install fix | `md-2exc`, `md-ouw3`, `md-wdi3`, `md-v314` |
| 2 | Linux: nostrc pin/install story, production contract producer + OCI publish, AUR `-bin` / deb / rpm + systemd user unit | `md-fdi4`, `md-x7wk`, `md-xs90`, `md-pw1q` |
| 3 | CI release pipeline (dry-run default) driving phases 1–2 | `md-6mqr` |
| 4 | Homebrew cask, Sparkle, Flatpak client (+manager when `md-fbg4` lands), Omarchy validation | `md-sit3`, `md-pjmz`, `md-lxxz`, `md-wx68` |
| 5 | Windows MSI + winget (with L2), portal/libei input to unlock Flatpak host, NIP-94 announcements, AppImage | `md-jsrk`, `md-y8kc`, `md-46v6`, `md-pqc7` |

## 13. Open decisions

1. **FFmpeg licensing.** Ship LGPL-only (HW encoders + native decoder; no software encode fallback when no HW encoder) or also a GPL `libx264` fallback? GPL fallback makes those artifacts GPL. metadesk has **no LICENSE file** today. A project license must be chosen before any binary release.
2. **macOS floor and arch.** 13.0 (ScreenCaptureKit) and universal2, or arm64-only at first?
3. **Apple/Windows signing identities.** Who holds the Developer ID and Trusted Signing accounts, and are HSM-backed keys required?
4. **SDL.** Keep SDL2 (and build real SDL2 for bundles) or migrate the client to SDL3 to remove the sdl2-compat dlopen hop?
5. **Linux repo hosting.** Self-hosted apt (`reprepro`), openSUSE Build Service (one source → deb+rpm+Arch repos), or Copr + PPA?
6. **nostrc distribution.** Keep static pin forever, or have nostrc ship install rules + pkg-config + a versioned ABI so distros package it (`S` on Linux)?
7. **Host-in-Flatpak.** Worth a portal/libei input backend, or keep host as native package only?
8. **Release key for NIP-94.** Separate project identity held by a bunker; which relays?
9. **Login-screen / pre-login access** (Windows service + agent, macOS LaunchDaemon) — in scope at all? It changes the service model on both platforms.

## 14. Follow-up beads filed (discovered-from `md-kb7`)

| Bead | P | Title |
|---|---|---|
| `md-2exc` | 1 | Project license + FFmpeg/x264 licensing decision |
| `md-ouw3` | 2 | macOS deps at 13.0 floor, minimal LGPL FFmpeg, universal2 |
| `md-wdi3` | 2 | Developer ID signing, notarization, host helper app + SMAppService |
| `md-fdi4` | 2 | nostrc install rules / supported static vendoring |
| `md-x7wk` | 2 | Linux contract producer (replaces `throwaway-bundle.sh`) + cosign OCI publish (related `md-ah9.13`) |
| `md-xs90` | 2 | AUR `metadesk-bin`, deb repo, Copr rpm, systemd user unit |
| `md-6mqr` | 2 | CI release pipeline |
| `md-v314` | 3 | meson install not runnable in place on macOS (`@rpath/libyuv`) |
| `md-sit3` | 3 | Homebrew cask |
| `md-pjmz` | 3 | Sparkle 2 updates for the DMG |
| `md-wx68` | 3 | Omarchy/Hyprland validation (related `md-5zi`) |
| `md-lxxz` | 3 | Flatpak client + manager (related `md-fbg4`) |
| `md-y8kc` | 3 | RemoteDesktop portal / libei input backend |
| `md-jsrk` | 3 | Windows MSI + winget |
| `md-46v6` | 4 | NIP-94 release events |
| `md-pqc7` | 4 | AppImage client |
| `md-pw1q` | 3 | Host process lifecycle (filed from `md-70v`, shared) |
