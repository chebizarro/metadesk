# H.264 encoder runtime policy

The first backend which opens and accepts its low-latency options wins:

| Platform | Order |
| --- | --- |
| macOS | NVENC (if requested), VideoToolbox, direct OpenH264, optional libx264 |
| Linux | NVENC (if requested), VAAPI, QSV, direct OpenH264, optional libx264 |
| Windows | NVENC (if requested), AMF, Media Foundation, direct OpenH264, optional libx264 |

`--no-nvenc` skips only NVENC in automatic mode. `--encoder NAME` requires exactly that backend
(for example `openh264` or `h264_mf`), overriding the NVENC preference; failure
does not select another backend.
The corresponding library config field is `MdEncoderConfig.encoder_name`.
The reported name is the selected codec, not a generic "NVENC"/"x264" label.
Media Foundation permits the OS software MFT (`hw_encoding=0`); its name is
`h264_mf` and it is conservatively classified as OS, not GPU hardware.
VAAPI creates a device/frames context and uploads CPU NV12 frames; QSV accepts
CPU NV12 with async depth 1 and lookahead disabled. All FFmpeg paths disable
B frames and retain the same monotonically increasing PTS-to-geometry-tag table,
including uploaded frames. Hardware presence alone is not a successful open.

## Windows FFmpeg integration requirement

FFmpeg 9.0.2's `mfenc.c` passes `CODECAPI_AVLowLatencyMode` as `VT_UI4`.
[Microsoft specifies `VT_BOOL` for the H.264 encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode)
(the decoder is the `VT_UI4` exception). On the Windows ARM64 VM the unpatched
OS software MFT opened/decoded but buffered ten frames, failing the unchanged
geometry-tag test's maximum pending-depth assertion. Do not relax that assertion.

The Windows profile of `packaging/ffmpeg/build-minimal-lgpl.sh` applies
`packaging/ffmpeg/patches/0001-mf-low-latency-vt-bool.patch` to a private source
copy, preserving the caller's clean FFmpeg tree and retaining a patch receipt.
Upstreaming remains pending (`md-gml8`). With the property changed to
`VT_BOOL(VARIANT_TRUE)`, a private minimal LGPL FFmpeg 9.0.2 build passed all
1,771 encode/decode checks: 24 tagged packets, none late or discarded at resize,
and 20 independent reconnect decoders. Stock MSYS2 FFmpeg is **not qualified**
for the low-latency MF path. Merely consuming an FFmpeg AVOption cannot prove
that the underlying MFT accepted a COM property.

For this MSYS2 SDK, the minimal FFmpeg build also requires
`--enable-d3d11va` with `--enable-mediafoundation` (otherwise `mfenc.c` refers to
undeclared D3D11 types), plus normal build tools including `make` and `cmp`
(`diffutils`). This does not require GPU encoding; `hw_encoding=0` still uses
the OS software MFT. The Windows packaging profile enables D3D11VA explicitly. No GPL codec was enabled in that dependency build.

## Direct OpenH264, not FFmpeg's libopenh264

No OpenH264 library is linked or bundled. The standalone `--fetch-openh264`
action can download and install Cisco's binary; normal startup never does.
Only Cisco's public BSD-licensed headers are vendored in `openh264_headers/`,
from [v2.7.0](https://github.com/cisco/openh264/tree/v2.7.0/codec/api/wels)
with their `COPYING`. The backend and loader fake are plain C, using
`ISVCEncoderVtbl`; no C++ source or link-language override is added to the core.
The sole local header correction restores the omitted `ForceIntraFrame` layer
argument to match the actual ABI; see `openh264_headers/README.md`.

This adapter accepts exactly **2.6.0.2502 and 2.7.0.2610, ABI soname 8**.
It resolves `WelsGetCodecVersionEx`, `WelsCreateSVCEncoder`, and
`WelsDestroySVCEncoder` and checks all four version fields **before creating
or invoking an encoder**. Missing symbols, other builds, and wrong-architecture
libraries are rejected. A shared soname or arbitrary 2.x version is not enough.
See [the ABI review](encode_openh264_versions.md) for header hashes and real
round-trip qualification. The fetcher pins only 2.7.0 and authenticates bytes;
a loader version check alone is compatibility validation, **not authentication**.
Source-built distro codecs do not acquire Cisco binary patent coverage.

Search order:

1. `--openh264-path /absolute/library/file` / `MdEncoderConfig.openh264_path`.
2. `METADESK_OPENH264_PATH`, if no config path was supplied.
3. System loader path, using `libopenh264.so.8` on Linux,
   `libopenh264.8.dylib` on macOS, or Cisco's
   `openh264-2.7.0-win-arm64.dll` / `-win64.dll` / `-win32.dll` on Windows.
   POSIX uses `dlopen(RTLD_NOW | RTLD_LOCAL)`; Windows uses UTF-8-to-wide
   `LoadLibraryExW(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`, **not CWD/PATH DLL search**.
4. The same filename in the per-user cache:
   - Linux: `$XDG_CACHE_HOME/metadesk/openh264/` (absolute XDG path only),
     otherwise `$HOME/.cache/metadesk/openh264/`.
   - macOS: `$HOME/Library/Caches/metadesk/openh264/`.
   - Windows: `%LOCALAPPDATA%/metadesk/openh264/`.

An explicit path is authoritative: if invalid, the OpenH264 attempt fails
without substituting a different library. Normal chain selection can still
continue to optional libx264; `--encoder openh264` prevents that fallback.
Config strings are copied and survive resize. Library handles are per encoder;
destroy calls Uninitialize/destroy **before** unloading, including failed opens.

NV12 is converted to I420 with libyuv. OpenH264 is configured for realtime screen
content, one spatial/temporal layer and one thread, no B frames. EncodeFrame is
synchronous: its aggregated Annex-B NALs form one packet with the submitted PTS,
and the common PTS/tag table attributes the packet. Rate-control skips emit no
packet and never transfer their tag to a later frame. Flush has no delayed
output. IDR requests, resize and runtime bitrate updates use the direct API.

## Installing the fallback

Run this **standalone, explicit user action**:

```sh
metadesk-host --fetch-openh264
```

It prints the complete [Cisco binary license](https://www.openh264.org/BINARY_LICENSE.txt)
and flushes it before DNS/network activity. The command itself is the explicit
action (no hidden startup fetch, updater, or acceptance inferred from encoder
selection). Read that notice before choosing to run it. Mixed CLI options are
rejected. Unsupported platforms fail closed.

The URL is fixed to `https://ciscobinary.openh264.org/` with the matching Cisco
2.7.0 platform/architecture filename from the
[official release](https://github.com/cisco/openh264/releases/tag/v2.7.0).
macOS supports arm64/x86_64, Windows arm64/x86_64/x86, and Linux
arm64/arm/x86_64/x86. Normal CA/hostname TLS verification stays enabled;
redirects are rejected. Standard curl HTTPS proxy/CA configuration remains
available; no arbitrary artifact URL, digest override or insecure switch exists.

`encode_openh264_fetch.c` pins the **decompressed signed library's SHA-256**
from Cisco's matching `<artifact>.signed.sha256.txt` file retrieved over
verified HTTPS. This is the published-checksum route, not a claim of detached
signature verification. Pins are trust roots reviewed with the source: never
replace them with a checksum downloaded at runtime alongside the artifact.
The full binary license is retained in `encode_openh264_license.h`.

Download size is capped at 8 MiB, output at 16 MiB. Partial, concatenated,
trailing or corrupt bzip2 streams and digest mismatches are rejected before
any cache write or library load. The verified binary is written to a random,
exclusive private temporary file, flushed, and atomically renamed to the
loader's cache filename. On POSIX the installer walks directory descriptors
without following symlinks, rejects macOS extended allow ACLs (including inherited
grants), requires safe owned ancestors and private 0700
metadesk/openh264 directories, and writes a 0600 library. On Windows it rejects
reparse points, validates ownership/mutation ACLs through opened handles,
and holds every ancestor without delete sharing through publication; mutation
ACLs exclude untrusted reparse changes and child deletion.
It creates protected current-user-only DACLs for cache directories/files and
uses the wide Windows environment APIs, including non-ASCII profile paths.
Unsafe pre-existing cache permissions fail closed rather than being modified.
An interrupted/failed transfer never replaces the previous cached binary.
The extra host dependency is libbz2 (system library on macOS); OpenSSL and
libcurl were already required. No FFmpeg profile/script interface changed.

Manual installation with an explicit absolute path is still supported. Verify
Cisco's checksum/signature yourself, decompress and point `--openh264-path` at
the library; for cache discovery use the ABI filename above. Only the explicit
fetch command performs the pinned-checksum install flow.

Example diagnostic runs (the tests also accept `MD_TEST_ENCODER=openh264`):

```sh
metadesk-host --encoder openh264 --openh264-path /path/to/libopenh264.so.8
METADESK_OPENH264_PATH=/opt/homebrew/opt/openh264/lib/libopenh264.8.dylib \
  MD_TEST_ENCODER=openh264 ./build/test_encode
```

Homebrew/distro source-built OpenH264 is useful for ABI and round-trip testing,
but **does not establish Cisco binary patent coverage**. Production fallback
instructions point to Cisco's own distribution, not a repackaged or statically
linked library. Bundled FFmpeg remains LGPL-only; libx264 is only attempted if
the user/system FFmpeg already supplies it. This runtime adapter cannot make a
GPL FFmpeg build LGPL.
