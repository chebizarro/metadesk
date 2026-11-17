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

Meson tries installed libraries first (including the Homebrew prefix on macOS),
then libraries under `nostrc_build_dir/nips/<component>`. Adapter libraries contain
multiple objects, so the fallback links their archive rather than guessing one
object file. A link check rejects installed backend archives missing the entry
points Metadesk uses and tries their build-tree counterpart. Source include paths,
generated export-header paths, and nested installed adapter include paths are
added only for enabled backends.

| Option | nostrc targets beyond the common core | System dependencies |
| --- | --- | --- |
| `signer_nip46` | `nostr_nip46_core`, `nip04` | Jansson |
| `signer_nip55l` | `nostr_nip55l_core`, `nip04`, `nip06`, `nostr_nip49_glib`, `nostr_nip49_core` (also common `nip19`/NIP-44) | GIO >= 2.76, libsodium; Security + CoreFoundation on macOS; libsecret-1 + `gnostr-secret` on Linux |
| `signer_nip5f` | `nostr_nip5f_core` (also common `nip19`/NIP-44) | common core dependencies |

For Linux, `gnostr-secret` can also come from
`nostrc_build_dir/gnome/seahorse`. Missing archives produce a configure error
naming the nostrc target to build. Build missing targets with
`cmake --build /path/to/nostrc/_build --target <target> --parallel 2`.

## libwebsockets upgrades and md-k1k

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

**A second, separate blocker remains (md-66n):** the common Metadesk JSON-library
discovery still selects `/opt/homebrew/lib/libnostr_json.dylib`. That legacy
installed library needs the now-absent `libwebsockets.21.dylib`, so an unmodified
macOS test run aborts in dyld even after rebuilding private `libnostr.a`. Current
nostrc builds `libjson/libnostr-json.2.dylib` with canonical `libnostr-json.pc`
metadata instead. Rebuilding the shared tree alone will not replace the old
underscore-named install; core JSON discovery must select the new library.
This is outside the signer-only build-edit scope. Extra `c_link_args` do not
solve it: the stale library remains a load dependency.

## Qualification (2026-11-17)

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
passed. md-dpl remains open pending md-66n and an unmodified macOS test gate.
