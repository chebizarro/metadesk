# Private sanitizer builds

Use nostrc master including the NIP-42 session API and metric-handle race fix
`7d8b55724`. Never rebuild a shared build tree for sanitizer tests.

On Linux, TSan must instrument **both nostrc and nsync**: libnostr and libgo
use nsync mutexes, channels and wait groups. Uninstrumented nsync hides their
happens-before edges. No race suppressions are used.

```sh
cmake -S "$nostrc/third_party/nsync" -B "$private/nsync-tsan" \
  -DBUILD_SHARED_LIBS=ON -DNSYNC_ENABLE_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS='-fsanitize=thread -g' \
  -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread
cmake --build "$private/nsync-tsan" --target nsync -j2
cmake -S "$nostrc" -B "$private/nostrc-tsan" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS='-fsanitize=thread -g' \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -g' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
  -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread \
  -DBUILD_APPS=OFF -DBUILD_TESTING=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF \
  -DENABLE_NIP46=ON -DENABLE_NIP55L=ON -DENABLE_NIP5F=ON
cmake --build "$private/nostrc-tsan" -j2 --target \
  nostr nostrgo go_fiber nostr_nip51_core nostr_nip78_core nostr_nip98 nostr_nip17_core \
  nip11 nostr_json nostr_nip46_core nostr_nip55l_core nostr_nip5f_core
meson setup build-tsan -Db_sanitize=thread -Db_lundef=false --buildtype=debug \
  -Dnostrc_root="$nostrc" -Dnostrc_build_dir="$private/nostrc-tsan" \
  -Dsigner_nip46=true -Dsigner_nip55l=true -Dsigner_nip5f=true
LD_LIBRARY_PATH="$private/nsync-tsan" TSAN_OPTIONS=halt_on_error=1 \
  setarch "$(uname -m)" -R meson test -C build-tsan -j2 \
  'nostr NIP-44' 'nostr ACL and AUTH private relays'
```

Keep libgo's normal channel options (no `GO_ENABLE_TSAN`); test the MPMC slot
paths that ship. `setarch -R` avoids the Linux aarch64 TSan/ASLR mapping issue.
Repeat protocol fixtures to exercise scheduling, not sleeps or polling.
Instrumented metadesk alone is not fully instrumented dependency evidence.

For ASan use another private tree with `address` in place of `thread` and
`-Db_sanitize=address`; do not reuse the TSan archives. Run the Nostr and MCP
tests against each build. Delete private trees only after collecting logs.
