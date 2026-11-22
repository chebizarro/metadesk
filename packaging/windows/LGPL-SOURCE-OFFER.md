# Source, licenses and replacement

Metadesk is MIT licensed; see LICENSE and THIRD_PARTY_NOTICES.md. Dependencies
retain their own licenses. The `licenses/` directory carries upstream texts;
`runtime-manifest.json` identifies every private PE file and MSYS2 package.
No GPL-only codec DLL or OpenH264 binary is included.

FFmpeg is LGPL-2.1-or-later, dynamically linked. The installed
`ffmpeg-corresponding-source.tar.gz` contains the exact patched source, the
Windows profile, patch, configure arguments and configuration receipts used to
build it. Rebuild in a native MSYS2 CLANGARM64 shell with Clang, make, patch,
diffutils, pkgconf, the Windows SDK and oneVPL 2.17 development files installed.
Extract the archive into a new directory and run the following there. The recipe
expects its original project-relative layout, recreated below. The archive
contains patched source, so reverse the patch before the recipe applies it to
its own private copy. `build-headers` includes the exact AMF/NVIDIA headers;
explicit include paths relocate this header-only dependency without needing
the original build receipt's absolute install prefix to exist.

```sh
mkdir -p producer/packaging
mv metadesk-recipe producer/packaging/ffmpeg
patch -R -p1 -d ffmpeg < producer/packaging/ffmpeg/patches/0001-mf-low-latency-vt-bool.patch
root=$(pwd -P)
CC=cc AR=llvm-ar MD_FFMPEG_JOBS=2 \
  MD_FFMPEG_ARCH=aarch64 MD_FFMPEG_TARGET_OS=mingw32 \
  PKG_CONFIG_PATH="$root/build-headers/lib/pkgconfig" \
  MD_FFMPEG_CFLAGS="-I$root/build-headers -I$root/build-headers/include" \
  sh producer/packaging/ffmpeg/build-minimal-lgpl.sh windows \
    "$root/ffmpeg" "$root/rebuilt"
```

The publisher must
also host this archive beside the MSI and link it from the release notes.

You may replace avcodec/avutil and other LGPL DLLs with ABI-compatible modified
builds in the installation directory. Exit the host/client, back up the original
DLLs, then replace them. MSI repair/upgrade restores the packaged versions.
No restriction on reverse engineering for debugging modifications is imposed.
FFmpeg is not statically linked; application relink objects are not required.

Other private LGPL components (libiconv, libintl, libidn2 and libunistring)
are identified in `runtime-manifest.json`. The library license, not a GNU tool's
license, applies to these DLLs; LGPL is selected for dual-licensed libraries.
The upstream libidn2 2.3.8 archive is additionally installed under `sources/`.
Their exact versions and upstream source URLs are in its retained MSYS2 package
metadata. The publisher must retain and serve their exact corresponding source
and patches/build recipes at the same release before distribution; a link to
an upstream homepage or this statement alone is NOT a completed LGPL source
supply. This qualification package is not authorized for public distribution
until those source archives and the source-pinned dependency receipts are
reconciled. See VALIDATION.md in the source tree for remaining release gates.
