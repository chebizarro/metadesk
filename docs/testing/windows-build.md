# Native Windows build qualification

## Status

Round 2 ports metadesk master `0b5d93861e29b4926c0dbe82c7a42b812764ce50`
on lane `md/l9-windows-build`. Native Windows host and client now build and
print usage; the host captures a real local frame in the interactive console.
This is **not** an authenticated cross-OS product-session qualification. The
written `md-z9pd` acceptance criterion for authenticated macOS-Windows checks
remains unverified; do not close that criterion on usage/capture evidence alone.

Environment: Windows 11 ARM64 VM `192.168.64.4`, MSYS2 `CLANGARM64` under
`C:\msys64`, clang 23.1.5, Meson 1.12.1, CMake 4.5.0, Ninja 1.13.2.
All Windows source/build work is isolated in `C:\Users\bizarro\md-l9`.
Never build in the macOS main checkout or another lane's VM tree.

### Qualified gates

| Gate | Result |
| --- | --- |
| nostrc selected library targets at `ea5ed954267c2b513eaa0f41acf3166fdc7f4951` | Build exit 0 (`BUILD_FIPS=OFF`) |
| Windows native Meson setup + full build, client/tools enabled | Exit 0 |
| Windows-applicable Meson suite | 29 passed, 0 failed; conditional cells listed below |
| macOS Meson suite in L9 worktree | 37 passed, 0 failed |
| Linux VM `/home/bizarro/md-l9`, build and full suite `-j2` | Exit 0; 36 passed, 0 failed |
| Linux ASan+UBSan focused stream/MCP/host-binding gates | Exit 0; 6 passed, 0 failed, leak detection enabled |
| Native `metadesk-host.exe --help` | Exit 0, usage printed |
| Native `metadesk-client.exe --help` | Exit 0, usage printed |
| Interactive `metadesk-host.exe capture-frame host-frame.raw` | Exit 0; 1024x768 BGRA, stride 4096, 3,145,728 bytes |
| Authenticated Windows-host/macOS-client direct-IP session | Not run; no public relay used |

### Port boundaries

- `md_socket` keeps Windows `SOCKET` values pointer-width, distinct from CRT
  descriptors. It owns Winsock initialization, error normalization,
  `closesocket`, `ioctlsocket`, `WSAPoll` and Windows socket timeout values.
  Listeners use `SO_EXCLUSIVEADDRUSE`, not Windows's permissive
  `SO_REUSEADDR`; Unix retains address reuse. Numeric IPv4-mapped IPv6 connects
  normalize to their IPv4 endpoint only on Windows, where `getaddrinfo` rejected
  the mapped literal. TLS refuses a handle that OpenSSL's int socket API cannot
  represent rather than silently narrowing it.
- Stream and MCP HTTP socket I/O use that shim. Windows accepted HTTP sockets
  are nonblocking before deadline-bounded I/O; Unix retains per-call flags.
  No relay filters, signing, AUTH recovery or session authorization changed.
- Native stdio uses borrowed CRT handles with joinable workers and
  `CancelSynchronousIo`; cancellation completion is joined before buffers or
  request state can expire. A potentially partial timed-out write poisons the
  output channel. Borrowed stdin is not closed or put into another mode.
- Winpthreads rejects `CLOCK_MONOTONIC` condattrs in this VM. Relative condvar
  waits now carry the remaining QPC deadline; Linux/macOS clock paths remain
  equivalent to their previous behavior. Native console events replace POSIX
  signal waits, without calling bridge teardown from a console callback.
  Actual console-close/logoff delivery was not tested by disrupting the seat.
- Small Windows CRT helpers supply missing formatting, string/line routines
  and non-inherited file opens; client startup explicitly initializes SDL main.
- `metadesk-host capture-frame OUTPUT.raw [DISPLAY]` is a local diagnostic using
  the production capture HAL. It creates no listener or relay, requires actual
  CPU pixels, validates the complete file write, and fails clearly for unsupported
  buffer types. It does **not** bypass authorization in normal video mode.

API rationale: [WSAPoll](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll)
operates on sockets, not CRT fds;
[CancelSynchronousIo](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio)
requests cancellation but does not join completion. The worker retains ownership
until its thread has exited, including cancellation-before-syscall races.

### Explicitly unsupported or unqualified

- Windows account/OS-credential persistence remains explicitly unsupported (`md-wmuo`);
  there is no plaintext credential fallback. `metadesk-host account list` was
  checked natively: exit 1 with the explicit Linux/macOS-only persistence error. Default saved-account video startup
  therefore fails clearly. Explicit remote signer builds need separate qualification;
  this run leaves NIP-46/NIP-55L/NIP-5F optional backends disabled.
- Windows MCP bearer-token files (`md-ezbe`) are rejected with an explanatory log: Unix
  mode 0600 is not Windows ACL validation. NIP-98 authorization remains available
  and its applicable private tests pass. A native test checks bearer configuration
  fails closed. No token-file validation is silently bypassed.
- Windows FIPS control remains unsupported (`md-m1we`), unchanged by this port.
- Unix fork/signal process fixtures are not Windows tests. Native framing,
  EOF, blocked/partial input shutdown, broken/stalled output, descriptor reuse
  and handle ownership are covered by dedicated Windows tests instead.
- Five HTTP test functions requiring POSIX 0600 bearer files are explicitly
  skipped on Windows, as is the Nostr account-constructor revocation cell.
  Common NIP-98 authorizer, stream/TLS/binding and private ACL/AUTH/session tests
  still run. Unix socket-path collision/runtime-dir cells are inapplicable to
  named pipes; common IPC behavior runs against the actual Windows backend.
- No Windows live UIA, lock/disconnect, sanitizer, installer/DLL bundle or
  authenticated cross-OS acceptance claim follows from this native suite.

The exact handle-stability assertion is in a target compiling unchanged production
I/O sources with only their direct dependencies. A whole-product process count
was demonstrably confounded: a no-I/O control's loaded-DLL handles rose from 204
to 1395 during idle startup, later settled at 651, and stayed there. The final ownership gate measured **68 handles before and after**. The isolated
I/O diagnostic control stayed at 97 through repeated cancellation/EOF/write cycles. Rather
than add a success sleep or relax an assertion, ownership coverage is isolated
from unrelated asynchronous DLL initialization; full-product stdio integration
also remains tested. The diagnostic idle intervals are not part of the gates.

## Linux pre-merge verification of `cbf3b76`

The unmodified port passed the full baseline Linux suite (36/36). That first
pass exposed a safety preflight miss: `test_agent` called a real input
constructor before its no-a11y skip, independently of `MD_INPUT_LIVE_TESTS`.
On this VM the UID has `/dev/uinput` access. The contexts were destroyed on the
skip path and no actions were dispatched through them, but this was not a
valid device-free run and must not be described as one.

The follow-up changes **only the agent test and this guide**, not production
stream/MCP/I/O code: remove the unused input constructor/cleanup from its
tree-only smoke and require `MD_A11Y_LIVE_TESTS=1` before creating live a11y.
All mocked input assertions remain; no timeout or assertion was relaxed.
After the fix, Linux passed **36/36** and macOS passed **37/37** again. The
Windows results above refer to the original port qualification, not a new
Windows rerun for this test-only follow-up.

Linux used kernel 7.0.0-42-generic/aarch64, GCC 13.3.0, Meson 1.3.2, and the
unchanged dedicated nostrc tree at `cfcb86296`. Both build and test concurrency
were limited to `-j2`. HOME, XDG runtime/state/cache and the session-bus address
were private to `/home/bizarro/md-l9`; live display variables were unset and
both live-test opt-ins were 0. L7's directory and GNOME session configuration
were not modified; no containers were used.

For the final full run a task-owned, unprivileged
[Landlock](https://docs.kernel.org/userspace-api/landlock.html) launcher denied
host input-device access, with an additional process-local seccomp trap on
`UI_DEV_CREATE`. System libraries/seat metadata were read-only; task directories
and temporary files remained writable. Read-only `/run/systemd` metadata is
necessary for private D-Bus UID/seat authorization: an earlier overly strict
launcher denied it and caused three fixture failures, not shared-code failures.
Restoring that narrow read-only prerequisite fixed those fixtures without
changing their assertions or global security policy.

The final `strace` audit found **zero `/dev/uinput` open attempts and zero
successful input-device opens**. The existing offscreen SDL negative-startup
fixture made nine read-only physical input probes; all nine were denied by
the process policy. The audit, rather than an unconditional pass, checked the
actual syscall results. No success sleep was added to the gates.

The sanitizer profile disabled only client/tools and live opt-ins; it built
`libmetadesk` and these test executables with `-Db_sanitize=address,undefined`:

```sh
meson compile -C build-asan -j2 \
  test_stream test_mcp test_mcp_stdio test_mcp_http test_host_binding
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./safe-run meson test -C build-asan --no-rebuild --print-errorlogs -j2 \
    'TCP stream transport' 'MCP server core' 'MCP accessibility queries' \
    'MCP stdio transport' 'MCP HTTP+SSE transport' host_binding
```

`safe-run` is the task-owned process restriction launcher described above,
not a production variant or container. The sanitized run was not under ptrace,
so leak detection remained active; it passed all six tests without suppressions.
Compile commands confirmed instrumentation of `stream.c`, `mcp_http.c`,
`mcp_stdio.c` and `io_util.c`. Shared `io_util` is exercised by the stdio and HTTP
transport tests; the standalone Windows I/O test is not a Linux target.
Third-party/nostrc dependencies were reused without a sanitizer rebuild, so
this is not a fully instrumented dependency-closure claim.

The entire Linux `md-l9` directory (source, normal/sanitized builds, private
runtime and evidence) and local task scratch were removed after recording the
results in the bead. No work in `md-l7` or the live GNOME session was cleaned up.

## Provisioning

PowerShell over SSH is session 0. Transfer a script rather than passing a complex
quoted shell pipeline through PowerShell 5.1. Launch scripts with:

```powershell
$env:MSYSTEM = 'CLANGARM64'
$env:CHERE_INVOKING = '1'
& C:\msys64\usr\bin\bash.exe -lc 'bash /c/Users/bizarro/md-l9/build.sh'
exit $LASTEXITCODE
```

Inside that native shell, L9 ran:

```sh
cd /c/Users/bizarro/md-l9
pacman -Q > packages-before.txt
pacman -S --needed --noconfirm \
  mingw-w64-clang-aarch64-ffmpeg \
  mingw-w64-clang-aarch64-SDL2 \
  mingw-w64-clang-aarch64-libyuv \
  mingw-w64-clang-aarch64-cmake \
  mingw-w64-clang-aarch64-libwebsockets \
  mingw-w64-clang-aarch64-jansson
pacman -Q > packages-after.txt
```

CMake, libwebsockets and Jansson were already installed and unchanged. The
resolver selected **sdl2-compat 2.32.74-5**, backed by SDL3 3.4.20-1, instead of
the requested legacy SDL2 package. FFmpeg is 9.0.2-4; libyuv is
1924.r2921.644251f25-2. All 83 packages were added in round 1, not reinstalled in round 2, and remain installed;
the complete transaction inventory is below.

This VM already had `mingw-w64-clang-aarch64-nsync 1.30.0-3` (no `nsync.pc`,
but a working `libnsync.dll.a`) and a separately installed `libsecp256k1` 0.8.1.
No CLANGARM64 secp256k1 package was found in the configured repository; an initial
transaction including that nonexistent package failed without installing
anything. The corrected transaction above succeeded. For a clean VM, use the
source-build fallback in nostrc's
[Windows validation workflow](https://github.com/chebizarro/nostrc/blob/6f7a8d025768d497bef700fe55eea1c687dac328/.github/workflows/windows-validation.yml),
adapting `/ucrt64` to `/clangarm64`. That source fallback was **not** rerun or
qualified by L9; the provenance of the pre-existing secp256k1 source was not
established. The VM also had `SigLevel = Never` before L9; L9 did not change
signature policy. This is test-VM evidence, not a secure clean-machine install.

Observed pkg-config versions after provisioning:

```text
libavcodec     63.1.102
libavutil      61.1.102
sdl2           2.32.74
libyuv         0.0.1924
libcjson       1.7.19
libb2          0.98.1
libcurl        8.23.0
jansson        2.15.1
libsecp256k1   0.8.1
libwebsockets  4.5.8-unknown
openssl        4.1.0
```

## Build nostrc from source

Use the current packaging pin, `6f7a8d025768d497bef700fe55eea1c687dac328`,
in a private directory. The L9 results above are historical and used a
different revision. With nostrdb disabled, the selected targets do not need
source submodules.

```sh
cd /c/Users/bizarro/md-6u0a
rev=6f7a8d025768d497bef700fe55eea1c687dac328
curl -fL "https://github.com/chebizarro/nostrc/archive/$rev.tar.gz" -o nostrc.tar.gz
mkdir nostrc
tar -xzf nostrc.tar.gz --strip-components=1 -C nostrc
```

The initial configure reached unrelated `apps/grelay` and failed for missing
libsoup-3.0; disable applications rather than provisioning unrelated apps.
The successful configuration and build were:

```sh
cmake -S nostrc -B nostrc/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTING=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
  -DBUILD_SIGNER_TESTS=OFF -DLIBNOSTR_BUILD_TESTS=OFF \
  -DBUILD_APPS=OFF -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_LIBBLOSSOM=OFF -DBUILD_LIBPORTHOME=OFF \
  -DBUILD_LIBVAULT=OFF -DBUILD_LIBVAULT_MARMOT=OFF \
  -DBUILD_NATIVE_HOST=OFF -DBUILD_WINDOWS_INSTALLER=OFF \
  -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF -DBUILD_LIBHANAMI=OFF \
  -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF -DBUILD_RELAYD=OFF \
  -DBUILD_FIPS=OFF -DSIGNET_ENABLE=OFF \
  -DENABLE_NIP55L=OFF -DENABLE_NIP5F=OFF -DENABLE_NIP57=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF
cmake --build nostrc/build --target \
  nostr nostr_json go_fiber nostr_nip44_core nip19 nostr_nip51_core \
  nostr_nip98 nip11 nostr_nip17_core -j2
```

This builds the libraries metadesk consumes, not all nostrc applications or its
test suite. NIP-17 and NIP-51 are object libraries; metadesk correctly reads
their `.obj` paths from CMake's Ninja manifest. `nostr_json` is explicitly shared
even with `BUILD_SHARED_LIBS=OFF`, producing `libjson/libnostr-json.dll` and
`libjson/libnostr-json.dll.a`. The L9 Meson fix recognizes the import library
and uses `cc.find_library` in the selected tree, preserving existing behavior
on macOS/Linux and avoiding a silently selected system library.

## Configure, build and run metadesk

**Media Foundation prerequisite:** the stock MSYS2 FFmpeg 9.0.2 package above
is a compilation dependency, not a qualified low-latency runtime. Its
`mfenc.c` sends `CODECAPI_AVLowLatencyMode` as `VT_UI4(1)`; the H.264 encoder
requires `VT_BOOL(VARIANT_TRUE)`. The resulting buffered output also fails
`capture geometry sync` and `host_binding`, not just `encode/decode round-trip`.
Use the corrected task-local FFmpeg runtime described in
[Windows FFmpeg integration requirement](../../src/core/encoder-runtime.md#windows-ffmpeg-integration-requirement)
before qualifying those tests. Do not force another encoder, submit extra
frames, relax assertions or extend timeouts to hide this dependency bug.

Export the assigned metadesk tree, including `imgui_vendor`, this port's `src`
and `tests`, and Meson files, to `md-6u0a/metadesk`; omit `.git`, `.beads` and
local build directories. Do not install nostrc globally or use a sibling tree.

```sh
cd /c/Users/bizarro/md-6u0a
meson setup metadesk/build-product metadesk \
  -Dnostrc_discovery=build-tree \
  -Dnostrc_root=C:/Users/bizarro/md-6u0a/nostrc \
  -Dnostrc_build_dir=C:/Users/bizarro/md-6u0a/nostrc/build \
  -Dtools=true
meson compile -C metadesk/build-product -j2
export PATH="/c/Users/bizarro/md-6u0a/nostrc/build/libjson:$PATH"
meson test -C metadesk/build-product --print-errorlogs -j2
./metadesk/build-product/metadesk-host.exe --help
./metadesk/build-product/metadesk-client.exe --help
```

For an already compiled build, a controlled MF runtime comparison can use
ABI-matching corrected DLLs beside the test/product executables. In the native
MSYS2 shell, set `MD_FFMPEG_PREFIX` to the private corrected FFmpeg install:

```sh
: "${MD_FFMPEG_PREFIX:?set the corrected task-local FFmpeg install prefix}"
cp "$MD_FFMPEG_PREFIX/bin/avcodec-63.dll" \
   "$MD_FFMPEG_PREFIX/bin/avutil-61.dll" metadesk/build-product/
meson test -C metadesk/build-product --print-errorlogs -j2
```

Use only the assigned build directory; never replace the shared MSYS2 DLLs.
Setting PATH alone is insufficient to establish which runtime was tested:
Meson prepends dependency directories, and Windows searches the executable's
directory first. Record the loaded module paths, DLL hashes and
`avcodec_configuration()`/`avcodec_license()` with the test result. A private
MF-only runtime comparison does not qualify the full GPU-enabled packaging
profile or distribution of those DLLs.

Both products are enabled by default. Runtime lookup needs native MSYS2 bin,
the metadesk build directory, and the selected nostrc `build/libjson` on PATH;
Windows has no build rpath. The usage checks were also run from PowerShell with
that PATH, not just from an MSYS shell. Optional signer backends are not enabled
by this baseline. DLL distribution/packaging is not qualified by this recipe.

On macOS, L9 configured its own `build-l9-macos` with
`-Dnostrc_root=/Users/bizarro/Documents/Projects/nostrc-wt/metadesk-master`
and the corresponding `-Dnostrc_build_dir=.../_build`, then compiled and ran
`meson test --print-errorlogs -j2`. Linux used dedicated `/home/bizarro/md-l9`,
`-Dnostrc_root=/home/bizarro/nostrc-master`, corresponding `_build`, and `-j2`.
Those dependency trees were read-only; L9 used no containers or live PipeWire
resources and did not modify L10's work.

## Interactive host capture

Do not lock/disconnect the signed-in console. SSH/session 0 is not a DXGI
capture environment. Use the existing runner with an isolated `.cmd`:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\bizarro\md-l9\metadesk\tools\win\run-interactive.ps1 `
  -Command C:\Users\bizarro\md-l9\capture.cmd `
  -DoneFile C:\Users\bizarro\md-l9\capture.done
```

The command runs the real product:

```bat
set PATH=C:\Users\bizarro\md-l9\metadesk\build-product;C:\Users\bizarro\md-l9\nostrc\build\libjson;C:\msys64\clangarm64\bin;%PATH%
metadesk-host.exe capture-frame C:\Users\bizarro\md-l9\host-frame.raw
```

The `.cmd` records the program's exit status before writing its done file.
Runner exit 0 alone only proves completion; check the recorded program status.
L9's VM display powers off despite `ES_DISPLAY_REQUIRED`. Its task-owned helper
used the existing `tests/test_capture_dxgi_win.cpp` display-watch pattern: create
an owned window, register `GUID_CONSOLE_DISPLAY_STATE`, send `SC_MONITORPOWER`
only to that window, and re-wake on display-off notifications. It launches the
real host child and dispatches window messages until child completion, without
injected input or success sleeps. This helper is test orchestration, not a
production keep-awake change. Use an awake interactive desktop when reproducing.

Observed product output:

```text
INFO: capture_dxgi: duplicating output 0 \\.\DISPLAY1 (1024x768 at 0,0, rotated 0 degrees)
host: captured one frame: 1024x768 stride=4096 format=1 bytes=3145728 output=C:\Users\bizarro\md-l9\host-frame.raw
```

The final frame SHA-256 was `fc3e8130b44e789b080b47e279bb8db20ad574804cd3a7190c119a617f8f70ce`.

This demonstrates host-local capture, not encoder/network delivery. The optional
authenticated Windows-host/macOS-client `--host ADDRESS --npub HOST_NPUB` session
was not run. Do not substitute this diagnostic or a standalone media harness for
that pairing's acceptance evidence.

## Review and cleanup

The inline-scoped Oracle review found no actionable defects; it did not inspect
workspace files or independently run tests. The diff-scoped Nostr Protocol Review
found no new A-J protocol smells: relay publish/subscription/EOSE/OK/CLOSED/AUTH
handling and grants were not changed; private ACL/AUTH/session gates still run.
This is not a repository-wide Nostr audit.

Cleanup exited 0 on both VMs. Before Windows deletion, task-owned running
processes and scheduled tasks were both **0**. The retained FFmpeg, SDL2-compat
and libyuv package versions were queried again successfully.

After recording the final gates, L9 removed the **entire** task-owned
`C:\Users\bizarro\md-l9` directory, including round-1 evidence, product/nostrc
source and builds, raw frame, source archives, private helper executables and
logs. Interactive tasks were unregistered by the runner. The Linux `md-l9`
source/build was removed; local task scratch/build directories are also removed. The
83 MSYS2 additions below intentionally stay installed. Counts, versions,
results and cleanup receipts are recorded in the bead; no sibling checkout,
VM resource, branch or stash is cleaned up.

## Exact package additions

All names below have the `mingw-w64-clang-aarch64-` prefix. These are the complete
before/after `pacman -Q` additions, not a list inferred from dependencies.

```text
mingw-w64-clang-aarch64-aom 3.15.3-1
mingw-w64-clang-aarch64-cairo 1.18.6-2
mingw-w64-clang-aarch64-dav1d 1.5.4-1
mingw-w64-clang-aarch64-fc-libs 23.1.5-1
mingw-w64-clang-aarch64-ffmpeg 9.0.2-4
mingw-w64-clang-aarch64-fontconfig 2.18.3-1
mingw-w64-clang-aarch64-freetype 2.15.0-1
mingw-w64-clang-aarch64-frei0r-plugins 3.7.1-1
mingw-w64-clang-aarch64-fribidi 1.0.18-1
mingw-w64-clang-aarch64-gdk-pixbuf2 2.44.8-1
mingw-w64-clang-aarch64-ggml 1.0.0-1
mingw-w64-clang-aarch64-giflib 6.1.3-1
mingw-w64-clang-aarch64-glslang 16.6.0-1
mingw-w64-clang-aarch64-graphite2 1.3.15-1
mingw-w64-clang-aarch64-gsm 1.0.24-1
mingw-w64-clang-aarch64-harfbuzz 15.1.0-1
mingw-w64-clang-aarch64-highway 1.4.0-3
mingw-w64-clang-aarch64-imath 3.2.3-3
mingw-w64-clang-aarch64-jbigkit 2.1-6
mingw-w64-clang-aarch64-lame 4.0-1
mingw-w64-clang-aarch64-lcms2 2.20-1
mingw-w64-clang-aarch64-lerc 4.1.1-1
mingw-w64-clang-aarch64-libass 0.17.5-1
mingw-w64-clang-aarch64-libbluray 1.5.0-1
mingw-w64-clang-aarch64-libcaca 0.99.beta20-8
mingw-w64-clang-aarch64-libdatrie 0.2.14-1
mingw-w64-clang-aarch64-libdeflate 1.26-1
mingw-w64-clang-aarch64-libdovi 3.4.0-1
mingw-w64-clang-aarch64-libexif 0.6.26-1
mingw-w64-clang-aarch64-libgme 0.6.5-2
mingw-w64-clang-aarch64-libjpeg-turbo 3.2.1-1
mingw-w64-clang-aarch64-libjxl 0.13.0-1
mingw-w64-clang-aarch64-liblc3 1.1.3-1
mingw-w64-clang-aarch64-libmodplug 0.8.9.0-5
mingw-w64-clang-aarch64-libmysofa 1.3.5-1
mingw-w64-clang-aarch64-libogg 1.3.6-1
mingw-w64-clang-aarch64-libplacebo 7.387.0-1
mingw-w64-clang-aarch64-libpng 1.6.59-2
mingw-w64-clang-aarch64-librsvg 2.63.4-1
mingw-w64-clang-aarch64-libsoxr 0.1.3-6
mingw-w64-clang-aarch64-libssh 0.12.2-2
mingw-w64-clang-aarch64-libthai 0.1.30-1
mingw-w64-clang-aarch64-libtheora 1.2.0-2
mingw-w64-clang-aarch64-libtiff 4.7.2-2
mingw-w64-clang-aarch64-libunibreak 7.0-1
mingw-w64-clang-aarch64-libva 2.24.1-1
mingw-w64-clang-aarch64-libvorbis 1.3.7-3
mingw-w64-clang-aarch64-libvpl 2.17.0-1
mingw-w64-clang-aarch64-libvpx 1.17.0-1
mingw-w64-clang-aarch64-libwebp 1.6.0-1
mingw-w64-clang-aarch64-libx264 0.165.r3222.b35605a-3
mingw-w64-clang-aarch64-libyuv 1924.r2921.644251f25-2
mingw-w64-clang-aarch64-llama.cpp 1~1.0.0-2
mingw-w64-clang-aarch64-lzo2 2.10-3
mingw-w64-clang-aarch64-mpg123 1.33.7-1
mingw-w64-clang-aarch64-openal 1.25.2-1
mingw-w64-clang-aarch64-openblas 0.3.34-1
mingw-w64-clang-aarch64-opencl-icd 2026.05.29-1
mingw-w64-clang-aarch64-opencore-amr 0.1.6-1
mingw-w64-clang-aarch64-openexr 3.5.4-2
mingw-w64-clang-aarch64-openjpeg2 2.5.4-2
mingw-w64-clang-aarch64-openjph 0.33.0-1
mingw-w64-clang-aarch64-opus 1.6.1-2
mingw-w64-clang-aarch64-pango 1.58.2-1
mingw-w64-clang-aarch64-pixman 0.46.4-3
mingw-w64-clang-aarch64-rav1e 0.8.1-1
mingw-w64-clang-aarch64-rtmpdump 2.6-2
mingw-w64-clang-aarch64-sdl2-compat 2.32.74-5
mingw-w64-clang-aarch64-sdl3 3.4.20-1
mingw-w64-clang-aarch64-shaderc 2026.4-1
mingw-w64-clang-aarch64-speex 1.2.1-1
mingw-w64-clang-aarch64-speexdsp 1.2.1-2
mingw-w64-clang-aarch64-spirv-cross 1~1.4.363.0-1
mingw-w64-clang-aarch64-spirv-tools 3~1.4.363.0-1
mingw-w64-clang-aarch64-srt 1.5.7-2
mingw-w64-clang-aarch64-svt-av1 4.3.0-1
mingw-w64-clang-aarch64-vid.stab 1.1.2-1
mingw-w64-clang-aarch64-vulkan-loader 1~1.4.363.0-1
mingw-w64-clang-aarch64-whisper.cpp 1~1.9.5.r334.g5922450fb-1
mingw-w64-clang-aarch64-x265 4.3-1
mingw-w64-clang-aarch64-xvidcore 1.3.7-5
mingw-w64-clang-aarch64-zimg 3.0.6-3
mingw-w64-clang-aarch64-zvbi 0.2.46-1
```
