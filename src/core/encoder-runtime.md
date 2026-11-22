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

Apply the adjacent **`ffmpeg-mf-low-latency.patch`** when building Windows
FFmpeg, pending an upstream fix (`md-gml8`). With the property changed to
`VT_BOOL(VARIANT_TRUE)`, a private minimal LGPL FFmpeg 9.0.2 build passed all
1,771 encode/decode checks: 24 tagged packets, none late or discarded at resize,
and 20 independent reconnect decoders. Stock MSYS2 FFmpeg is **not qualified**
for the low-latency MF path. Merely consuming an FFmpeg AVOption cannot prove
that the underlying MFT accepted a COM property.

For this MSYS2 SDK, the minimal FFmpeg build also requires
`--enable-d3d11va` with `--enable-mediafoundation` (otherwise `mfenc.c` refers to
undeclared D3D11 types), plus normal build tools including `make` and `cmp`
(`diffutils`). This does not require GPU encoding; `hw_encoding=0` still uses
the OS software MFT. Only the private test dependency was patched; packaging
integration is L15-owned. No GPL codec was enabled in that dependency build.

## Direct OpenH264, not FFmpeg's libopenh264

No OpenH264 library is linked, bundled, installed or downloaded by metadesk.
Only Cisco's public BSD-licensed headers are vendored in `openh264_headers/`,
unmodified from [v2.7.0](https://github.com/cisco/openh264/tree/v2.7.0/codec/api/wels)
with their `COPYING`. The adapter uses the public C++ API behind a C interface;
the upstream C vtable's `ForceIntraFrame` declaration omits the C++ layer argument.

This adapter pins **OpenH264 2.7.0.2610, ABI soname 8**. It resolves
`WelsGetCodecVersionEx`, `WelsCreateSVCEncoder`, `WelsDestroySVCEncoder` and checks
all four version fields **before creating or invoking an encoder**. Other
versions, missing symbols and wrong-architecture libraries are rejected; ABI
updates require a header/version review, not an unversioned library guess.
Version checking is compatibility validation, **not binary authentication**.

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

Choose the matching OS/architecture binary from
[Cisco's 2.7.0 release](https://github.com/cisco/openh264/releases/tag/v2.7.0).
Review its binary license, verify Cisco's published checksum and signature,
decompress it and point `--openh264-path` at the library. To use cache discovery,
rename the POSIX Cisco filename to the ABI filename above. There is currently
**no `--fetch-openh264`**; any future fetcher must require explicit action and
authenticate the download, not run during startup or silently trust a checksum.

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
