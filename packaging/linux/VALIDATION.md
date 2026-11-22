# W9 local Linux packaging evidence — 2026-11-22

**Not a release qualification. Neither md-x7wk nor md-xs90 can close.**
No push, merge, Beads closure, hosted CI, public repository publication or real
signing key was used. Main and sibling checkouts were not edited/built.

## Environment and provenance

Assigned branch `md/w9-linux-packages`, application baseline
`0e91e463b2baff1fca2ff6daca1262bdaa1d5858`; W9 packaging/install changes are
identified by per-script hashes in the manifests and the retained source RPM.
Artifacts are local development snapshots, not artifacts built from a release
tag. The later test/docs/help-wording edits are not a claim to have rebuilt the
recorded snapshots. Rebuild from the committed source before any promotion.

Ubuntu 24.04 aarch64 VM, rootless Podman 4.9.3, private `~/md-w9` tree. Read the
three requested bd memories first; only read the dedicated nostrc-master input,
never modified/rebuilt it. nostrc full pin:
`cfcb86296068c1e4090bc5ce8a486392dc74eb11`. Source archives include submodules.
Native system FFmpeg: Ubuntu libavcodec 60; Fedora 44 libavcodec 62. Both packages
have static nostrc/JSON. Fedora lacks nsync-devel and builds pinned nsync 1.30.0
statically; Ubuntu uses its system nsync. Builds used -j2; final Fedora LTO and
find-debuginfo workers also explicitly limited to 2.

Ubuntu base digest:
`sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8`.
Fedora image pulled locally, renamed to the W9 prefix, base image ID
`ff918341dfc920e8694e2079a68792627b0d2b3cf883fd3bce9f409a8a1289cc`.
No desktop, host session bus/AT-SPI socket, display/PipeWire socket or uinput
mount was used. No live input/capture or GUI qualification is claimed.

## Built artifacts

Retained locally, **ignored, not committed**, under `packaging/linux/artifacts/`:

- `metadesk_0.1.0-1_arm64.deb` — SHA256
  `417cd5fe019c848e4e2fcab3aa650e6c0bfb52e46a986428ecebd84700f71b15`.
- `metadesk-0.1.0-1.fc44.aarch64.rpm` — SHA256
  `e67808d7b37d01a8e1e5c0767fb86b15c1271e6fc8d8ac0a9ab8af07f397ca3c`.
- Matching `.src.rpm`, `-debuginfo` and `-debugsource` RPMs.
- `apt-repo/` pool + noble/arm64 Packages, Packages.gz and unsigned Release.

All five native package hashes were checked after copying from the VM; see
[evidence/artifact-sha256.txt](evidence/artifact-sha256.txt). The runtime tree,
VM build outputs, local registry/key and all W9 images are disposable and are
removed, not promoted. Native binaries contain no Cisco OpenH264 library.

## Tests and lint

| Check | Actual result |
|---|---|
| Ubuntu native host/client build | PASS, including final Meson-native RUNPATH relink |
| Ubuntu `.deb` assembly | PASS with real dpkg-shlibdeps dependency metadata |
| Clean Ubuntu image dependency install + host/client `--help`, ldd | PASS |
| Final `.deb` remove/reinstall in that disposable Ubuntu root | PASS; same runtime dependencies; help/closure pass |
| Local apt layout, network disabled, file: repo | PASS update + install + both help commands; explicit trusted=yes only for this **unsigned local fixture**, not release trust |
| lintian | exit 0, **0 errors, 1 warning**: initial-upload-closes-no-bugs; no bogus Debian bug number or suppression added |
| Fedora 44 source `rpmbuild -ba` | PASS, clean final rebuild; runtime, source, debuginfo and debugsource RPMs generated |
| Clean Fedora 44 image install | PASS, DNF resolved the runtime dependencies; no development dependencies installed there |
| Final RPM replace/reinstall + `rpm -V`, both help, ldd | PASS |
| Installed systemd user unit static verify in Fedora container | PASS using systemd-analyze --user; unit never enabled/started |
| rpmlint 2.8.0, four packages + spec | exit 0, **0 errors, 5 warnings**, 0 badness; see below |
| Packaging safety tests | Linux **8 pass**; macOS 7 pass / 1 ELF-only fixture skip |
| Shell syntax/ShellCheck, PKGBUILD bash syntax, Python compile, git diff --check | PASS |
| Arch makepkg/namcap | **NOT RUN**: official Arch image manifest advertises amd64 only, no native aarch64 root provided |
| Debian 13 / x86_64 package matrices | **NOT RUN**; Ubuntu binaries are not passed off as Debian/Arch/Fedora binaries |

The Ubuntu slim image intentionally excludes documentation/man pages through
its dpkg configuration. Its `dpkg --verify` reports those excluded files; this
is **not** claimed as a complete payload-integrity pass. The package archive
contains the docs/man pages (build-summary receipt); Fedora `rpm -V` passed.
Early static unit verification on the VM lacked an installed executable, and
--user/--root verification was unsupported there; those attempts are not passes.
The installed Fedora unit static check above is the qualifying syntax result.

RPM warnings: Source0/Source1 local filenames each reported twice because both
SRPM and explicit spec were linted (**four URL warnings**); immutable hosted
source URLs are owner input, not invented. **One OpenSSL crypto-policy warning**
flags `SSL_CTX_set_cipher_list` in libmetadesk. This needs review of the pinned
static dependency's Fedora crypto-policy behavior; it is neither suppressed nor
claimed to prove an exploitable defect. No fully lint-clean result is claimed.

Meaningful packaging fixes during qualification:

- Removed global ldconfig configuration; use a private relative RUNPATH.
- Put nostrc/dependency build inputs outside the Meson source directory.
- Let RPM own debug splitting; do not pre-strip RPM input.
- Fedora patchelf 0.18.0 rewrites damaged executable ELF notes/build IDs. Native
  staging now does **no patchelf rewriting**. One opt-in Meson install option
  emits `$ORIGIN/../lib/metadesk`; normal Linux/OCI and Darwin defaults preserved.
  Final installed Fedora ELF retains its build ID and expected RUNPATH.
- Corrected RPM metadata lint errors rather than suppressing them.

## OCI producer and signing

The unchanged W3 container-profile FFmpeg builder was actually attempted using
FFmpeg 8.1.2, verified tar SHA256
`464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`.
It **fails closed**: `ERROR: libvpl >= 2.6 not found` on this aarch64 build root.
No encoder flag removed, FFmpeg source/profile edited, or system codec closure
substituted. Therefore **the production minimal-LGPL runtime tree and agent
image were NOT built/qualified**. The pinned producer Containerfile and new
production-input consumer gates remain unqualified until W3/owner supplies a
working matching-architecture SDK/profile and curated dependency legal/source
payload. No successful full producer/consumer matrix is claimed.

A real **native-package smoke image**, not that production channel, was built
locally: `localhost/md-w9-native-smoke:noble`, image ID
`4e979af790ebf66fe3636be0fc55ad658a5a5152c9126ce98ea3006d72fbfe59`.
It installed the actual Ubuntu native package and runtime dependency closure.
Direct no-desktop MCP failed exit 1 with `accessibility backend unavailable`;
entrypoint failed exit 1 with `missing session bus socket`, rather than starting
a substitute service. No host desktop mounts or devices were involved.

Default publish dry run printed podman/cosign commands without executing them.
An offline regression test uses an empty PATH and checks no file creation;
release dry-run output contains no insecure demo flags.

Practical local demo: cosign **3.1.4**, downloaded binary SHA checked against its
upstream checksum list; new password-protected **throwaway** key pair;
loopback registry at `127.0.0.1:15009`, W9 image/repository/container prefix.
Push, sign and verification succeeded for:

`127.0.0.1:15009/md-w9-metadesk@sha256:e3e074c9210754c0603078fe26031cf19ab41fc7e046f70854ec3dc4ee06b3be`

Actual cosign receipt validated claims and signature against the specified
public key. Demo uses explicitly deprecated legacy/offline compatibility flags
for cosign 3.1.4, no TUF public service selection or transparency upload; normal
TLS/transparency defaults remain intact outside `--local-demo`. The first v3
attempt exposed its changed signing-config default; the explicit offline demo
configuration fixed that mismatch, not a retry-to-green or release-policy bypass.
This tests publishing/crypto plumbing only, **not** production image legality,
owner identity, public registry credentials, public transparency, or promotion.

Contract coverage: static RPATH rejection fixture passes; native smoke exercises
closure/no-desktop failure only. **Production gates 1–6 as a whole do not pass**.
Gate 5 (authenticated HTTP/stdio real application tree) was prohibited by lane
isolation. Gate 6 authenticated-session/no-listener/tool-surface/SIGTERM behavior
was not qualified. No live/cross-architecture/hosted evidence is inherited from
the earlier container spike.

## Owner prerequisites / remaining work

- W3-approved working minimal LGPL FFmpeg SDK/profile for each architecture,
  exact private dependency licenses/corresponding sources/replacement material,
  then real pinned producer + production image build and all contract gates.
- Authorized isolated desktop gate 5/6 session; x86_64 + aarch64 hosted matrix.
- Registry/repository + credentials, cosign signing/verification policy/keys,
  tag/digest and multiarch promotion policy. No real keys used by W9.
- Apt origin/domain/suites + repository key/public-key distribution and signed
  InRelease/Release.gpg; Copr project/chroots/source uploads/immutable URLs and
  maintainer approval. Reconcile the Fedora crypto-policy lint warning.
- Arch-native release tarballs, authenticated immutable URLs and real per-arch
  SHA256 values in PKGBUILD, an Arch/Arch Linux ARM build root, makepkg/namcap
  qualification; Debian 13 and x86_64 package/install/lint matrix.
- Service start/device grants, actual capture/encode/GPU and live desktop
  acceptance remain separate, explicitly authorized runtime work.

## Review and tool scope

Oracle reviewed **only supplied actual code excerpts**, not the unbound tree or
main checkout. Three findings fixed: disallow all unexpected FFmpeg components,
remove any stale JSON archive before constructing it, require explicit registry
qualification. Regression coverage includes archive-member removal and inert
publish behavior. No full-tree Oracle approval is claimed.

Jev: 3 find-lines calls / 8 requests, 66,651 input tokens, about USD 0.00280;
answered probabilities .99/.97/.93 narrowed plan/notices/Meson context. No
judgment was used to authorize deletion, publication or a security exception.

Cleanup receipt is recorded in [evidence/cleanup.txt](evidence/cleanup.txt).
