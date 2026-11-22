# Linux packaging

W9 tooling, **not a qualified release**. See [VALIDATION.md](VALIDATION.md) for
actual results and remaining gates. The application is MIT. Native packages link
**system** FFmpeg; OCI requires W3's `packaging/ffmpeg` **container** profile and
its exact codec inventory. Cisco OpenH264 is never copied/downloaded by any tool
here. Do not substitute distro FFmpeg into a production OCI tree.

## Inputs and ownership

Use an isolated matching-architecture Linux build root, not the main checkout.
Tools execute trusted ELFs (`ldd`); never process untrusted inputs. Outputs must
be new directories. Builds use **two jobs**, including upstream nostrc/nsync.
Meson has one native-only install option, `linux_package_rpath` (default false).
It emits the private RUNPATH at link/install time, avoiding destructive ELF
post-processing; OCI retains the existing empty install-RPATH behavior. Paths in commands below are absolute paths chosen by
the operator; `$SRC`, `$WORK` and `$NOSTRC` must not overlap output directories.

Pin metadesk and nostrc by full Git SHA. An exported source tree must contain
`.metadesk-commit` / `.nostrc-commit`, respectively (40 hex characters). Native
spec pins nostrc `cfcb86296068c1e4090bc5ce8a486392dc74eb11` and includes the
complete upstream source export **with its pinned submodules**, not a GitHub
archive with empty submodule directories. Record the archive hashes in release
provenance. Manifests include per-packaging-file hashes, source/nostrc revisions,
architecture, mode, FFmpeg policy and explicit dynamic-module boundaries.

`build-nostrc.sh` builds only required targets. Upstream at that pin forces JSON
shared even with `BUILD_SHARED_LIBS=OFF`. `static-json.py` creates a **private**
build overlay, archiving precisely the objects in CMake's trusted link manifest
(no globbing of stale objects, no editing upstream). Native staging rejects
nostrc/NIP shared dependencies. This overlay currently requires the Unix
Makefiles generator, which `build-nostrc.sh` selects.

Ubuntu/Debian provide nsync; Fedora 44 aarch64 did not resolve `nsync-devel`.
The RPM builds pinned `third_party/nsync` 1.30.0 statically (commit
`5b420ef6b16532a4fad4be1fde8e483b6b7bd8f4`) with its Apache-2.0 notice and
`Provides: bundled(nsync)`. The rest of the linked Linux dependencies are distro
packages; no private FFmpeg, SDL, PipeWire, GLib or TLS stack in native packages.

## Native .deb and apt layout

Install development prerequisites in a disposable distro build root (see the
package list in `Containerfile.producer`; native builds do not need a private
FFmpeg prefix). Then:

```sh
"$SRC/packaging/linux/build-nostrc.sh" "$NOSTRC" "$WORK/nostrc-build"
"$SRC/packaging/linux/static-json.py" "$WORK/nostrc-build" "$WORK/nostrc-static"
"$SRC/packaging/linux/build.sh" native "$SRC" "$NOSTRC" "$WORK/nostrc-static" "$WORK/build"
"$SRC/packaging/linux/produce.py" native "$SRC" "$WORK/build" "$NOSTRC" "$WORK/native"
"$SRC/packaging/linux/debian/build-deb.sh" "$WORK/native/tree" 0.1.0-1 "$WORK/deb"
lintian "$WORK/deb/"*.deb
"$SRC/packaging/linux/debian/apt-repo.sh" "$WORK/deb/"*.deb noble arm64 "$WORK/apt"
```

Build each distro ABI separately (especially Debian 13 vs Ubuntu 24.04); do not
rename an Ubuntu binary tarball to claim Debian/Fedora/Arch compatibility.
`dpkg-shlibdeps` derives real SONAME/version dependencies. A `.deb` is assembled
with `dpkg-deb --root-owner-group`; no root install or service activation is used
on the producer. The apt layout has pool/, Packages/Packages.gz and Release with
hashes. **It is unsigned and must not be publicly published as-is.** The owner
must provide an apt origin/domain/suite policy and repository signing key,
create `InRelease` and `Release.gpg`, securely distribute the public key with
`signed-by=`, and set validity/rotation/retention policy. No key is embedded here.

Native libraries are private under `/usr/lib/metadesk`. Executables use the
Meson-generated, fixed **bundle-relative** `$ORIGIN/../lib/metadesk` RUNPATH; there are **no
producer paths** and no changes to `/etc/ld.so.conf*` or global ldconfig cache.
This is a deliberate native exception to OCI's *no RPATH at all* rule. Native
staging hashes its raw tree; Debian normalizes docs and emits package md5sums.

## RPM / Copr

`rpm/metadesk.spec` is a source-build spec, not a repacked Ubuntu executable.
The RPM passes native-only `--keep-debug`: rpmbuild, not the staging tool,
owns debug splitting/stripping. Create `metadesk-0.1.0.tar.gz` (top directory `metadesk-0.1.0/`, including the
export marker) and `nostrc-<full-sha>.tar.gz` (top directory `nostrc/`, including
its full-sha marker and submodules), place them in rpmbuild/SOURCES, and:

```sh
rpmbuild -ba --define "_topdir $WORK/rpmbuild" "$SRC/packaging/linux/rpm/metadesk.spec"
rpmlint "$WORK/rpmbuild/RPMS/"*/*.rpm "$WORK/rpmbuild/SRPMS/"*.rpm
```

Copr owner must supply the project/account, supported Fedora chroots/arches,
reviewed immutable source URLs/hashes and source uploads, maintainer identity,
release policy, and permission for bundled pinned nostrc/nsync. No Copr API call
is made. Fedora's distro FFmpeg codec coverage is its responsibility; help-smoke
is not hardware/video qualification.

## Arch / Omarchy

`aur/PKGBUILD` is an intentionally **unpublishable template** until the owner
provides **Arch-native** x86_64/aarch64 `produce.py native` tarballs containing
`tree/usr/`, publishes immutable release URLs, and replaces both `OWNER_SHA256_*`
values using the signed artifact digest. `SKIP` checksums are not allowed.
AUR's official Arch target is x86_64; aarch64 is an Arch Linux ARM extension and
needs its own verified build root and package ABI validation.

Run `makepkg --verifysource`, `makepkg --cleanbuild` and `namcap` on both recipe
and resulting package inside those roots before owner publication to AUR. No
AUR SSH key, push, or release download is performed by this lane.

## User service and device access

Packages install `/usr/lib/systemd/user/metadesk-host.service`; they **never
enable/start it**. After configuring the account/signer and desktop session,
the user may run `systemctl --user enable --now metadesk-host.service`.
Import DISPLAY/WAYLAND_DISPLAY and session environment via the desktop's normal
systemd user integration. Use a per-user drop-in to select explicit options.
`NoNewPrivileges=yes` and `UMask=0077` protect the user process/state. The unit
follows graphical-session.target; it is not a system/root daemon and does not
create or substitute a desktop. Installation and package removal do not touch
an existing user's running service.

No udev rule or broad `/dev/uinput` grant is shipped. Device/input enablement is
explicit administrator policy with the Linux-input lane; using accessibility
or observation-only mode must not require a globally writable device. Optional
Wayland input remains a source-build opt-in. No service smoke here exercises
live capture, AT-SPI, or uinput.

## OCI producer and runtime consumer

`Containerfile.producer` uses the pinned Ubuntu 24.04 multiarch digest from the
contract validation. Supply a complete private context containing `src/`,
`nostrc/`, `ffmpeg-prefix/` from the unchanged W3 container-profile build and a
curated `legal/` tree containing **each private dependency's exact notices,
corresponding source/patches/build instructions and LGPL replacement/relink
instructions**. The producer refuses a system/wrong-license/wrong-codec FFmpeg. Copied
pkg-config prefix/libdir/includedir fields are relocated and validated before
compilation; already-staged FFmpeg copies must byte-match their verified inputs.

```sh
podman build --layers=false -t localhost/md-w9-producer:local \
  -f "$SRC/packaging/linux/Containerfile.producer" "$WORK/context"
podman create --name md-w9-export localhost/md-w9-producer:local
podman cp md-w9-export:/output/tree "$WORK/runtime-bundle"
podman rm md-w9-export
# Use a PRIVATE COPY of deploy/containers as the context, not a source checkout.
cp -a "$SRC/deploy/containers" "$WORK/consumer"
cp -a "$WORK/runtime-bundle" "$WORK/consumer/runtime-bundle"
podman build --layers=false -t localhost/md-w9-agent:local \
  -f "$WORK/consumer/Dockerfile.agent" "$WORK/consumer"
"$SRC/packaging/linux/verify-image.sh" localhost/md-w9-agent:local md-w9-gates
```

The image checks SHA256SUMS, the manifest mode, no producer RUNPATH/RPATH and
in-image `ldd`. Runtime helper verifies hashes/closure and fail-closed
no-desktop behavior without any desktop mount. **Gate 5 and authenticated-session
SIGTERM still require a separately authorized matching-UID desktop run.**
Do not treat `verify-image.sh`'s explicit NOT RUN output as release acceptance.
Runtime probe expectations use the same target-architecture encoder selector as
the FFmpeg builder; QSV/libvpl are x86/x86_64-only. `verify-image.sh` writes
receipts under ignored `packaging/linux/output/receipts/`, or `MD_RECEIPT_DIR`.
Keep build/test command captures in that ignored output tree, not in Git.
Run both x86_64 and aarch64 matrices in owner CI; this lane cannot invent
ARM-incompatible SDK profiles or override W3's requested encoder gates.
`reference/throwaway-bundle.sh` remains historical fixture tooling, **not a
release producer**. Existing spike consumer/compose instructions stay historical.

`Containerfile.native-smoke` installs a real native .deb in a disposable image.
It is only package-install/entrypoint/signing-plumbing qualification, **not** the
minimal-LGPL production OCI channel and must never be promoted as such.

## Signed publish: inert unless explicitly executed

```sh
"$SRC/packaging/linux/publish.py" localhost/md-w9-agent:local ghcr.io/owner/metadesk:VERSION
# Prints commands; does not invoke podman/cosign, read keys or touch a registry.
```

Owner CI, **after all contract/legal gates**, supplies registry/repository,
credentials, immutable version/tag and arch-manifest policy, cosign private key
reference + separately trusted verification public key (or a reviewed keyless
OIDC policy), transparency/retention policy and promotion authorization. It can
then opt in with `--execute --key KEY --public-key PUB`. Push receipts resolve
the actual registry digest; both signing **and verification use that digest**,
not a mutable tag. Release mode keeps normal TLS and transparency defaults.

A local demo may use `--execute --local-demo` only for a loopback destination
`127.0.0.1:PORT/md-w9-*` or `localhost:PORT/md-w9-*`. Supply a throwaway key pair
and `COSIGN_PASSWORD`; no real key or registry credentials. Demo alone disables
TLS verification/transparency upload; with tested cosign 3.1.4 it explicitly
selects the deprecated legacy offline format and disables TUF service selection.
These offline compatibility flags are NEVER passed in release mode. It proves crypto/publishing plumbing, not
release trust, hosted CI or owner key policy. See
[cosign signing documentation](https://docs.sigstore.dev/cosign/signing/signing_with_containers/)
and the result summary in VALIDATION.md. Debian packaging conventions follow the
[Debian maintainer guide](https://www.debian.org/doc/manuals/debmake-doc/).
