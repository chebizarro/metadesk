# metadesk <VERSION> — <DATE>

> Release editor: replace every `<PLACEHOLDER>`; do not publish this template
> with missing source links or an unclassified dependency. Produce one codec
> statement per artifact; do not describe a distro's system FFmpeg as our build.

## Changes

- <USER_VISIBLE_CHANGES>
- <KNOWN_LIMITATIONS>

## Downloads and provenance

- <ARTIFACT_OS_ARCH_URL_AND_SHA256>
- metadesk revision: <COMMIT>; nostrc revision: <COMMIT>
- Exact dependency inventory, licenses and source hashes: <SBOM_AND_MANIFEST_URL>
- Build/configure logs, toolchain and FFmpeg patches: <REPRODUCIBILITY_URL>

## License and codecs

metadesk is MIT licensed, Copyright (c) 2026 Chris Daley (project started
2026-04-13). [Project license](../LICENSE); [third-party notices](../THIRD_PARTY_NOTICES.md).
Dependencies retain their own licenses. Include these files and the upstream
license/NOTICE texts in every artifact's licensing information.

**Bundled DMG, MSI, container and Flatpak builds:** minimal LGPL-only FFmpeg,
H.264 decoder/parser and hardware/OS encoders (VideoToolbox on macOS;
NVENC/AMF/Media Foundation/QSV on Windows; NVENC/AMF/VAAPI/QSV on Linux,
as supported by the machine). No GPL codecs, libx264 or libx265 in this codec
payload; no FFmpeg libopenh264 dependency. This does not claim that a complete
Linux base image/runtime contains no GPL-licensed system programs.

**Optional software fallback:** metadesk loads Cisco's prebuilt OpenH264
binary at runtime, downloaded **separately from Cisco to the user's device**;
OpenH264 is never bundled in our artifacts. Its source code is BSD licensed;
Cisco's conditional binary/patent terms are separate from the source license.
Users can enable/disable/re-enable it at <CODEC_CONTROL_LOCATION>, which must
display “OpenH264 Video Codec provided by Cisco Systems, Inc.” Reproduce the
complete [Cisco binary license](https://www.openh264.org/BINARY_LICENSE.txt)
in <INSTALLED_LICENSE_LOCATION>. Runtime download version, authenticated
source and integrity verification: <CISCO_RUNTIME_DETAILS>. If integration
is not available in this release, say so rather than claim a working fallback.

**Native .deb/.rpm/AUR packages:** link system libavcodec/libavutil. The distro's
configuration and licensing apply, including libx264 only if supplied by that
distro. libx264 is otherwise only an optional source-build feature, not a
bundled-release fallback.

H.264 patents may impose obligations depending on jurisdiction and use.
Hardware encoding and open-source copyright licenses do not by themselves
establish patent clearance. Cisco's terms have conditions and limits. This
release note is not legal advice.

## LGPL corresponding source and replacement

This release uses FFmpeg <VERSION/COMMIT> under LGPL-2.1-or-later.
Download its **exact corresponding source**, including all patches and build
instructions, at <SAME_RELEASE_FFMPEG_SOURCE_ARCHIVE_URL>. Archive SHA-256:
<HASH>. Full configure arguments per artifact: <CONFIGURE_ARGS_URL>.
The source is hosted alongside our binary downloads, not just linked to the
upstream homepage. Include `COPYING.LGPLv2.1` and attribution in the payload.

FFmpeg is dynamically linked. Users may replace the shared libraries with
compatible modified versions; replacement and local macOS re-signing
instructions: <REPLACEMENT_INSTRUCTIONS_URL>. No restriction on reverse
engineering for debugging these modifications is imposed. If any LGPL
component is linked statically, provide relinkable application objects and
instructions at <RELINK_OBJECTS_URL_OR_NOT_APPLICABLE>.

Other redistributed LGPL components (including private GLib/libintl or image
base libraries): <COMPONENT_SOURCE_LINKS_AND_LICENSE_TEXT_LOCATIONS>.
Native distro packages use the distro's corresponding-source mechanisms:
<DISTRO_COPYRIGHT_AND_SOURCE_INSTRUCTIONS>.

Release check: the downloaded source and recipes must reproduce the shipped
library configuration; attach license and codec enumeration evidence for each
bundled artifact. macOS-only evidence does not qualify Windows/Linux builds.
