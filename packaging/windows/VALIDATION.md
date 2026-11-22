# W10 Windows MSI validation — 2026-11-22

## Result and boundary

**ARM64 packaging/install/upgrade/uninstall and winget schema validation: PASS.**
**Trusted signing qualification: NOT COMPLETE.** A timestamped self-signed
fixture was produced, but the strict Authenticode trust gate failed. This is
not a public release, factory-clean VM qualification, hosted winget acceptance,
x64 qualification, or authenticated capture/stream qualification. `md-jsrk`
remains open; x64 follow-up is **md-sfxh**. Nothing was pushed or merged.

Source: assigned `md/w10-windows-msi`, base `1a3cc8e774ceb9f973acc4e6cd5737db0b33756f`.
Only `packaging/windows/` was changed. No main checkout build/edit, sibling
source change, or `packaging/ffmpeg/` edit was made.

## Environment and tools

- VM `192.168.64.4`: Windows 11 Pro `10.0.26300`, ARM64, 4 GB RAM.
- Private source/build tree: `C:\Users\bizarro\md-w10`; all compiles `-j2`.
- MSYS2 CLANGARM64: clang 23.1.5, CMake 4.5.0, Meson 1.12.1, Ninja 1.13.2.
- WiX **5.0.2+aa65968c**, x64 tool under emulation, ARM64 MSI target. Official
  `wix-cli-x64.msi` was administratively extracted privately, not registered:
  ProductCode `{64F01EA3-1A9A-46D0-BAA9-BCB2EE6C73A7}`, ProductState `-1`.
  Download SHA256 `097383b2773bdd3d76a6a2cd3f4de6357bae59172e1b6fa5ad87ce062074e8d0`.
- Missing MSYS make 4.4.1-3, diffutils 3.12-1, patch 2.8.1-1 were extracted into
  the private tree. No pacman installation/removal transaction was performed.
- Signing tool: SDK `10.0.26100.0/arm64/signtool.exe`. Unsigned development
  PowerShell scripts ran with **process-only** execution-policy Bypass.

## Reproducibility and closure

`fetch-sources.py` verified source hashes for FFmpeg n9.0.2, nostrc
`ea5ed954267c2b513eaa0f41acf3166fdc7f4951`, secp256k1 v0.8.0,
NVIDIA headers, libidn2 2.3.8, 58 pinned AMF header/license Git blobs and three
libyuv notices from its actual source revision. See `sources.lock.json`.
The producer uses the existing complete Windows FFmpeg profile, not an MF-only
substitute. Explicit `mingw32/aarch64` overrides avoid the x64 MSYS-shell host
detection. Private static MIT secp256k1, consumer `SECP256K1_STATIC`, and explicit
private include/link paths avoid the VM's unowned `libsecp256k1-7.dll`.

**35 ARM64 PE files: two CLIs, one Metadesk core DLL, two FFmpeg DLLs and
30 DLLs from 27 version-locked MSYS2 packages.** `llvm-readobj` recursively
checked PE machine type/imports; SDL3 is an explicit sdl2-compat dlopen edge.
OS/API-set imports (36 unique names) were not copied. Actual private closure:

```text
avcodec-63.dll  avutil-61.dll  libbrotlicommon.dll
libbrotlidec.dll  libbz2-1.dll  libc++.dll
libcjson-1.dll  libcrypto-4-arm64.dll  libcurl-4.dll
libiconv-2.dll  libidn2-0.dll  libintl-8.dll
libjansson-4.dll  libjpeg-8.dll  libmetadesk-0.dll
libnghttp2-14.dll  libnghttp3-9.dll  libngtcp2-16.dll
libngtcp2_crypto_ossl-0.dll  libnsync.dll  libpsl-5.dll
libssh2-1.dll  libssl-4-arm64.dll  libunistring-5.dll
libuv-1.dll  libvpl-2.dll  libwebsockets.dll
libwinpthread-1.dll  libyuv.dll  libz-1.dll
libzstd.dll  metadesk-client.exe  metadesk-host.exe
sdl2.dll  sdl3.dll
```

`out/runtime-manifest.json` records unsigned PE hashes, imports, OS boundary,
package versions, package metadata and selected library licenses. The signed
MSI contains its own updated PE hashes. All actual package versions are pinned
in `runtime-licenses.json`; unknown packages/versions fail. No GPL-only DLL,
x264/x265, bundled OpenH264, or stock full-codec FFmpeg is in the payload.
Dual-licensed libraries use the LGPL/BSD alternative; GNU command-line tools
are not bundled. libidn2's missing package license directory was **not skipped**:
its pinned upstream source supplies the full license/copyright texts and an
offline source archive. libyuv LICENSE/PATENTS/AUTHORS are included.

The staged runtime codec probe passed: LGPL-2.1-or-later avcodec/avutil; exactly
H.264 decoder/parser plus h264_nvenc, h264_amf, h264_mf and h264_qsv encoders.
GPL/nonfree/version3/libx264/libx265/libopenh264 are disabled. MF source SHA256
is `1f96132f7e595b1a44d753df2423059da2bbd39406d6dd4396e89e27a611c9a1`.
The installed FFmpeg source archive contains exact patched source, headers,
recipe, patch and configure/configuration receipts. Its layout/header entries
and MF hash were independently checked; the installed source offer explains
recipe layout reconstruction, reverse-patching and relocated header paths.
An independent full rebuild from that archive was **not** performed.

This is a repeatable native recipe, **not bit-for-bit reproducibility or a fully
source-pinned Windows SDK**. Rolling MSYS2 package availability, original package
archive hashes/recipes, toolchain source provenance, source tar timestamps,
MSI identities and signatures remain release-producer work. The current closure
uses **OpenSSL 4.1.0**, not the plan's intended OpenSSL 3 producer.

## Native gates

| Gate | Result |
|---|---|
| Full native product compile, client/tools enabled | PASS, `-j2` |
| Strict ARM64/private closure and licenses | PASS, 35 PEs / 27 packages |
| Staged FFmpeg runtime inventory/license probe | PASS |
| WiX MSI compile and explicit ICE validation | PASS: 0 errors; 93 ICE91 + 1 ICE61 warnings |
| Installed unsigned 0.1.0 help + uninstall | PASS |
| 0.1.0 → lifecycle-only 0.1.1 upgrade, installed help, uninstall | PASS |
| 0.1.0 → same-version self-signed fixture replacement, help, uninstall | PASS |
| Downgrade 0.1.1 → 0.1.0 | Rejected, exit 1603 with authored newer-version message; rollback product cleanup PASS |
| Self-signed fixture installed help + uninstall | PASS; not trusted-signature acceptance |
| winget 1.29.380 `validate --manifest` | PASS, final three-file set, exit 0 |
| Portable authoring tests / Python compile / Bash and PowerShell syntax | PASS |
| Public download, winget install/submission, factory-clean OS, GPU/capture session | NOT RUN |

Per-user fixture path was `%LOCALAPPDATA%\Programs\Metadesk-W10-Test`, separate
from W1's directories. Both CLIs resolved from that installed directory and
returned exit 0 from new `cmd /d` processes with persisted machine/user PATH
reloaded and MSYS2/build paths excluded. Installed PE hashes matched the
manifest. Host help writes stderr normally; the final harness captures native
stdout/stderr directly and judges exit status, rather than PowerShell 5.1's
stderr-to-ErrorRecord behavior. No capture/input or automatic host start ran.

ICE91 warns about intentionally user-only directories; the MSI is not a
machine install. ICE61 warns about the deliberate same-three-part-version
replacement policy, which was tested. Warnings were retained, not suppressed.
ICE64 initially found the missing parent-folder removal entry; authoring was
fixed, not skipped. Custom installation-directory persistence and failed-upgrade
cleanup were corrected following review and exercised natively.

## Signing dry run — strict trust failure retained

Default build is unsigned and never opens a signing store. Explicit `-Sign`
signs all 35 PEs, updates hashes, signs the MSI and requires `signtool verify
/pa /all /v`. The final fixture completed **36 SHA256 Authenticode signing
operations**, with RFC3161 SHA256 timestamps from DigiCert. Its one-day
CurrentUser\My signer was `D4814EBD11409E8F398F8420235AC9B892C1DB8D`.

**Verification failed** because the chain terminates at an untrusted root.
`Get-AuthenticodeSignature` reported `UnknownError` with that trust-chain
message, not a successful verification. The timestamp certificate subject was
DigiCert SHA256 RSA4096 Timestamp Responder 2026 1. Neither .NET Root insertion
nor certificate-only `certutil -user -f -addstore Root` was accepted in SSH:
`0x80070032`, ERROR_NOT_SUPPORTED. No machine-store/policy or console workaround
was attempted. The exact console/profile/policy cause is not established.
The production verification guard remains fail-closed. Trusted verification
needs an owner-approved interactive check or a trusted signing identity.

All four throwaway My certificates/private keys were deleted with `-DeleteKey`;
independent My/Root inventory found zero W10 entries. Temporary public `.cer`
files were removed. The two failed Root imports created no trust entries.

## Artifacts retained locally, ignored by Git

Under `packaging/windows/out/` (never published; the self-signed MSI is explicitly
untrusted and not releasable):

| Artifact | Bytes | SHA256 |
|---|---:|---|
| `metadesk-0.1.0-arm64.msi` | 30,363,648 | `71e59e765da85e51ed2fdfae3da2ece4c18ef9437e6ed2f6465699c287e0c146` |
| `metadesk-0.1.0-arm64-selfsigned-untrusted.msi` | 30,416,896 | `0fc89b2b00a9de5c1204f3c7a464be3e2aec6e0565e37e20f3ad45a9c0c16cb1` |
| `metadesk-0.1.0-arm64-stage.zip` | 32,667,778 | `b32ca21f9cc6bba5d36b9400ee80919677f02621377a86863c176b8b50a27286` |
| `ffmpeg-corresponding-source.tar.gz` | 17,587,768 | `e48f65db809a57fe435082051932115de82596bd271accfc93271cdbf50e97bd` |

`runtime-manifest.json` and `msys2-packages.txt` accompany them. The checked-in
winget installer references the final unsigned MSI ProductCode
`{8D2FEA4F-1750-471A-B8A3-20E8D5B92DA3}` and hash, with an intentionally unhosted
`https://example.invalid/...` URL. Regenerate after final signing with a real
HTTPS release URL; schema validation is not hosted acceptance.

## Owner prerequisites and follow-up

- Trusted code-signing certificate/private-key access, or Azure Trusted Signing
  account/profile, authorized identity, client plugin and metadata/endpoint;
  approved RFC3161 service. Self-signed fixture evidence is not public trust.
- Immutable Windows dependency/source receipts, intended OpenSSL-family
  reconciliation, and exact MSYS2 patches/recipes/source for every shipped LGPL
  DLL. Upstream-only source and this offer are not a completed distro source
  supply. Publish the exact FFmpeg source beside the MSI, fill release notes,
  produce SBOM and regenerate signed hashes/winget URLs before distribution.
- Native x64 build/install closure qualification: **md-sfxh**. Only ARM64 Windows
  11 was available. No older Windows or physical GPU support claim is made.
- Per-user logon startup is not authored: it needs explicit host credentials,
  configuration and opt-in lifecycle. No session-0 service or auto-start occurs.
  FIPS mesh daemon is not bundled; it requires a separate Windows installation.

## Cleanup receipt

Before teardown, all five tested Metadesk ProductCodes returned ProductState
`-1` (unregistered); WiX's administrative image also returned `-1`. Files,
custom install directory, Start-menu directory, dedicated installer registry
root and PATH entry were absent/restored. Certificate entries and temporary
`.cer` files were zero. W1's binaries, shared MSYS2 DLLs/package database and
interactive console were not modified, locked or disconnected.

**Final teardown PASS:** after local artifact SHA256/size and winget
hash/ProductCode verification, `C:\Users\bizarro\md-w10` was recursively removed.
A subsequent check confirmed the tree and test installation/installer key
absent, zero W10 certificates, and no task executable still running. No tool
installation, task directory or test certificate was left on the VM.
