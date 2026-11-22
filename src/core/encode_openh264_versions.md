# Reviewed OpenH264 ABI set

The adapter uses the v2.7.0 public C API. Accept **only** the complete version
quadruples `2.6.0.2502` and `2.7.0.2610` in
`encode_openh264_runtime.h`, not a numeric interval and not soname 8 in general.
A new release requires the same review before adding its exact tuple.

## Header comparison (2026-11-22)

Files were retrieved from Cisco's immutable release tags:

- https://github.com/cisco/openh264/tree/v2.6.0/codec/api/wels
- https://github.com/cisco/openh264/tree/v2.7.0/codec/api/wels

`codec_api.h`, `codec_app_def.h`, and `codec_def.h` are byte-identical between
these tags. Their SHA-256 values, respectively, are:

```
21f29b20c24f7c7946f2e243d0bc2532fb3542f6c28af338209477e70d9036c9
a40581a24263866dca19911928f7bc4eb354ff78d9dd56dbf0f55fc4fd923726
f974d269b5935e8dc7265b8bfc02f60e5185b4d6165d30541d2758a4506f1979
```

Thus the encoder vtable order, enum values, callback signatures, and layouts
of `SEncParamExt`, `SSourcePicture`, `SLayerBSInfo`, and `SFrameBSInfo` are
unchanged. `codec_ver.h` differs only in the minor/build tuple and string.
Both releases' upstream C `ForceIntraFrame` declaration omits the layer
argument present in the C++ interface and implementation; retain our documented
C declaration correction and pass `-1`. See `openh264_headers/README.md`.

**2.5.x is not compatible**: comparing v2.5.0 shows added PSNR members in
`SEncParamExt`, `SSourcePicture`, and `SLayerBSInfo` by v2.6.0. A matching major
version is not layout evidence. 2.4.1 (soname 7) also remains rejected.

## Qualification

Cisco 2.6.0 and 2.7.0 ARM64 binaries are tested on the available macOS host,
Linux VM and Windows VM with the production C adapter and unchanged
`test_encode`: 1,771 checks for each version/platform, including bitrate changes,
IDR requests, PTS/geometry tags, resize, and 20 independent reconnect decoders.
The offline suite tests exact accepted/rejected tuples; fake libraries for
2.5.1 and an unvetted 2.7.0 build abort in encoder creation, proving rejection
occurs before creating or invoking an encoder. Existing missing-library and
99.x rejection tests remain.

This is native ARM64 runtime qualification, **not** x86/32-bit hardware test
coverage. Header identity applies to those ABIs, but their runtime behavior
needs native release gates. Source-build compatibility does not establish
Cisco-distributed-binary patent coverage.

The explicit fetcher deliberately downloads **only 2.7.0**, with pinned
Cisco-published SHA-256 over verified HTTPS. Older 2.6.0 release notes publish
MD5, not SHA-256; 2.6.0 test binaries were downloaded from Cisco over verified
HTTPS and checked against those published MD5 values (plus platform signature
checks where available). MD5 is not an acceptable new fetcher trust pin, and
no automatic 2.6.0 download path is provided. Older binary test provenance must
not be confused with the 2.7.0 fetcher's pinned SHA-256 authentication policy.
