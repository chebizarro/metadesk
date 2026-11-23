# Portable client AppImage (md-pqc7)

Client only, Linux native architecture. No host/service, compiler, input access,
OpenH264, x264 or x265 in the image. Build on the oldest supported glibc baseline
(Ubuntu 24.04 / glibc 2.39 for the measured ARM64 candidate). glibc and the ELF
loader remain system-owned. Desktop/driver dlopen behavior is not established by
an offline help/decode smoke; see `VALIDATION.md` for the exact matrix.

## Producer

Use a private directory, a clean metadesk export, and complete nostrc source at
`6f7a8d025768d497bef700fe55eea1c687dac328` (including its pinned submodules if
using `packaging/linux/build-nostrc.sh`). Never consume a shared build tree.
Ubuntu native build dependencies are listed in `packaging/linux/Containerfile.producer`;
add VAAPI/DRM, NV codec and AMF **headers**, not vendor GPU runtimes. The canonical
`container` FFmpeg profile is also the Linux portable-image profile; x86_64
additionally needs oneVPL, ARM64 must not acquire QSV.

```sh
# All variables below are absolute, private paths. FFmpeg output must be absent.
PKG_CONFIG_PATH="$HEADERS/lib/pkgconfig" MD_FFMPEG_CFLAGS="-I$HEADERS/include" \
  sh "$SRC/packaging/ffmpeg/build-minimal-lgpl.sh" container "$FFMPEG_SOURCE" "$FFMPEG_OUT"
sh "$SRC/packaging/linux/build-nostrc.sh" "$NOSTRC" "$NOSTRC_BUILD" "$NOSTRC_PREFIX"
export PKG_CONFIG_PATH="$FFMPEG_OUT/install/lib/pkgconfig"
export LD_LIBRARY_PATH="$FFMPEG_OUT/install/lib:$NOSTRC_PREFIX/lib"
sh "$SRC/packaging/flatpak/build-product.sh" "$SRC" "$NOSTRC_PREFIX" "$BUILD" "$PREFIX" client
python3 "$SRC/packaging/appimage/assemble.py" "$PREFIX" "$FFMPEG_OUT/install" "$APPDIR" --arch aarch64
sh "$SRC/packaging/appimage/make-image.sh" "$APPDIR" "$IMAGE" "$APPIMAGETOOL" "$TYPE2_RUNTIME"
sh "$SRC/packaging/appimage/dry-sign.sh" "$IMAGE" "$NEW_PRIVATE_SIGNING_DIR"
```

Download tools to a private directory and verify `tools.lock.json` **before
execution**. These are versioned ARM64 inputs, not overwritten `continuous`
assets. x86_64 requires its own reviewed tool/runtime hashes. `make-image.sh`
uses an explicit type-2 runtime and does not fetch anything itself.
`appimagetool` supports extract-and-run for FUSE-less builders. The resulting
image supports `--appimage-extract` and `APPIMAGE_EXTRACT_AND_RUN=1`; native
FUSE mounting must be checked separately on target desktops.

`assemble.py` verifies the exact codec inventory before walking the dependency
closure, rejects forbidden codecs and system FFmpeg substitution, removes
producer RPATHs, and records original and packaged DSO hashes in `bundle.json`.
Its launcher scopes private libraries to this application. SDL2 from the measured
Ubuntu build is real SDL2 (not the SDL3 compatibility wrapper).

## Smoke and compliance

The image carries `usr/libexec/metadesk/{verify-codecs,decode-smoke}` and a
643-byte synthetic H.264 frame. The latter calls the actual shipped
`md_decoder_*` API and checks one 16x16 RGBA frame, including libyuv conversion;
it is not merely FFmpeg's version/help output. `tests/make-fixture.c` generated
the input offline using distro FFmpeg/libx264; no generator or encoder library
is copied into the artifact. Fixture SHA256:
`de75b60e3df4da3dde9596f446582f56ac07891a6c39741bf84ba64b9e627bb2`.

The local GPG/zsync demonstration uses a new disposable identity, deletes its
private key, and writes an intentionally invalid update URL. No release key,
public endpoint, upload, or production signing claim. Detached signatures must
be verified by the owner-distributed release key before production use; zsync
metadata is not itself a signature.

**Not cleared for distribution:** stage a reviewed copyright/license corpus for
all DSOs, exact corresponding sources and build recipes alongside the artifact,
and the root third-party notices. Preserve LGPL replacement/reverse-engineering
rights: extract the image, replace compatible shared LGPL libraries, run
`AppRun` (or rebuild the image with the provided tool). Include the exact FFmpeg
source archive/configure receipt, LGPL text, patches and concrete release source
URLs. The image already carries project notices/FFmpeg LGPL text, but that alone
is not a complete source offer. No public release/publishing is implemented.
