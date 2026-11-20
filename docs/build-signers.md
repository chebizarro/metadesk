# Building the optional signer backends

All backends default to off. Enable any combination with
`-Dsigner_nip46=true`, `-Dsigner_nip55l=true`, and `-Dsigner_nip5f=true`.
No local pkg-config descriptors are needed for signer discovery.

```sh
meson setup build-signers \
  -Dnostrc_root=/path/to/nostrc -Dnostrc_build_dir=/path/to/nostrc/_build \
  -Dsigner_nip46=true -Dsigner_nip55l=true -Dsigner_nip5f=true
meson compile -C build-signers -j2
NOSTRC_TEST_ISOLATION=1 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  meson test -C build-signers --print-errorlogs
```

The tests use deterministic keys and loopback signer fixtures, not a live
signer, desktop session, or user keyring. `NOSTRC_TEST_ISOLATION=1` prevents
nostrc's key-store fallback from consulting the user's store.

## Dependency discovery

For signer adapters, Meson tries installed libraries first (including the Homebrew
prefix on macOS), then libraries under `nostrc_build_dir/nips/<component>`. Adapters
are regular CMake library targets, so the fallback links their archive. A link
check rejects installed backend archives missing the entry
points Metadesk uses and tries their build-tree counterpart. Source include paths,
generated export-header paths, and nested installed adapter include paths are
added only for enabled backends. Like the core nostrc headers, they are target
include directories, so they win over installed headers (see
[Core nostrc library discovery](#core-nostrc-library-discovery-md-r4ia)).

| Option | nostrc targets beyond the common core | System dependencies |
| --- | --- | --- |
| `signer_nip46` | `nostr_nip46_core`, `nip04` | Jansson |
| `signer_nip55l` | `nostr_nip55l_core`, `nip04`, `nip06`, `nostr_nip49_glib`, `nostr_nip49_core` (also common `nip19`/NIP-44) | GIO >= 2.76, libsodium; Security + CoreFoundation on macOS; libsecret-1 + `gnostr-secret` on Linux |
| `signer_nip5f` | `nostr_nip5f_core` (also common `nip19`/NIP-44) | common core dependencies |

For Linux, `gnostr-secret` can also come from
`nostrc_build_dir/gnome/seahorse`. Missing archives produce a configure error
naming the nostrc target to build. Build missing targets with
`cmake --build /path/to/nostrc/_build --target <target> --parallel 2`.

### Common JSON library

JSON follows the core libnostr build-tree preference, not the signer adapter
installed-first order. Meson selects, in order:

1. `nostrc_build_dir/libjson/libnostr-json.{dylib,so,a}`, when present. The typed
   library dependency includes Jansson and lets Meson manage the build rpath, so
   the current shared library loads without `DYLD_LIBRARY_PATH` or
   `LD_LIBRARY_PATH`. Meson removes this build-tree rpath during installation;
   no raw rpath linker flags or install rpath are added.
2. Installed `libnostr-json` pkg-config metadata (the current nostrc module name),
   then the `nostr-json` module alias, then the `nostr-json` library directly.
3. On Linux, the old `nostr_json` library with a deprecation warning.

macOS deliberately rejects legacy-only discovery with an actionable configure
error. The old `/opt/homebrew/lib/libnostr_json.dylib` can retain dependencies on
removed Homebrew SONAMEs; rebuilding the canonical library does not replace it.
Build only the missing target if necessary:

```sh
cmake --build /path/to/nostrc/_build --target nostr_json --parallel 2
```

No compatibility symlinks, loader overrides, generated-manifest edits, or local
pkg-config shims are needed. Removing the stale underscore-named Homebrew library
is recommended after checking its other consumers; Metadesk does not remove or
modify it.

## Core nostrc library discovery (md-r4ia)

Metadesk always links these nostrc components besides the JSON library above:

| Component | CMake target | Kind on nostrc master |
| --- | --- | --- |
| libnostr | `nostr` | library (static by default) |
| libgo | `nostrgo` (older trees: `libgo`), `go_fiber` | library, always-static `go_fiber` |
| NIP-44 | `nostr_nip44_core` | static library |
| NIP-51 | `nostr_nip51_core` | **OBJECT library**: no archive |
| NIP-98 | `nostr_nip98` | library |
| NIP-19 | `nip19` | library |
| NIP-17 | `nostr_nip17_core` | **OBJECT library**: no archive |
| NIP-11 | `nip11` | library |

libnostr and libgo prefer `nostrc_build_dir` (they must match the source
headers). libnostr falls back to an installed copy. Each NIP component is
discovered in this order:

1. An installed library (the Homebrew prefix on macOS, the linker's default
   path elsewhere).
2. The library CMake built for the target, by file name under
   `nostrc_build_dir/nips/<component>`. `lib<target>.a` is linked by path;
   `.so`/`.dylib` go through `cc.find_library()`, so Meson manages the build
   rpath. A file-name probe never silently substitutes a system copy.
3. For CMake `OBJECT` libraries, every object that the CMake generator lists
   for the target. Unix Makefiles lists them as `<target>: <object>` rules in
   `nips/<component>/CMakeFiles/<target>.dir/build.make`. Ninja lists them as
   `build nips/<component>/CMakeFiles/<target>.dir/...o:` edges in the
   top-level `build.ninja`.

Why step 3 reads the generator's manifest:

- **A fixed object path broke.** The earlier fallback linked
  `CMakeFiles/nostr_nip51_core.dir/src/nip51.c.o` only. nostrc 5b3c987c8 split
  that target into `nip51.c`, `nip51-document.c` and `nip51-schema.c`, so
  `nostr_nip51_document_*`/`nostr_nip51_tag_*` were undefined.
- **A glob of `CMakeFiles/` is unsafe.** It would also link objects of renamed
  or deleted sources, which an upgraded build tree keeps, and produce
  duplicate or stale symbols.
- **The manifest is exact.** It is the list CMake itself hands to
  `$<TARGET_OBJECTS>` consumers, and it is rewritten whenever CMake regenerates
  the target.
- **It stays fresh.** `fs.read()` makes Meson reconfigure when the manifest
  changes.
- **Failures are explicit.** An object that is listed but not built is a
  configure error naming the `cmake --build ... --target` to run. Other
  generators (Xcode, Visual Studio) are not parsed and produce a "not found"
  error.

Configure prints one line per component naming what it chose, for example:

```text
Message: nip51: using build-tree objects of CMake target nostr_nip51_core: nip51.c.o, nip51-document.c.o, nip51-schema.c.o
Message: nip98: using installed library
Message: libgo: using build-tree libraries libnostrgo.a + libgo_fiber.a from /path/to/nostrc/_build/libgo
```

**Headers follow the selected nostrc.** The source include dirs of every
component (`libnostr`, `libgo`, `nips/nip{11,17,19,44,51,98}`), their generated
build-tree include dirs, and the enabled signer include dirs are target
`include_directories`. Meson emits those before any dependency's flags. Before
md-r4ia they were only dependency include dirs, and `nips/nip44`/`nips/nip51`
were missing. The libyuv fallback adds the bare Homebrew prefix
(`-I/opt/homebrew/include`) ahead of them, so on macOS `<nostr/nip51/nip51.h>`,
`nip17`, `nip44`, `nip98` and the signer headers resolved to an older
*installed* nostrc while the objects came from the build tree. Check with
`ninja -C <build> -t deps | grep 'nostr/nip'`: no `/opt/homebrew/include/nostr`
path should appear when `nostrc_root` is set.

On macOS, Homebrew still carries installed `nip11`, `nostr_nip98`, `nip19` and
`nostr_nip44_core` archives. Installed-first discovery keeps selecting them over
the build tree, as before md-r4ia.

### Validation (md-r4ia, 2026-11-20)

`meson test` counts below are passed/total. "All signers" means
`-Dsigner_nip46=true -Dsigner_nip55l=true -Dsigner_nip5f=true`. The VM runs
used `-j2`, and each suite ran in parallel and then serially
(`--num-processes 1`) with the isolation variables above. The shared trees were
used read-only; master was a private `git worktree`/`git archive` copy built
out of tree.

| Host | nostrc build tree (commit) | nip51 linked as | Default | All signers |
| --- | --- | --- | --- | --- |
| macOS | shared `_build` (checkout 120ca9672, Makefiles) | 1 object | 37/37 | 42/42 |
| macOS | private master 5b3c987c8, Makefiles | 3 objects | 36/37 | 39/42 |
| macOS | private master 5b3c987c8, Ninja | 3 objects | — | 39/42 |
| Linux VM | shared `~/nostrc/_build` (da2e4d62b, Makefiles) | 1 object | 36/36 both | 41/41 both |
| Linux VM | private master 5b3c987c8, Makefiles | 3 objects | 35/36 both | 38/41 both |

Every master failure is the same allowlist-grant regression: `capability
grants`, plus `access_cli` and `mcp-proxy NIP-98 e2e` with all signers. It is
not a build failure. nostrc 5d1283a7c made `nostr_nip51_parse_list()` keep
application tags such as `metadesk-caps`, so `src/core/nostr.c` counts each
grant twice and denies it (md-jqnz). A throwaway copy with that double-append
removed passed 42/42 against macOS master.

md-jqnz fixed this. The allowlist is now decoded from raw event tags and no
longer calls `nostr_nip51_parse_list()` (rules in
[access.md](access.md#public-allowlist-grant-format-v1-md-ikr)). The
`nostr ACL and AUTH private relays` test prints which nip51 tag policy
(`legacy` or `lossless`) the build links.

The md-0732 TSan wiring still configures correctly.

- **Uninstrumented tree:** `-Db_sanitize=thread` against the uninstrumented
  shared macOS tree emits the warning about uninstrumented nostrc.
- **Instrumented VM tree:** against a private TSan build of nostrc master
  (5b3c987c8, recipe in `tests/tsan-nostrc.supp`) plus TSan nsync, both nostr
  tests get `halt_on_error=1:suppressions=tests/tsan-nostrc.supp`.
- **Repeat runs:** the two nostr tests passed 20/20 with the suppressions and
  20/20 without them, with no ThreadSanitizer reports. This is evidence for
  md-d9tj. The suppressions stay while the shared trees predate 7d8b55724.

## libwebsockets upgrades and md-k1k (historical diagnosis)

The following records the earlier md-dpl investigation, before the shared nostrc
build was rebuilt on 2026-11-17. The current md-66n qualification below uses that
fresh shared build, not the private diagnostic build.

The macOS shared nostrc tree was configured with libwebsockets **4.5.8**:
`LWS_VERSION:INTERNAL=4.5.8-unknown` in its CMake cache and the 4.5.8 include
path in `libnostr/CMakeFiles/nostr.dir/flags.make`. Homebrew now provides **5.1.0**.
The recorded cache and compile flags point to stale build-tree `libnostr.a`
objects as the leading explanation, not a proven archive provenance. An old
`lws_context_creation_info` layout passed to the new library is consistent with
the intermittent failure: unrelated old-layout bytes can be interpreted as an
`io_ops` pointer (or plugin fields), producing the ABI-0 message or a crash.
nostrc zero-initializes this struct and does not supply custom `io_ops`; adding
`lws_io_ops_init()` would not fix a stale-object layout mismatch. The private
clean rebuild and repeat results below support this diagnosis.

Upstream documents the new context `io_ops` field and the version stamp for
custom IO tables in the [sans-IO split documentation](https://libwebsockets.org/lws-api-doc-main/html/md_READMEs_2README_8sans-io-split.html)
and [context creation code](https://github.com/warmcat/libwebsockets/blob/main/lib/core/context.c).

Meson warns when nostrc's cached LWS version differs from current pkg-config
metadata (ignoring suffix-only differences such as `-unknown`). When that cache
is present, native builds with OpenSSL metadata also compare the selected headers
with `lws_get_library_version()`. Neither check can prove which
headers an old object was compiled against: a changed cache without rebuilding
objects can hide a stale archive. Rebuild the dependency, not just Metadesk.
Do not create compatibility symlinks between different libwebsockets SONAMEs.

### Private rebuild used for diagnosis

No shared macOS nostrc build or nostrc sources were modified. From a clean private
directory, the dependency configure command was:

```sh
cmake -S /Users/bizarro/Documents/Projects/nostrc \
  -B /tmp/md-dpl-nostrc-lws5 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_RELAYD=OFF \
  -DENABLE_NIP04=ON -DENABLE_NIP06=ON -DENABLE_NIP11=ON \
  -DENABLE_NIP17=ON -DENABLE_NIP19=ON -DENABLE_NIP44=ON \
  -DENABLE_NIP46=ON -DENABLE_NIP49=ON -DENABLE_NIP51=ON \
  -DENABLE_NIP55L=ON -DENABLE_NIP5F=ON -DENABLE_NIP98=ON
cmake --build /tmp/md-dpl-nostrc-lws5 --parallel 2 --target \
  nostr nostr_json go_fiber nostr_nip46_core nostr_nip55l_core \
  nostr_nip5f_core nostr_nip49_glib nip04 nip06 nip19 \
  nostr_nip51_core nostr_nip98 nip11 nostr_nip17_core
```

The actual initial configure additionally passed `-DBUILD_GNOSTR_APP=OFF` and
`-DENABLE_APPS=OFF`; current nostrc reported both as unused. They are omitted above.

### Exact shared-tree rebuild command (after concurrent consumers finish)

This preserves the shared tree's other configured features, clears stale LWS
pkg-config cache entries, and rebuilds the relevant libraries. It was exercised
against the private build directory, **not** the shared one.

```sh
cmake -S /Users/bizarro/Documents/Projects/nostrc \
  -B /Users/bizarro/Documents/Projects/nostrc/_build -U '*LWS*'
cmake --build /Users/bizarro/Documents/Projects/nostrc/_build --parallel 2 --target \
  nostr nostr_json go_fiber nostr_nip46_core nostr_nip55l_core \
  nostr_nip5f_core nostr_nip49_glib nip04 nip06 nip19 \
  nostr_nip51_core nostr_nip98 nip11 nostr_nip17_core
```

**Second blocker found during md-dpl (now fixed by md-66n):** the old common
Metadesk JSON-library discovery selected `/opt/homebrew/lib/libnostr_json.dylib`. That legacy
installed library needs the now-absent `libwebsockets.21.dylib`, so an unmodified
macOS test run aborts in dyld even after rebuilding private `libnostr.a`. Current
nostrc builds `libjson/libnostr-json.2.dylib` with canonical `libnostr-json.pc`
metadata instead. Rebuilding the shared tree alone will not replace the old
underscore-named install; core JSON discovery must select the new library.
This is outside the signer-only build-edit scope. Extra `c_link_args` do not
solve it: the stale library remains a load dependency.

## Current md-66n / md-dpl qualification (2026-11-17)

From Metadesk master `323292d` plus the md-66n discovery fix, all configurations
configure, build, and pass normal `meson test --print-errorlogs` without loader
overrides, pkg-config shims, ABI aliases, or generated-manifest changes:

| Enabled options | macOS | Linux aarch64 VM |
| --- | --- | --- |
| none (defaults) | 25/25 | 26/26 |
| NIP-46 only | 26/26 | 27/27 |
| NIP-55L only | 26/26 | 27/27 |
| NIP-5F only | 26/26 | 27/27 |
| all three | 28/28 | 29/29 |

There were zero failures or skips. The all-enabled `nostr NIP-44` test also
passed 100/100 serial repetitions on each platform (`--no-rebuild --repeat=100
--num-processes=1`). Counts differ from the earlier md-dpl snapshot
because master has additional tests; Linux also has the private AT-SPI bus-loss
test. All builds used `-j2`, default client settings, and explicit
`nostrc_root` / `nostrc_build_dir`. Tests used only the isolation variables shown
at the top of this document; neither platform had loader or pkg-config overrides.

macOS used `/Users/bizarro/Documents/Projects/nostrc/_build`, freshly rebuilt
against libwebsockets 5.1.0 and OpenSSL 4 before this task. Linux used
`bizarro@192.168.64.3:~/wt-66n` and `/home/bizarro/nostrc/_build`. No shared nostrc
targets needed building, and no shared dependency tree or Homebrew files were
modified. No packages were installed.

`otool -L` on the NIP-46 library shows `@rpath/libnostr-json.2.dylib`, and its
`LC_RPATH` includes the selected nostrc build's `libjson` directory. That JSON
dylib depends on `libwebsockets.23.dylib`, not the removed `.21` SONAME. The
macOS default still has no dynamic JSON dependency. A separate negative setup
using a private symlink view of the build tree with `libjson` omitted fails with
the intended legacy-library diagnostic; the original shared tree stays untouched.
Installed canonical pkg-config/direct-library fallbacks were not runtime-tested
because neither platform needed them for this matrix.

### Install rpath regression gate

The build-tree JSON dependency uses `cc.find_library()`, not raw library/rpath
`link_args`. Meson therefore tracks the JSON directory as a removable build
rpath, including when the dependency propagates to `metadesk-host` and the client.
The default empty install rpath is preserved. This keeps the installed producer
ELFs compatible with the [Tier A bundle contract](../deploy/containers/reference/bundle-contract.md);
it does not replace the producer's dependency-closure or in-image checks.

On Linux, stage using the bundle prefix/libdir and inspect the installed files
**before** any producer `patchelf --remove-rpath` step:

```sh
meson setup build --prefix=/opt/metadesk --libdir=lib \
  -Dnostrc_root=/path/to/nostrc -Dnostrc_build_dir=/path/to/nostrc/_build
meson compile -C build -j2
meson install -C build --destdir "$PWD/build/stage"
readelf -d build/stage/opt/metadesk/bin/metadesk-host
readelf -d build/stage/opt/metadesk/bin/metadesk-client
readelf -d build/stage/opt/metadesk/lib/libmetadesk.so.0.1.0
# No installed ELF may have an RPATH or RUNPATH dynamic tag.
```

The pre-merge rpath correction revalidated default and all-signers builds and
normal tests: macOS **25/25**, **28/28**; Linux **26/26**, **29/29**, no failures or
skips. Both Linux configurations were installed with the prefix/libdir above.
`readelf -d` confirmed **no RPATH/RUNPATH** on all three installed producer ELFs
(host, client, libmetadesk), without running `patchelf`. Their build-tree host
executables still had `$ORIGIN/:/home/bizarro/nostrc/_build/libjson` RUNPATH, and
`ldd` on the all-signers build resolved canonical build-tree JSON. Both macOS
configurations also staged successfully: installed host/client/libmetadesk had
no `LC_RPATH`, while the build libraries retained the JSON lookup path. No full
container closure or in-image test was rerun for this dependency-only correction.

Nostr Protocol Review of this build/docs-only diff found no protocol-behavior
changes: no polling, timer closes, relay frame handling, filters, or subscription
lifecycles were introduced or modified. This is not a live relay/signing or
interactive-desktop qualification.

## Historical md-dpl qualification (before md-66n, 2026-11-17)

All ten platform/configuration combinations configured, built, and linked without
pkg-config shims. The normal test gate is **not fully green**:

| Enabled options | Linux normal tests | macOS normal tests | macOS isolated JSON selection |
| --- | --- | --- | --- |
| none (defaults) | 22/22 | 22/22 | not needed |
| NIP-46 only | 23/23 | 1 pass, 22 dyld aborts (md-66n) | 23/23 |
| NIP-55L only | 23/23 | 23/23 | not needed |
| NIP-5F only | 23/23 | 1 pass, 22 dyld aborts (md-66n) | 23/23 |
| all three | 25/25 | 1 pass, 24 dyld aborts (md-66n) | 25/25 |

Linux used `bizarro@192.168.64.3:~/wt-dpl`, the existing
`/home/bizarro/nostrc/_build`, and `-j2`. Its required backend libraries and system
packages were already present; no packages or nostrc targets needed building.
Tests had `DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent` and
`NOSTRC_TEST_ISOLATION=1`; the live GNOME session, keyring and accessibility bus
were not used.

macOS used the private nostrc build above. Installed NIP-55L lacked
`nostr_nip55l_sign_event_json`; the configure probe correctly selected the private
archive. Standalone NIP-5F's installed archive also needed undeclared GLib server
symbols; the server link probe selected its compatible private archive.

**Isolated results are diagnostic evidence, not the normal Meson gate.** Only in
ignored generated `build.ninja` files, the legacy JSON link path was replaced by
`/tmp/md-dpl-nostrc-lws5/libjson/libnostr-json.dylib`; these manifests were rebuilt
with Ninja, then tests ran with `--no-rebuild` and
`DYLD_LIBRARY_PATH=/tmp/md-dpl-nostrc-lws5/libjson`. No tracked core discovery,
source files, installed libraries, pkg-config files, or ABI aliases were changed.
With that explicit fresh-JSON selection, the all-enabled build additionally ran:

```sh
NOSTRC_TEST_ISOLATION=1 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  DYLD_LIBRARY_PATH=/tmp/md-dpl-nostrc-lws5/libjson \
  meson test -C build-dpl-all --no-rebuild --repeat=100 --num-processes=1 \
    'nostr NIP-44'
```

Result: **100/100**, zero failures. The shared-tree cache warning was separately
verified against the untouched shared build; the current header/library probe
passed. At that point md-dpl remained open pending md-66n and an unmodified macOS
test gate; see the current qualification below.
