# W9 Linux packaging qualification

Worktree: `md/w9-linux-packages`. No push, merge into master, public publication,
real signing keys, Beads closure, host desktop/session access or uinput activity.
Raw receipts belong in ignored `packaging/linux/output/`, not Git.
Only this summary and `evidence/artifact-sha256.txt` are retained as evidence.

## Original native-package qualification (2026-11-22)

Application baseline `0e91e463b2baff1fca2ff6daca1262bdaa1d5858`, nostrc
`cfcb86296068c1e4090bc5ce8a486392dc74eb11`. Ubuntu 24.04 and Fedora 44 aarch64.

- Built Ubuntu `.deb`, unsigned noble/arm64 apt layout, Fedora runtime RPM,
  SRPM, debuginfo and debugsource packages. Five package SHA256 values are in
  `evidence/artifact-sha256.txt`; artifacts remain ignored under `artifacts/`.
- Clean-container installs, host/client help and dependency closure passed.
  A network-disabled local `file:` apt repository install passed, with
  `trusted=yes` solely for that unsigned local fixture, not release trust.
- Fedora `rpm -V` and installed systemd user-unit static verification passed;
  unit never enabled/started. Ubuntu slim-container dpkg exclusions omit docs,
  so its documentation-file integrity check is not claimed as a pass.
- lintian: exit 0, **0 errors / 1 first-upload warning**. rpmlint 2.8: exit 0,
  **0 errors / 5 warnings**: four local source-URL diagnostics and one
  `SSL_CTX_set_cipher_list` crypto-policy warning requiring dependency review.
  No warning suppression or fully lint-clean claim.
- Native packages link system FFmpeg, static pinned nostrc/JSON; Fedora builds
  pinned nsync 1.30.0 (`5b420ef6b16532a4fad4be1fde8e483b6b7bd8f4`) statically.
  Cisco OpenH264 is never bundled. Native-only Meson install RUNPATH avoids
  patchelf rewriting that damaged Fedora ELF build IDs; RPM owns debug splitting.
- AUR template syntax passed; no Arch ARM build root available. makepkg/namcap,
  Debian 13 and x86_64 native-package matrices remain unrun.

## Original signing demonstration

Default publish was verified inert (empty PATH, no file creation). A real
native-package smoke image was pushed to a loopback registry, signed using a
new ephemeral key and verified with cosign 3.1.4 by digest:

`sha256:e3e074c9210754c0603078fe26031cf19ab41fc7e046f70854ec3dc4ee06b3be`.

That image was **not** the production minimal-LGPL OCI channel. Offline legacy
compatibility/TLS/transparency exceptions were confined to `--local-demo`;
release defaults remain intact. Registry, anonymous task-owned volume, private
key, all W9 Podman resources, `~/md-w9` and `/private/tmp/md-w9-*` were removed
and independently verified absent. Idle builder/install sleep containers needed
Podman's SIGKILL escalation; this was not a product SIGTERM qualification.

## Architecture-profile follow-up

- Fixed the original Intel-only libvpl/QSV blocker. The unchanged three-argument
  builder uses `MD_FFMPEG_ARCH`, otherwise the CC/cross-prefix compiler target.
  Windows/container/Flatpak request QSV only on x86/x86_64 (including i686).
  ARM64 retains NVENC/AMF plus VAAPI or Media Foundation as appropriate.
  Both Linux runtime inventory gates use the same encoder selector.
- Fixture matrix covers 3 profiles × 5 architectures, retaining exact inventory,
  Windows MF patch drift/already-applied guards and LGPL checks. Fixtures are
  wiring evidence, not cross-platform FFmpeg compilation.
- Real ARM64 FFmpeg 8.1.2 build: LGPL-2.1-or-later; H.264 decoder/parser;
  **h264_nvenc, h264_amf, h264_vaapi**, no QSV/libvpl, x264 or OpenH264.
  FFmpeg tar SHA256:
  `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`.
  nv-codec-headers n13.0.19.0 tar SHA256:
  `86d15d1a7c0ac73a0eafdfc57bebfeba7da8264595bf531cf4d8db1c22940116`.
  AMF v1.4.36 headers only, commit
  `16f7d73e0b45c473e903e46981ed0b91efc4c091` (sparse source checkout).
  VAAPI headers 2.20.0-2ubuntu0.2 extracted into the private tree; no host
  packages installed. No hardware encoder/GPU success is inferred.
- Master `426de60` (W3 plus owner W4 backend integrations) merged into W9 via
  `3a385db`, after profile fix `ef367e9`; no application changes by W9.
- Fresh macOS client/tools build against the dedicated nostrc pin: **51 pass,
  0 fail, 1 opt-in AX owned-clicks/native-video skip (77)**. Homebrew dependency
  deployment-target linker warnings remain; this is not a signed Mac release.
- Linux safety tests: **10 pass**. macOS: **9 pass, 1 ELF-fixture skip**.
  FFmpeg packaging fixtures: **6 pass**, including 15 profile/arch cells and
  compiler-target inference/explicit-override coverage.

## Production ARM64 OCI result

**Pinned producer and production agent image built successfully**, not the
native-package substitute used in the first demonstration. Source snapshot:
`0e5ce45ea11ada3c268382a999092712d5951363`; nostrc pin unchanged.
Ubuntu 24.04 base:
`sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8`.
The producer rebuilt nostrc and the host inside that same-architecture base,
with -j2 and a 1500 MB build limit, then staged 71 private dependency DSOs.
The runtime image was independently assembled from that bundle and pinned base.

- **Gates 1–3 PASS locally:** architecture/source/nostrc/base manifest, complete
  SHA256SUMS, no ELF RPATH/RUNPATH, and no missing runtime DT_NEEDED dependency.
- **Gate 4 PASS:** direct MCP invocation without a desktop exits **1**, with
  `host[mcp]: accessibility backend unavailable`; the entrypoint also fails
  closed without a session socket. Launch errors/crashes do not count as passes.
- Actual runtime-image codec probe: **LGPL-2.1-or-later**, one H.264 decoder and
  parser, exactly **NVENC/AMF/VAAPI** encoders; no QSV or bundled OpenH264/x264/x265.
  Host help and payload audit passed; no C/C++ compiler, build orchestration
  tools, source/object/archive payload. readelf/binutils remain for auditing.
- **Gate 5 NOT RUN** (authorized real desktop HTTP/stdio tree).
  **Gate 6 only configuration checked:** no desktop/device/display mounts,
  network none, capabilities dropped, read-only root and no-new-privileges.
  Authenticated-session tool surface/listeners and preparation/session SIGTERM
  are **NOT qualified**. No live GNOME/AT-SPI/uinput access or GPU claim.

OCI manifest digest:
`sha256:169e84d3ffa79fb5e957bd3a0653a0ad62ea4a657e1e6c266e0620a822a4980e`.
Image config ID:
`2758423be6c8cfa68b56c6e6ad8ba509af199a358dc5be22ecb9828644f8fbb0`.
Local archive retained ignored at `artifacts/oci/metadesk-agent-arm64.oci.tar`,
SHA256 `645ce74573e44d9335012e04aa2f56348a0aa61a06d2a738c9c3f6679e046759`.
All six retained artifact hashes were verified after copying. This image was
**not pushed or signed**; the earlier ephemeral-key demo proves plumbing only,
not a signature on this new candidate or a release/owner identity.

Actual clean builds exposed and fixed producer issues rather than relaxing
coverage: relocated FFmpeg **prefix/libdir/includedir** are checked before
linking; merged master's mandatory XInput and bzip2 dependencies are declared;
later dependency walks accept an already-staged FFmpeg DSO only if it byte-
matches the verified input. Tests reject system paths, altered staged copies,
unexpected FFmpeg components, launch failures and wrong failure diagnostics.
Verification cleanup removes names individually: an absent --rm helper cannot
prevent removal of an existing inspected container. Fake-command regression
coverage and an actual Podman create/remove check passed.
Oracle reviewed supplied excerpts only; its no-desktop classification finding
was fixed, not presented as full-tree or release approval.

Raw receipts now use ignored `output/receipts/` or `MD_RECEIPT_DIR` (other
captures under `output/followup/`); only the summary/hash receipt stays in Git.
The image's local qualification legal corpus contains licenses/source pointers
and replacement guidance, **not a reviewed complete release source offer**.
Public distribution still requires owner-curated exact source/version mapping
and corresponding-source/replacement-rights material for private dependencies.

Cleanup independently verified after copying/rehashing artifacts: **0 W9
containers/images/volumes/networks**, `~/md-w9` absent, and
`/private/tmp/md-w9-*` absent. No host packages installed/removed, global prune,
shared checkout mutation, live desktop/device access or new signing keys.
Only assigned-worktree ignored artifacts/receipts are retained.

## Closure / owner prerequisites

Neither bead is closed. md-x7wk **cannot close yet**: local ARM64 gates above pass, but the
x86_64+aarch64 hosted CI matrix, authorized authenticated desktop gates 5/6, and
owner registry/signing/promotion policy remain outstanding. Native md-xs90 additionally retains its
cross-distro/architecture, Arch artifact/checksum and lint-review gates.

Owners must provide apt/Copr hosting and signing keys, immutable source/release
URLs, Arch-native binaries/checksums, a reviewed private-dependency legal/source
and replacement-rights corpus, and the isolated authorized desktop needed for
remaining runtime tests. No synthetic or local-only result replaces these gates.

Tool scope: Jev narrowed flags/dependency context (4 requests, 23,049 input
tokens, about USD 0.00097); direct source reads/builds verified the findings.
