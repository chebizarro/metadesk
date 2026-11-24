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
- Package and library inventories, source pins and build evidence: <SBOM_AND_MANIFEST_URL>
- Build/configure logs, toolchain and FFmpeg patches: <REPRODUCIBILITY_URL>

## License and codecs

metadesk is MIT licensed, Copyright (c) 2026 Chris Daley (project started
2026-04-13). [Project license](../LICENSE); [third-party notices](../THIRD_PARTY_NOTICES.md).
Dependencies retain their own licenses. Include these files and the upstream
license/NOTICE texts in every artifact's licensing information.

**Bundled AppImage, DMG and Flatpak builds:** see the codec policy for each
artifact above. These use minimal LGPL FFmpeg, not a distro codec build.
No GPL codecs, libx264, libx265 or libopenh264 are included in that codec
payload. The codec inventory does not measure hardware encoding availability.

**Optional software fallback:** metadesk loads Cisco's prebuilt OpenH264
binary at runtime, downloaded **separately from Cisco to the user's device**;
OpenH264 is never bundled in our artifacts. Its source code is BSD licensed;
Cisco's conditional binary/patent terms are separate from the source license.
These release smokes do not qualify Cisco runtime acquisition or fallback.
See the [Cisco binary license](https://www.openh264.org/BINARY_LICENSE.txt).

**Native .deb/.rpm/AUR packages:** link system libavcodec/libavutil. The distro's
configuration and licensing apply, including libx264 only if supplied by that
distro. libx264 is otherwise only an optional source-build feature, not a
bundled-release fallback.

H.264 patents may impose obligations depending on jurisdiction and use.
Hardware encoding and open-source copyright licenses do not by themselves
establish patent clearance. Cisco's terms have conditions and limits. This
release note is not legal advice.

## Copyleft corresponding source and replacement

The **exact corresponding source** for each bundled FFmpeg build is attached
alongside its binary download. Each archive includes the upstream source
archive, any applied patches, the build recipe, full configure arguments,
license notices and codec inventory.

<FFMPEG_SOURCE_TABLE>

FFmpeg and the AppImage Ubuntu closure libraries are dynamically linked.
Users may replace the shared libraries with
compatible modified versions; replacement and local macOS re-signing
instructions (including AppImage extraction and Flatpak local rebuild):
<REPLACEMENT_INSTRUCTIONS_URL>. No restriction on reverse
engineering for debugging these modifications is imposed. If any LGPL
component is linked statically, provide relinkable application objects and
instructions at <RELINK_OBJECTS_URL_OR_NOT_APPLICABLE>.

Other redistributed copyleft components (including the AppImage runtime’s
static libfuse and any Ubuntu closure libraries): <COMPONENT_SOURCE_LINKS_AND_LICENSE_TEXT_LOCATIONS>.
Native distro packages use the distro's corresponding-source mechanisms:
<DISTRO_COPYRIGHT_AND_SOURCE_INSTRUCTIONS>.

The codec inventory in each source attachment is for that specific build.
