# Native arm64 macOS 13.0 dependency profile

Requires Python >= 3.12, Git, Xcode/SDK, CMake, Meson, Ninja, pkg-config, make and Perl.

```sh
MD_DMG=1 /path/to/metadesk/packaging/macos/build-release.sh \
  /path/to/nostrc-git-repository /private/tmp/md-release-new
```

The release DMG contains two sibling bundles: `Metadesk.app` (the existing
host/client bundle, with its host agent launcher) and `Metadesk Manager.app`
(the native AppKit manager, its companion `metadesk-client`, and their private
library closure). Copy the Manager alone to use it as a client frontend; it
requires its executable client in the bundle and never searches `PATH`. Local hosting still uses
`metadesk-host` from `Metadesk.app` or an existing CLI installation.

Both apps are ad-hoc signed by default; the DMG is unsigned. Expect a separate
[first-launch approval](https://support.apple.com/en-us/102445) for each downloaded
app. With ad-hoc signing, expect Keychain access to be tied to the exact Manager
build that created an item: an update may prompt or refuse access, and you may
need to re-import pairings. Download approval and Keychain upgrade behavior have
not been tested. Signed on-screen qualification remains open (`md-8xlg`, `md-wdi3`).

In `deps.lock.json`, `type: git` pins the repository URL and full `commit`
and `tree` object IDs. The builder shallow-fetches the exact commit and checks
both IDs before exporting a clean source tree with local `git archive`; the
archive bytes are not part of the lock. For nostrc it reads the objects from
the supplied repository without modifying it or using its working tree. The
current nostrc pin is `6f7a8d025768d497bef700fe55eea1c687dac328`. Submodules
are not fetched. GLib-free builds keep explicit build-tree discovery: upstream's
full install at this revision also builds NIP-55L, whose macOS Keychain code
needs GLib headers.

`type: archive` is reserved for published release assets, with an `archive`
cache filename and `sha256`. `build-deps.py OUTPUT --downloads CACHE
--nostrc-repo REPO` can reuse these archives but always rechecks their bytes
before extraction. Do not use Gitiles or GitHub-generated source archives as
release assets.

For iterative packaging against an already-built private dependency tree:

```sh
export MD_DEPS_PREFIX=/private/tmp/md-release-new/deps/prefix MD_MACOS_MIN=13.0
/path/to/metadesk/packaging/macos/build-app.sh \
  /private/tmp/md-release-new/deps/sources/nostrc \
  /private/tmp/md-release-new/deps/build/nostrc /private/tmp/md-app-new -Dmacos_manager=true
```

No target libraries are installed system-wide. Tools may be Homebrew
executables, but target pkg-config discovery is isolated, CMake ignores host
package prefixes, and Meson receives `macos_deps_prefix`. Apple SDK libcurl,
uuid, zlib and frameworks remain OS-owned. The profile uses real SDL2, libyuv
without JPEG, OpenSSL 3 and GLib-free nostrc with NIP-46/NIP-5F. NIP-55L is off.
OpenH264 is neither downloaded nor bundled. FFmpeg uses the existing shared
`packaging/ffmpeg/build-minimal-lgpl.sh macos` interface unchanged.

Every build sets `MACOSX_DEPLOYMENT_TARGET=13.0`; CMake also receives
`CMAKE_OSX_DEPLOYMENT_TARGET=13.0` and `CMAKE_OSX_ARCHITECTURES=arm64`. Pins,
compiler/SDK, command logs, configuration and upstream license texts are
retained in the private output. Sources/builds/archives are retained for the
publisher to assemble corresponding-source releases. This is source-pinned,
not a bit-identical-output, legal-compliance or notarization claim.

## Gates

* `check-install.sh PREFIX`: staged host/client help and manager client-path
  lookup (when built), with an empty environment and no dyld overrides.
* `macho.py FLOOR TREE...`: every Mach-O slice, including legacy minimum load
  commands; missing/too-new minimums and an empty image set fail.
* `MD_MACOS_MIN=13.0 verify-app.sh APP`: actual image minimums (not just plist),
  dependency/rpath closure, signatures and clean-environment startup for each
  app. For the Manager this checks the exact bundled client path, client help,
  and `--smoke-quit`: the packaged executable opens its real AppKit window,
  runs the event loop, and exits through normal quit. This is an offline
  check with an isolated home; it requires a window server and does not
  exercise a connection, signers, Keychain, host IPC, or user data.
* `build-release.sh`: full configured Meson suite, including the isolated
  Keychain fixture. Optional live/permission tests retain their explicit skips.
* The release adapter re-runs the app gates on the read-only mounted DMG,
  probes both FFmpeg copies, and records each bundle's measured libraries and
  hashes. Each inventory must pass the same reviewed source-offer rules.
* Exercise Launch Services/Finder separately on a logged-in Mac:

  ```sh
  /usr/bin/open -n -W -a /private/tmp/md-app-new/Metadesk.app \
    --stdout /private/tmp/md-launch.stdout --stderr /private/tmp/md-launch.stderr
  ```

  The current CLI-style client prints usage and exits without connection
  arguments. Require actual usage output (or a running client), not just
  `open` success. This is not a GUI connection/codec qualification.

  To exercise the Manager through Launch Services, use the same offline smoke:

  ```sh
  (
    home=$(mktemp -d) || exit 1
    trap 'rm -r "$home"' EXIT
    /usr/bin/open -n -W -a '/private/tmp/md-app-new/Metadesk Manager.app' \
      --env "HOME=$home" --env "CFFIXED_USER_HOME=$home" \
      --env PATH=/usr/bin:/bin:/usr/sbin:/sbin \
      --stdout "$home/smoke.stdout" --stderr "$home/smoke.stderr" --args --smoke-quit &&
      grep -Fx 'PASS: Manager window opened and normal quit completed' "$home/smoke.stdout"
  )
  ```

  Require the app's completion marker, not just `open` success.

arm64 is the first profile. x86_64/universal2 need their own source build and
qualification, not `lipo` of Homebrew bottles. Developer ID/notarization and the
host helper/service remain separate release work. Before publishing, supply
corresponding-source archives/URLs alongside binaries and preserve LGPL
replacement/re-signing rights per `THIRD_PARTY_NOTICES.md`.

## W5 qualification — 2026-11-22

On arm64 macOS 27.0.1 (26A434), base metadesk `9791779`, Xcode/SDK 27.0:

* A fresh `build-deps.py` source build exited 0. The then-current lock contained 12 pins; nostrc was archived at `cfcb86296068c1e4090bc5ce8a486392dc74eb11`.
  The runtime FFmpeg inventory probe exited 0: LGPL-2.1-or-later, exactly H.264
  decoder/parser and `h264_videotoolbox` encoder.
* `build-app.sh` with only `MD_DEPS_PREFIX` (no caller-supplied minimum) exited
  0, copied the lock/legal texts, and ran `verify-app.sh` with the fixed 13.0
  gate. The ad-hoc signed bundle contains **13 private libraries / 15 Mach-O
  images**, every one at **minos 13.0**, independently confirmed with
  `xcrun vtool -show-build`. No Homebrew or `/usr/local` load paths survived.
  Images: host, client, libmetadesk, nostr-json, SDL2, avcodec, avutil, cJSON,
  jansson, crypto, ssl, websockets, secp256k1, nsync and yuv.
* Staged host/client help exited 0 and loaded the **staged** libmetadesk with
  no dyld overrides. Repeating installation with `--only-changed` also passed.
  The original absolute `/opt/metadesk/lib/libmetadesk.0.dylib` install-name
  failure was reproduced before the Darwin install hook was added.
* Full configured macOS suite: **51 OK / 0 failures / 1 skip**, Meson exit 0.
  Skip: opt-in `AX owned clicks and native video`, exit 77. Both clock guards,
  NIP-46 and NIP-5F integration tests passed. No full live-desktop qualification
  or execution on macOS 13 itself is claimed.
* Launch Services/Finder: `open -n -W -a Metadesk.app --stdout ... --stderr ...`
  returned 0. The actual app printed the missing-identity diagnostic and its
  usage with the bundle executable path, then exited. `open` also printed
  `Unable to block on application (GetProcessPID() returned ...)`; startup is
  established by the captured app output, not by `open`'s return code alone.
* Four parser tests, shellcheck, shell syntax and `git diff --check` passed.
  A real two-slice fixture (arm64 minos13 / x86_64 minos14) was rejected at
  floor13; this is verifier coverage, **not** universal2 product qualification.
  A floor12 check against the real bundle also failed as expected.
* Scoped Oracle review of supplied consequential code found one enforcement
  gap: the direct private-prefix build entry point needed to pin final
  verification to 13.0 independently of the computed plist. Fixed before the
  final app build; no whole-file or independent-runtime Oracle approval claimed.

Initial recipe diagnosis also caught libwebsockets 5.1 default HTTP/3 selecting
GnuTLS and CMake's prefix propagation discovering host metadata. The profile
now explicitly disables HTTP/3/QUIC/GnuTLS and CMake-to-pkg-config prefix
injection. No files under `packaging/ffmpeg/` were changed. Private outputs and
archives were deleted after verification. x86_64/universal2 remains `md-c17b`.

## Host session agent (W17)

The bundle includes `Contents/Library/LaunchAgents/dev.metadesk.host.plist`
and a sealed `Contents/Resources/metadesk-host-agent` launcher. Registration
is opt-in; SMAppService approval, stable signed helper/TCC identity and
notarization remain md-wdi3. See [host lifecycle](../../docs/host-lifecycle.md).
