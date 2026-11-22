# Minimal LGPL FFmpeg profiles

Build with `build-minimal-lgpl.sh PROFILE SOURCE OUTPUT`. SOURCE must be a clean,
extracted, pinned FFmpeg tree. OUTPUT must be a new absolute path outside both
SOURCE and the metadesk worktree. The script interface is unchanged; see its
header for toolchain/header overrides. Windows patches a private source copy,
retains `patches/`, `patches.txt`, `configure-args.txt` and build logs in OUTPUT,
and fails closed if the carried patch no longer applies.

## Windows ARM64 qualification (2026-11-22, md-gml8)

The complete **windows** profile, not a reduced MF-only substitute, was built
natively with MSYS2 CLANGARM64, Clang 23.1.5 (`aarch64-w64-windows-gnu`) at `-j2`.
Source was the official FFmpeg GitHub `n9.0.2` archive:

```
https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n9.0.2
SHA256 6e374ed621e48faa40639307dff48ba6fe574a509977956d2cce9669b7cc27e9
```

The invocation used the existing supported overrides:

```sh
CC=cc AR=llvm-ar MD_FFMPEG_JOBS=2 \
PKG_CONFIG_PATH="$HEADERS/lib/pkgconfig:$PKG_CONFIG_PATH" \
MD_FFMPEG_CFLAGS="-I$HEADERS" \
  sh packaging/ffmpeg/build-minimal-lgpl.sh windows "$SOURCE" "$OUTPUT"
```

Dependencies used for this qualification (not a complete release dependency lock):

- nv-codec-headers 13.1.15.1.2, official FFmpeg master archive retrieved over
  HTTPS; archive SHA256
  `724e122b06cdf0054672716d1ea6958dfbd520ea0307d9029f846692cfb6e245`.
- AMF public headers from official GPUOpen AMF commit
  `57ee0cfb0e1a08d9041f77435653794c1b012be0`.
- Native installed oneVPL 2.17 and the MSYS2 Windows SDK.
- Missing MSYS make/diffutils/patch tools were extracted into the private test
  directory, not installed into or removed from the VM's shared package set.

Configure reported `License: LGPL version 2.1 or later`. `verify-codecs.c`, linked
and run against the installed private libraries, passed the exact inventory:

| Type | Enabled |
| --- | --- |
| Decoder | h264 |
| Parser | h264 |
| Encoders | h264_nvenc, h264_amf, h264_mf, h264_qsv |
| GPL/nonfree/version3, libx264, FFmpeg libopenh264 | disabled |

The existing Windows patch was applied successfully. The caller's unmodified
`libavcodec/mfenc.c` SHA256 remained
`711c964c87fa3590166620c7ad264b3c39812f4f97bbdcdd6fb95a20fbc5fb96`;
the patched private copy was
`1f96132f7e595b1a44d753df2423059da2bbd39406d6dd4396e89e27a611c9a1`.

Metadesk's unchanged `test_encode` was compiled and linked against this minimal
FFmpeg and run with `MD_TEST_ENCODER=h264_mf`: **1,771 checks passed**, including
24 tagged packets, zero emitted after later frames, zero discarded at resize,
and 20 independent reconnect decoders. The existing four-frame pending-tag
assertion was not changed. This qualifies the Windows OS/software MFT path;
compiled inventory does **not** positively qualify NVENC/AMF/QSV on physical GPUs.
This is a library-profile build/runtime check, not a signed installer or a full
Windows application release/dependency-closure qualification.

## Upstream submission handoff — not sent

`patches/upstream/0001-avcodec-mfenc-use-VT_BOOL-for-low-latency-encoder-mo.patch`
is a real `git format-patch` based on FFmpeg master
`359c82515228774eb683dffce9d623b6b4a2fa53`, fetched on 2026-11-22. It changes the
encoder's `CODECAPI_AVLowLatencyMode` variant to `VT_BOOL(VARIANT_TRUE)`; see
[Microsoft's property documentation](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode).
The decoder-specific `VT_UI4` exception does not apply to this encoder property.

`patches/upstream/draft-email.txt` is the accompanying ffmpeg-devel draft.
Nothing was sent. Recheck current master and the author/email before sending.
The master-rebased patch was checked for application/whitespace, not compiled
as a full master build; actual binary qualification above uses `n9.0.2`.
Remove the carried downstream patch only after a vetted FFmpeg release includes
it, updating the source pin and patch-wiring tests together. Do not reverse an
already-applied/fixed patch or silently accept drift.
