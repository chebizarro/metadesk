# Third-party notices and release inventory

metadesk itself is MIT licensed; see [LICENSE](LICENSE). Dependencies are not
relicensed by that file. This inventory records the `md-2exc` owner decision
(2026-11-22) and the dependency evidence checked for lane L15. It is not legal
advice or a claim that an unbuilt artifact has passed a license audit.

## Evidence and artifact scope

At metadesk `887b62d`, **there is no `subprojects/` directory or wrap file**.
The vendored client dependency is `imgui_vendor/` (Dear ImGui 1.90.4;
`LICENSE.txt`, MIT, Copyright 2014–2024 Omar Cornut). Its embedded stb headers
carry their own MIT/public-domain alternatives; retain those header notices.
nostrc is an external dependency (installed SDK or separate build tree).
Current producers pin `6f7a8d025768d497bef700fe55eea1c687dac328`. Its root
`LICENSE` is MIT (Chris Daley); `third_party/nsync/LICENSE` is Apache-2.0.

The [macOS prototype](packaging/macos/bundle_app.py) writes
`Contents/Resources/metadesk-bundle.json`: `private_libraries` records origins,
`dlopen_bundled` records extra runtime libraries, and `image_minos` records all
images. **The original prototype manifest is not committed or retained in this
worktree.** The historical 30-dylib result in [plan §3](docs/plans/packaging-2026-11-21.md#3-current-state--measured-not-assumed)
is evidence of a developer prototype, not an LGPL release. Installed producer
license files and current Mach-O dependencies were checked separately below;
these versions must not be mistaken for a frozen release BOM.

Legend: **B** = private shared library; **P** = source/static code in binary;
**S** = system dependency; **R** = Flatpak runtime; **—** = not shipped.
Optional features are included only when built. OCI is host-only; Flatpak is
client/manager (future manager dependencies apply only when it is included).

| Component | DMG | MSI | .deb/.rpm/AUR | Container | Flatpak |
|---|---|---|---|---|---|
| nostrc core/NIPs/libgo, nsync | P | P | P (system later) | P | P |
| libnostr-json | B | B | P | B | P |
| FFmpeg avcodec/avutil, minimal LGPL build | B | B | S (distro build) | B | B (own module) |
| SDL2, ImGui (+SDL3 only for sdl2-compat) | B/P | B/P | S/P | — | R or B/P |
| libyuv (+JPEG if enabled), secp256k1, libsodium | B/P | B/P | S or P | B/P | B/P |
| cJSON, jansson, libb2 | B/P | B/P | S or P | B/P | B/P |
| libwebsockets, OpenSSL, libcurl and enabled closure | B/P | B/P | S | B/P or base | B/P or R |
| uuid | OS | OS | S | S | R |
| GLib/GObject/GIO and their closure | B if linked | — unless linked | S | B or base | R |
| PipeWire/SPA, AT-SPI2, D-Bus, udev/systemd | — | — | S | base/host integration | R/host services |
| GTK4/libadwaita (future manager) | — | — | S | — | R |
| OpenH264 binary | **—** | **—** | **—** | **—** | **—** |
| FIPS daemon, GPU driver runtimes | **—** | **—** | separate packages | host integration | host integration |

For each actual release, expand all B/P/base rows into an **exact version/hash,
license-file and source URL inventory**, including transitive libraries and
container base packages. Bundle this notice **and the upstream license and
copyright texts**, including exceptions/NOTICE/PATENTS files, in the artifact's
legal directory (macOS `Contents/Resources/licenses/`). A summary table is not
a substitute for those texts. Reconcile against the manifest plus runtime
loads; fail the release on an unclassified library. Base-owned OS libraries
are not copied into DMG/MSI. The original macOS prototype did not assemble
this legal directory; the source-pinned W5 profile below does. Publication and
corresponding-source gates remain release-pipeline work.

### L15 macOS verification manifest

A fresh private prototype was built from this lane with
`MD_FFMPEG_PREFIX` selecting minimal FFmpeg 8.1.2; the generated manifest and
runtime-load checks were inspected before deleting the temporary build.
Its **20 private libraries** (including metadesk itself) are fully classified:

| Manifest images | Version / license |
|---|---|
| libmetadesk.0 | 0.1.0 / MIT |
| libnostr-json.2 | nostrc cfcb86296068 / MIT |
| libavcodec.62, libavutil.60 | FFmpeg 8.1.2 / LGPL-2.1-or-later |
| libSDL2-2.0.0, libSDL3 (explicit dlopen) | sdl2-compat 2.32.74, SDL3 3.4.20 / Zlib |
| libcjson.1 | 1.7.19 / MIT |
| libcrypto.4, libssl.4 | OpenSSL 4.1.0 / Apache-2.0 (**not** the planned 3.x pin) |
| libglib-2.0.0, libgobject-2.0.0 | 2.90.1 / LGPL-2.1-or-later |
| libintl.8 | gettext 1.0 / LGPL-2.1-or-later |
| libjansson.4 | 2.15.1 / MIT + Lucent dtoa notice |
| libjpeg.8 | libjpeg-turbo 3.2.1 / IJG + BSD-3-Clause + Zlib |
| libnsync.1 | 1.30.0 / Apache-2.0 |
| libpcre2-8.0 | 10.49 / BSD-3-Clause WITH PCRE2-exception + JIT notices |
| libsecp256k1.7 | 0.8.0 / MIT |
| libsodium.26 | 1.0.22 / ISC |
| libwebsockets.23 | 5.1.0 / MIT + per-file notices |
| libyuv | header version 1925, unknown source revision / BSD-3-Clause + PATENTS |

libb2 0.98.1 (CC0-1.0) is an explicit dynamic link input, but the linker
drops it from this prototype (no BLAKE symbols or runtime load); it is not
claimed as statically incorporated. ImGui and nostrc components are static
code as described above. libcurl/uuid and system frameworks resolve to
Apple base-owned paths in this prototype; the Homebrew libcurl extras below
are therefore **not** its shipped closure. No GPL codec library or OpenH264
appears in this manifest. FFmpeg dylibs target macOS 13.0, but the whole app
still requires 27.2 because of SDL3/libwebsockets. This is a developer
verification artifact, not a distributable release or a cross-platform BOM.

## W5 source-pinned macOS arm64 profile

The [macOS dependency builder](packaging/macos/README.md) uses the exact URLs
and SHA256 values in [`deps.lock.json`](packaging/macos/deps.lock.json).
This replaces Homebrew bottles for this profile, not for distro builds or the
historical L15 prototype above. All target code, including nostrc, is built
with a macOS 13.0 deployment target.

| Component | Pin | License / selected configuration |
|---|---|---|
| FFmpeg | 8.1.2 | LGPL-2.1-or-later; shared avcodec/avutil, native H.264 decode/parser and VideoToolbox encode only |
| SDL2 | 2.32.10 | Zlib; real SDL2, no sdl2-compat/SDL3 dlopen |
| libyuv | `0ec9f68d82634f6f3414aaaf780e03f9edf002b3` (header 1985) | BSD-3-Clause + PATENTS; JPEG disabled |
| OpenSSL | 3.5.9 | Apache-2.0; shared libssl/libcrypto, no loadable modules |
| libsodium | 1.0.22 | ISC |
| cJSON | 1.7.19 | MIT |
| libwebsockets | 5.1.0 | MIT + upstream per-file notices; OpenSSL, poll, no HTTP/3/GnuTLS/event-loop plugins |
| jansson | 2.15.1 | MIT + Lucent dtoa notice |
| libb2 | 0.98.1 | CC0-1.0; native-CPU tuning/OpenMP disabled |
| secp256k1 | 0.8.0 | MIT; recovery enabled |
| nsync | 1.30.0 | Apache-2.0; shared |
| nostrc | `6f7a8d025768d497bef700fe55eea1c687dac328` | MIT; static core/NIPs + shared nostr-json, GLib-free, NIP-46/NIP-5F enabled |
| Dear ImGui | 1.90.4 (vendored) | MIT; embedded stb notices retained in source |

Apple libcurl, uuid, zlib and platform frameworks are OS-owned. No GLib,
gettext, PCRE2, JPEG, SDL3, GPL codec or OpenH264 is part of this profile.
libb2 is built as an explicit dependency but may be dropped from the final
load closure when unused; consult the actual bundle manifest, not this input
list. The builder retains source archives, build/config logs, toolchain and
root upstream legal texts; the app stages those legal texts, the lock file,
this notice, metadesk's license and ImGui's license under Resources.
Corresponding-source release publication, attribution UI, Developer ID and
notarization are still publisher gates; shipping this table is not sufficient.

## Component licenses (not guessed from package names)

Versions below are locally inspected evidence, not release pins. `opt` paths
refer to the Homebrew producer's installed license files; record the resolved
Cellar revision in the release manifest. Distro versions must be checked in
the distro's copyright metadata, not inferred from this macOS snapshot.

| Component / inspected evidence | License and notes |
|---|---|
| nostrc (`LICENSE`), including libnostr-json/libgo/NIP cores | MIT; retain nested third-party notices as well as the root license. |
| SDL2 / sdl2-compat 2.32.74 (`opt/sdl2/LICENSE.txt`), SDL3 3.4.20 (`opt/sdl3/LICENSE.txt`) | Zlib. Real SDL2 is also Zlib; sdl2-compat requires the separately inventoried SDL3 dlopen dependency. |
| libyuv, installed header version 1925 | BSD-3-Clause, plus `PATENTS` grant. Header identifies these upstream files; local hand-installed dylib has no source revision/packaging receipt. **Release must rebuild from a pinned source and retain LICENSE/PATENTS/AUTHORS**, not label this binary reproducible. [Upstream tree](https://chromium.googlesource.com/libyuv/libyuv/). |
| OpenSSL 3 (3.5.9, `opt/openssl@3/LICENSE.txt`) | Apache-2.0. Intended release TLS family. The prototype actually resolves OpenSSL 4 (4.1.0, same Apache-2.0 text); do not call that OpenSSL 3. Rebuild libwebsockets/curl against the selected TLS pin to avoid two stacks. |
| libsodium 1.0.22 (`opt/libsodium/LICENSE`) | ISC; retain any additional per-file notices in the selected source. |
| cJSON 1.7.19 (`opt/cjson/LICENSE`) | MIT. |
| libwebsockets 5.1.0 (`opt/libwebsockets/LICENSE`) | MIT with identified BSD-3-Clause/Zlib/CC0 and other per-file notices. Current 5.x is **not** the old LGPL-with-static-link-exception license. Retain its full LICENSE and applicable included-code notices. |
| Dear ImGui 1.90.4 (`imgui_vendor/LICENSE.txt`) | MIT, including vendored stb alternatives noted above. |
| jansson 2.15.1 (`opt/jansson/LICENSE`) | MIT, with Lucent permissive `src/dtoa.c` notice and other exceptions reproduced in that file. |
| libb2 0.98.1 (`opt/libb2/COPYING`) | **CC0-1.0**, not the license of an unrelated BLAKE2 implementation. |
| libcurl 8.23.0 (`opt/curl/COPYING`) | curl license (permissive); preserve its copyright/permission notice. Apple system libcurl is OS-owned; Homebrew curl has additional private dependencies (below). |
| libuuid on Linux | BSD-3-Clause ([util-linux libuuid COPYING](https://github.com/util-linux/util-linux/blob/master/libuuid/COPYING)); not the GPL license of util-linux tools. macOS uuid calls are in libSystem; Windows UUID APIs are OS-owned. |
| secp256k1 0.8.0 (`opt/secp256k1/COPYING`) | MIT. |
| nsync 1.30.0 (`opt/nsync/LICENSE` and nostrc `third_party/nsync/LICENSE`) | Apache-2.0. Build dependency may be statically incorporated; it still needs notices. |
| minimal FFmpeg 8.1.2 (`COPYING.LGPLv2.1`, `LICENSE.md`, generated config) | LGPL-2.1-or-later; selected shared avcodec/avutil only. This is the Mac verification version, not a universal release pin. See compliance below. |
| nv-codec-headers (NVIDIA headers), AMD AMF headers | MIT; build headers only, no vendor encoder binary is bundled. Pin and retain their license notices if incorporated. |
| oneVPL dispatcher (QSV), libva, libdrm (Linux) | MIT-style/per-file permissive notices; dynamic dispatcher/VA libraries, if private, are additional B entries, not merely headers. GPU driver/runtime libraries are supplied by the user/OS. |

### Additional closure and Linux system libraries

| Component | License / distribution boundary |
|---|---|
| GLib, GObject, GIO (producer 2.90.1) | LGPL-2.1-or-later; system on native Linux, runtime in Flatpak, private if actually linked on macOS/container. Include corresponding-source and replacement rights for private copies. |
| GNU gettext **libintl** (producer 1.0) | LGPL-2.1-or-later. Gettext **tools** are GPL-3.0-or-later and are not application payload; don't assign the tools' license to libintl. |
| PCRE2 10.49 (`share/doc/pcre2/LICENCE.md`) | BSD-3-Clause WITH PCRE2-exception; JIT/sljit includes BSD-2-Clause notices. |
| libffi | MIT (where not OS-owned). |
| libjpeg-turbo (local libyuv dependency) | IJG, BSD-3-Clause and Zlib terms; retain `LICENSE.md` and referenced license texts for selected source. |
| zlib | Zlib; base-owned on macOS, otherwise classify actual copy. |
| xz/liblzma | Version-specific public-domain/BSD/per-file licenses; legacy FFmpeg closure only here. Preserve selected upstream COPYING, do not infer from xz tools. |
| zstd; brotli | zstd: BSD-3-Clause (or upstream GPL-2.0 alternative); brotli: MIT. |
| nghttp2, nghttp3, ngtcp2 | MIT. Conditional Homebrew libcurl HTTP/2/3 closure, not core metadesk requirements. |
| libssh2; libpsl; libunistring (+libidn2 if selected) | libssh2: BSD-3-Clause; libpsl: MIT; libunistring: LGPL-3.0-or-later or GPL-2.0-or-later (use LGPL); libidn2: LGPL-3.0-or-later or GPL-2.0-or-later (use LGPL). These may enter via curl/libintl; inventory the actual link closure. |
| glibc/loader, libm, pthreads, libresolv (Linux base) | LGPL-2.1-or-later and per-file exceptions. Do not bundle another libc in the application tree. Containers still redistribute base packages: retain base source/copyright mechanisms. |
| libgcc, libstdc++ (Linux base) | GPL-3.0-or-later WITH GCC-exception-3.1. “No GPL codecs” is **not** a claim that a whole Linux base image contains no GPL software. |
| AT-SPI2 libraries, GTK4, libadwaita | LGPL-2.1-or-later; Linux system/Flatpak runtime. GTK/adwaita apply only to future manager artifacts. |
| libudev/libsystemd; libmount/libblkid | LGPL-2.1-or-later (library targets); retain per-file notices and distro metadata. Tools may have different licenses. |
| D-Bus libdbus | AFL-2.1 OR GPL-2.0-or-later (dual licensed), not LGPL. |
| PipeWire/libspa | MIT generally; `libspa-alsa.so` includes LGPL code, `libjackserver.so` has GPL-2.0 code. Do not bundle the latter into the minimal codec payload. [Upstream LICENSE](https://github.com/PipeWire/pipewire/blob/master/LICENSE). |
| Wayland, libei, portals and graphics/window-system libraries | Generally MIT-style for Wayland/libei; portals and their backends have their own licenses. Treat as system/runtime services; enumerate exact redistributed runtime packages, not guessed blanket licenses. |
| Apple frameworks/libSystem; Windows SDK/system DLLs | Platform-owned, not redistributed by metadesk. Any app-local MSVC redistributable needs Microsoft's distribution terms and exact package inventory. |

### Historical prototype: excluded codec closure

The measured Homebrew prototype in plan §3 includes **GPL FFmpeg** and x264 /
x265 (GPL-2.0-or-later). Its producer's FFmpeg formula is GPL-3.0-or-later.
That artifact must not be published as the chosen LGPL-only release.
Other recorded closure members: dav1d (BSD-2-Clause), SVT-AV1/libvpx/Opus
(BSD-3-Clause), LAME (LGPL-2.0-or-later), mpg123 (producer 1.33.7:
LGPL-2.1-only), plus the GLib/libintl/PCRE2 and JPEG closure above. These
codecs are **not required and not included in the minimal FFmpeg profiles**.
Do not simply remove their dylibs from a GPL-configured FFmpeg; rebuild it.

## Bundled FFmpeg: LGPL compliance and reproducibility

Use [build-minimal-lgpl.sh](packaging/ffmpeg/build-minimal-lgpl.sh) and the
[per-artifact flags](docs/plans/packaging-2026-11-21.md#71-ffmpeg-configure-policy-per-artifact).
No libx264, libx265, libopenh264, GPL, version3 or nonfree features are enabled.
OpenH264 is not an FFmpeg dependency. Preserve the exact configure arguments,
config.log/config.h/config_components.h, toolchain, upstream archive hash or
commit, and all patches with each binary's manifest.

Release checklist, following [FFmpeg's compliance guidance](https://ffmpeg.org/legal.html):

- Distribute the **exact corresponding FFmpeg source** including patches and
  build instructions as a source archive beside the binary on the release
  server. Fill the concrete same-release URL in the release notes/download
  page; an upstream homepage alone is not a corresponding-source offer.
- Include `COPYING.LGPLv2.1`, FFmpeg's attribution and applicable per-file
  notices in the artifact, documentation and About/license UI.
- Dynamically link avcodec/avutil (DLL/dylib/so). Preserve users' ability to
  replace them with compatible modified versions, and document how to run a
  modified local bundle (including re-signing on macOS). If static linking is
  ever introduced, supply relinkable application objects and instructions.
  Do not prohibit reverse engineering for debugging modifications.
- Apply these obligations to **other private LGPL libraries** too. Distro
  packages instead use the system libavcodec and the distro's copyright and
  source mechanisms; their enabled codecs and licensing may differ, including
  libx264 if supplied by the distro. MIT does not relicense those dependencies.

## OpenH264 runtime and H.264 patents

OpenH264 source is BSD-2-Clause licensed. The optional software fallback uses
**Cisco's prebuilt binary**, separately downloaded from Cisco to the user's
device and dynamically loaded by metadesk itself. It is **never bundled** in
DMG/MSI/OCI/Flatpak or native packages; do not compile it into FFmpeg or
redistribute a self-built substitute as Cisco-covered. Hardware/OS encoders
are preferred; source builds may opt into a system libx264 path separately.

Cisco's binary patent terms apply conditionally to Cisco-distributed binaries,
not automatically to source builds or all H.264 uses. Runtime integration must
provide user enable/disable/re-enable control, display **“OpenH264 Video Codec
provided by Cisco Systems, Inc.”** where that control lives, and reproduce the
complete binary license in the user's licensing information. These controls,
download verification and the loader are L14 integration work, not implemented
by this documentation lane. See the [Cisco binary license](https://www.openh264.org/BINARY_LICENSE.txt),
[FAQ](https://www.openh264.org/faq.html), and [source license](https://github.com/cisco/openh264/blob/master/LICENSE).

H.264 patent obligations can depend on jurisdiction, distribution and use.
Neither MIT/LGPL/BSD nor a hardware encoder is a blanket patent clearance;
Cisco's coverage has explicit conditions and limits. This notice provides
technical distribution policy, **not legal advice**.
