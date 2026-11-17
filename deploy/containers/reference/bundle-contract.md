<!-- validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture consumption; see validation.md. -->
# Tier A runtime bundle contract

This is the md-kb7 handoff. `throwaway-bundle.sh` is a **throwaway local producer**,
not supported packaging, a release pipeline, or a reproducible-build claim.

## Producer and input

Build on Ubuntu 24.04 with the same architecture as the runtime base. Validation
uses aarch64, metadesk `8157ac0`, and the existing `/home/bizarro/nostrc/_build`.
The producer needs Meson/Ninja, the normal metadesk development dependencies,
`patchelf`, `ldd`, and Python. The image never compiles source.

From an isolated copy (not `~/metadesk`):

```sh
./deploy/containers/reference/throwaway-bundle.sh \
  "$PWD" /home/bizarro/nostrc "$PWD/deploy/containers/runtime-bundle"
```

Both output and `build-container` must initially be absent. The script configures
`-Dclient=false --prefix=/opt/metadesk --libdir=lib`, builds with two jobs, and
runs `meson install --destdir`. This is **not a media-free build**: the shared
library still links media dependencies. It resolves trusted local ELFs with
`ldd`, copies the transitive non-base dependency closure, dereferences private
library symlinks, strips RPATH/RUNPATH with `patchelf`, and writes `SHA256SUMS`.
Never run this producer on untrusted binaries.

## Image input

```text
runtime-bundle/opt/metadesk/
  bin/metadesk-host
  lib/libmetadesk.so* + complete private non-base DT_NEEDED closure
  SHA256SUMS
```

Base-owned: glibc and its loader/companion libraries, libgcc, libstdc++, and CA
certificates. The Dockerfile adds Python and GLib D-Bus CLI tooling. No client,
compiler, source/build tree, keys, bearer token, session socket, desktop state,
or screencast restore token belongs in the bundle. Library lookup uses
`/etc/ld.so.conf.d/metadesk.conf` plus `ldconfig`, not build-tree RPATHs.

The closure check covers linked ELFs, **not arbitrary dlopen features**. The
validated observation path uses GLib/GIO D-Bus and AT-SPI; its real tree smoke
must pass in-image. Optional media/GPU/portal/NSS integrations and their dynamic
modules are not qualified. Packaging must enumerate/retest any newly enabled
runtime modules rather than treating ldd alone as proof.

## Consumer gates

1. Pin Ubuntu by digest and record architecture, source revision, and hashes.
2. Verify `SHA256SUMS` and inspect `readelf -d` for every ELF: no producer RPATHs.
3. In the actual image, run `ldd /opt/metadesk/bin/metadesk-host`; **no `not found`**.
   The Dockerfile enforces this during assembly; validation repeats it at runtime.
4. With no desktop, direct host invocation must fail nonzero with a diagnostic,
   not start a substitute service or hang.
5. Attach exact filesystem session socket and discovered AT-SPI **directory**;
   authenticate as the same host UID (Podman `--userns=keep-id`). Test both
   authenticated HTTP and stdio against a known accessible application.
6. Confirm no 7700 listener, input/capture tool exposure, devices, or display/
   PipeWire mounts; test SIGTERM including the preparation phase.

Only local images are produced. No registry publishing, signing, promotion,
FIPS, video deployment, or Kubernetes is provided here. Delete the disposable
bundle/build directories after testing; keep evidence, not binary artifacts.
