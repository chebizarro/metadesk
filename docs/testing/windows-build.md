# Native Windows build qualification

## Status: dependency closure reached; product portability blocked

L9 tested metadesk `3f66fcc46a0dcc1b56931f8803aee813f9bf09cc` plus the
`meson.build` nostr-json import-library fix on 2026-11-21 (VM receipt:
2026-11-22 02:28 UTC). This is **not** a successful full-product build or a
Windows E2E qualification. Bead `md-z9pd` must remain open.

Environment: Windows 11 ARM64 VM `192.168.64.4`, MSYS2 `CLANGARM64` under
`C:\msys64`, clang 23.1.5, Meson 1.12.1, CMake 4.5.0, Ninja 1.13.2.
All source/build work was isolated in `C:\Users\bizarro\md-l9`. Do not use a
sibling lane's source directory or run a native build in the macOS main checkout.

### Results

| Gate | Result |
| --- | --- |
| MSYS2 provisioning | Exit 0; 83 packages added, none removed |
| nostrc CMake configure, selected targets | Exit 0; 90 Ninja steps completed |
| metadesk Meson setup before fix | Exit 1: no recognized build-tree nostr-json import library |
| metadesk Meson setup after fix | Exit 0; 33 targets configured, client enabled, tools enabled |
| metadesk compile (`-j2`) | Exit 1: `src/core/io_util.c:4:10: fatal error: 'poll.h' file not found` |
| Full-product Meson test suite | Not run: compilation blocked |
| `metadesk-host.exe --help` | Not run: executable not produced |
| `metadesk-client.exe --help` | Not run: executable not produced |
| Host capture in interactive console | Not run: executable not produced; no task registered |
| Authenticated macOS-Windows E2E | Not run; neither provisioning nor component harnesses qualify this |

The failed compiler invocation used native CLANGARM64 headers, not MSYS POSIX
headers. `io_util.c` also depends on POSIX `sys/socket.h`, `poll`, pipe/file
semantics and nonblocking socket flags. This needs a real Windows I/O port,
not a header-search workaround or mixing MSYS and native ABI libraries.
`src/host/main.c:43` independently includes `poll.h` unconditionally and its
signal setup uses `struct sigaction`. L8 owns `src/host`, `src/client` and
session/Nostr work, so L9 stopped portability edits rather than changing those
files. A successful configure is not evidence that other translation units or
the linker are Windows-ready; Ninja stopped at the first failed core object.

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
1924.r2921.644251f25-2. All 83 added packages remain installed for later runs;
the complete transaction inventory is below.

This VM already had `mingw-w64-clang-aarch64-nsync 1.30.0-3` (no `nsync.pc`,
but a working `libnsync.dll.a`) and a separately installed `libsecp256k1` 0.8.1.
No CLANGARM64 secp256k1 package was found in the configured repository; an initial
transaction including that nonexistent package failed without installing
anything. The corrected transaction above succeeded. For a clean VM, use the
source-build fallback in nostrc's
[Windows validation workflow](https://github.com/chebizarro/nostrc/blob/ea5ed954267c2b513eaa0f41acf3166fdc7f4951/.github/workflows/windows-validation.yml),
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

L9 downloaded the GitHub source archive for pinned revision
`ea5ed954267c2b513eaa0f41acf3166fdc7f4951` into `md-l9/nostrc` (not a shared
checkout). GitHub's compare API reported it 61 commits ahead of `cfcb86296`,
zero behind. No nostrc source changes were made. With nostrdb disabled and
system nsync available, none of the source submodules was needed for the
selected targets. To reproduce the source extraction:

```sh
cd /c/Users/bizarro/md-l9
rev=ea5ed954267c2b513eaa0f41acf3166fdc7f4951
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

## Configure and attempt metadesk

Export the assigned metadesk worktree/commit to `md-l9/metadesk`, including
`imgui_vendor` and the L9 Meson change, but excluding `.git`, `.beads` and local
build directories. Do not install nostrc globally or point this at another
lane's tree.

```sh
cd /c/Users/bizarro/md-l9
meson setup metadesk/build-product metadesk \
  -Dnostrc_root=C:/Users/bizarro/md-l9/nostrc \
  -Dnostrc_build_dir=C:/Users/bizarro/md-l9/nostrc/build \
  -Dtools=true
meson compile -C metadesk/build-product -j2
# Blocked at io_util.c; only run these after the Windows port builds:
meson test -C metadesk/build-product --print-errorlogs
```

Both products are enabled by default. This baseline leaves optional NIP-46,
NIP-55L and NIP-5F signer backends disabled. It does not qualify authenticated
Windows sessions. Runtime lookup will also need the native MSYS2 bin directory
and the selected nostrc `build/libjson` directory on PATH; there is no Windows
rpath. DLL packaging remains untested until products link and run.

## Interactive follow-up (not executed by L9)

Do not lock/disconnect the signed-in console. SSH/session 0 is not a valid
DXGI capture environment. After the real products build, run usage checks and
`meson test`, then use the existing `tools/win/run-interactive.ps1` with an
isolated `.cmd` and done file in `md-l9`:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\bizarro\md-l9\metadesk\tools\win\run-interactive.ps1 `
  -Command C:\Users\bizarro\md-l9\smoke.cmd `
  -DoneFile C:\Users\bizarro\md-l9\smoke.done
```

The runner registers an Interactive-logon task, bounds failure, and unregisters
it in `finally`; its exit 0 only proves the done file appeared. The `.cmd` must
record each program's exit status separately before writing that file.
`capture_frame.exe frame.raw` uses the production capture library but is a
**diagnostic-tool** capture, not proof that `metadesk-host.exe` captured a frame.
Verify actual host capture separately; do not label component results as a
host smoke or authenticated E2E result. The VM's console display can power off
and make duplication idle: consult `bd memories windows-vm-dxgi --json` for the
existing display-wake harness behavior rather than adding success sleeps.

## Evidence and cleanup

The VM retains `md-l9/{packages-before.txt,packages-after.txt,receipt.txt,
nostrc-commit.txt,nostrc-configure.log,nostrc-build.log,metadesk-setup.log,
metadesk-build.log}` and the task's provisioning/command scripts. No scheduled
task was created. Task-owned metadesk source/build, nostrc source/build, and
source archives were removed after collecting logs. Installed MSYS2 packages
were intentionally retained. No sibling source, scheduled task, worktree,
branch or stash was modified or removed.

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
