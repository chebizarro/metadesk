# W14 AppImage qualification — 2026-11-22

Source baseline `e0abbc06555f174acc5c2fe28622cf7cff14832d`, branch
`md/w14-appimage-flatpak`; nostrc `cfcb86296068c1e4090bc5ce8a486392dc74eb11`.
No application-source, canonical FFmpeg-policy, sibling or main-checkout edits.
No push, merge, public publication, production signing or Beads closure.

## Artifact and real runtime evidence

Ignored artifact: `output/Metadesk-0.1.0-aarch64.AppImage` (about 20 MiB).
SHA256: `b6f0e131f2d3451df56e9a46e2e3bc24b47a9bd6db948c1b8583032de2fa41af`.
`output/bundle.json` records **102 private DSOs**, original source hashes and
post-RPATH-removal packaged hashes. glibc/ELF loader are not bundled.
No OpenH264/x264/x265/libvpl is present. Only avcodec/avutil from FFmpeg.
No host frontend or compiler is installed into the image.

- Native producer: Ubuntu 24.04 aarch64, GCC 13.3, glibc 2.39, `-j2`.
  Client build and real `AppRun --help` **PASS**. Actual AppImage
  `APPIMAGE_EXTRACT_AND_RUN=1 ... --help` **PASS**, no FUSE requirement.
- Official FFmpeg 8.1.2 source SHA256:
  `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`.
  Consumed canonical `container` profile with NV/AMF headers, VAAPI/DRM;
  **LGPL-2.1-or-later**, one H.264 decoder/parser and exactly
  **h264_nvenc, h264_amf, h264_vaapi**, no QSV on ARM64. Runtime enumeration
  **PASS**. A separately compiled probe linked to full system FFmpeg was
  rejected (exit 1), rather than accepted as a minimal profile.
- Clean **Fedora 44 aarch64** container, glibc 2.43, image config
  `ff918341dfc920e8694e2079a68792627b0d2b3cf883fd3bce9f409a8a1289cc`:
  final image extraction, client help, exact runtime codec inventory, and the
  packaged `md_decoder_*` H.264 → libyuv → **one 16x16 RGBA frame** all **PASS**.
  Container was network-disabled, read-only, no capabilities, no-new-privileges,
  artifact-only read-only mount and private tmpfs. No package installation,
  desktop/bus/device mounts, uinput, AT-SPI/session access or GPU claim.
- Native FUSE execution, Arch, x86_64, real remote connection, GNOME/Hyprland
  rendering, driver/dlopen paths and physical hardware encoders: **NOT RUN**.
  Acceptance is not claimed for the untested distro/desktop/architecture matrix.

## Signing, checks and fixes

Versioned AppImageTool 1.9.1 and type2 runtime 20251108 fetched privately;
URL/hashes in `tools.lock.json`. The earlier fetched `continuous` runtime was
not used. GPG detached signing/verification with a disposable Ed25519 identity
and zsync metadata generation **PASS**. URL is `example.invalid/DRY-RUN`;
no upload or production trust. Private GPG home/key removed; public demo
signature/key/zsync retained ignored under `output/dry-sign/`.

Six policy/wiring tests **PASS** on macOS and Ubuntu. All new shell scripts
pass `sh -n` and ShellCheck. Both AppStream documents validate successfully;
see the Flatpak validation for exact lint/desktop boundaries. Oracle reviewed
supplied packaging excerpts only, after snapshot routing failed: no actionable
finding, not a full-tree review or release approval.

A real staging failure exposed Meson's `libmetadesk.so.*.p` object directory
matching a shared-library glob; selective staging now excludes directories.
The first Fedora harness was invoked before its image pull finished and failed
without launching the product. The corrected harness sequences a completed pull
and uses `--pull=never`; final actual-artifact tests above passed. No product
assertions, timeouts, sleeps or retries-to-green were introduced.

## Owner gates and cleanup

**Not distribution-cleared.** Complete/review the transitive license/source and
LGPL replacement-rights corpus, concrete release source URLs, production
signing/update policy and remaining distro/desktop/architecture matrices.
The exact FFmpeg source/configure receipt, local source snapshot and public
signing demo are retained ignored under `output/`; project notices and FFmpeg
LGPL text alone are not a complete transitive source offer.

Shared W14 VM/disk cleanup receipt is in `../flatpak/VALIDATION.md`. Raw logs
remain private during qualification and are removed, never committed.
