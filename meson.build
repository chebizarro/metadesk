project('metadesk', ['c', 'cpp'],
  version : '0.1.0',
  default_options : [
    'c_std=c17',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
  meson_version : '>= 0.60.0',
)

# ── Platform detection ───────────────────────────────────────────────────────
host_os = host_machine.system()  # 'linux', 'darwin', 'windows'

if host_os == 'linux'
  # c_std=c17 (not gnu17) puts glibc in strict-ANSI mode, which hides POSIX and
  # GNU declarations the Linux HAL needs: struct addrinfo (stream.c,
  # fips_addr.c), F_DUPFD_CLOEXEC (capture_pipewire.c), and ssize_t/locale_t
  # used by PipeWire's spa/utils/string.h. Opt back in project-wide.
  add_project_arguments('-D_GNU_SOURCE', language : ['c', 'cpp'])
endif

# Tests assert through bare assert(); never let NDEBUG compile them out,
# or -Db_ndebug=true turns the suite into silent no-ops. No src/ file
# uses assert, so this only affects tests.
# (Tests assert via MD_CHECK in tests/md_test.h, which is never gated by
#  NDEBUG; src/ contains no assert() calls. No -UNDEBUG stopgap needed.)

# ── Options ──────────────────────────────────────────────────────────────────
enable_client  = get_option('client')
enable_nip46   = get_option('signer_nip46')
enable_nip55l  = get_option('signer_nip55l')
enable_nip5f   = get_option('signer_nip5f')

# ── Cross-platform dependencies (pkg-config) ────────────────────────────────
dep_avcodec   = dependency('libavcodec',      version : '>=60.0.0', required : true)
dep_avutil    = dependency('libavutil',        version : '>=58.0.0', required : true)
dep_cjson     = dependency('libcjson',        version : '>=1.7.17')
dep_b2        = dependency('libb2',           version : '>=0.98')
dep_threads   = dependency('threads')

# ── System library prefix detection ──────────────────────────────────────────
# Auto-detect Homebrew prefix on macOS (Apple Silicon: /opt/homebrew, Intel: /usr/local)
cc = meson.get_compiler('c')
_sys_lib_dirs = []
_sys_inc_dirs = []

if host_os == 'darwin'
  _brew = run_command('brew', '--prefix', check : false)
  if _brew.returncode() == 0
    _brew_prefix = _brew.stdout().strip()
  else
    _brew_prefix = '/opt/homebrew'
  endif
  _sys_lib_dirs = [_brew_prefix / 'lib']
  _sys_inc_dirs = [_brew_prefix / 'include']
  message('Homebrew prefix: ' + _brew_prefix)
endif

# Optional: libyuv has no pkg-config on most distros — try system, fallback to find_library
dep_yuv = dependency('libyuv', required : false)
if not dep_yuv.found()
  _yuv_lib = cc.find_library('yuv',
    dirs : _sys_lib_dirs + ['/usr/local/lib'],
    required : true)
  _yuv_incs = _sys_inc_dirs.length() > 0 ? include_directories(_sys_inc_dirs[0]) : include_directories()
  dep_yuv = declare_dependency(
    dependencies : _yuv_lib,
    include_directories : _yuv_incs,
  )
endif

# ── nostrc: Nostr C library ─────────────────────────────────────────────────
# nostrc components:
#   Installed (pkg-config/find_library): libnostr, nip44, secp256k1, etc.
#   Not yet installed (build-tree only): libgo runtime, nip51
# Use -Dnostrc_root=<path> to specify the nostrc project root for uninstalled
# components. Use -Dnostrc_build_dir=<path> to override the build directory.
_nostrc_root = get_option('nostrc_root')
_nostrc_build = get_option('nostrc_build_dir')
if _nostrc_build == '' and _nostrc_root != ''
  _nostrc_build = _nostrc_root / 'build'
endif

fs = import('fs')

# nostrc headers: prefer the source tree (it is ahead of the installed
# libnostr: publish_and_wait, event_middleware_ex, nostr_event_validate),
# fall back to the installed headers.
_nostrc_inc_paths = []
if _nostrc_root != ''
  _nostrc_inc_paths += _nostrc_root / 'libnostr' / 'include'
  _nostrc_inc_paths += _nostrc_root / 'libgo' / 'include'
  _nostrc_inc_paths += _nostrc_root / 'nips' / 'nip11' / 'include'
  _nostrc_inc_paths += _nostrc_root / 'nips' / 'nip17' / 'include'
  _nostrc_inc_paths += _nostrc_root / 'nips' / 'nip98' / 'include'
  _nostrc_inc_paths += _nostrc_root / 'nips' / 'nip19' / 'include'
  _nostrc_inc_paths += _nostrc_build / 'nips' / 'nip98' / 'include'
  _nostrc_inc_paths += _nostrc_build / 'nips' / 'nip19' / 'include'
endif
# nostrc generates export-macro headers (nostr-export.h, libgo-export.h,
# nostr-json-export.h) into the CMake build tree; the source headers include
# them unqualified.
if _nostrc_build != ''
  foreach d : ['libnostr', 'libgo', 'libjson']
    if fs.is_dir(_nostrc_build / d / 'include')
      _nostrc_inc_paths += _nostrc_build / d / 'include'
    endif
  endforeach
endif
foreach d : _sys_inc_dirs
  _nostrc_inc_paths += d / 'nostr'
endforeach
nostrc_inc = include_directories(_nostrc_inc_paths)

# libnostr: prefer the build-tree static lib (matches the headers above)
_nostrc_core = _nostrc_build / 'libnostr' / 'libnostr.a'
if _nostrc_build != '' and fs.exists(_nostrc_core)
  dep_nostr_lib = declare_dependency(link_args : [_nostrc_core])
  message('nostr: using build-tree libnostr: ' + _nostrc_core)
else
  dep_nostr_lib = cc.find_library('nostr', required : true, dirs : _sys_lib_dirs)
endif
dep_nostr = declare_dependency(
  dependencies : dep_nostr_lib,
  include_directories : nostrc_inc,
)
dep_nip44 = cc.find_library('nostr_nip44_core', required : true, dirs : _sys_lib_dirs)

# nip51 — try installed library first, fallback to build-tree object
dep_nip51_lib = cc.find_library('nostr_nip51_core', required : false, dirs : _sys_lib_dirs)
if dep_nip51_lib.found()
  dep_nip51 = dep_nip51_lib
elif _nostrc_build != ''
  _nip51_obj = _nostrc_build / 'nips' / 'nip51' / 'CMakeFiles' / 'nostr_nip51_core.dir' / 'src' / 'nip51.c.o'
  dep_nip51 = declare_dependency(link_args : [_nip51_obj])
  message('nip51: using build-tree object: ' + _nip51_obj)
else
  error('nostr_nip51_core not found. Install nostrc or set -Dnostrc_root=<path>')
endif

# NIP-98 HTTP auth and NIP-19 npub allowlist decoding.
dep_nip98_lib = cc.find_library('nostr_nip98', required : false, dirs : _sys_lib_dirs)
if dep_nip98_lib.found()
  dep_nip98 = dep_nip98_lib
elif _nostrc_build != ''
  _nip98_obj = _nostrc_build / 'nips' / 'nip98' / 'CMakeFiles' / 'nostr_nip98.dir' / 'src' / 'nip98.c.o'
  dep_nip98 = declare_dependency(link_args : [_nip98_obj])
  message('nip98: using build-tree object: ' + _nip98_obj)
else
  error('nostr_nip98 not found. Install nostrc or set -Dnostrc_build_dir=<path>')
endif
dep_nip19 = cc.find_library('nip19', required : true, dirs : _sys_lib_dirs)

# nostrc transitive deps: libgo runtime, libwebsockets, openssl, secp256k1, nsync, nostr_json
# libgo — try installed library first, fallback to build-tree static libs
_libgo_lib = cc.find_library('libgo', required : false, dirs : _sys_lib_dirs)
_go_fiber_lib = cc.find_library('go_fiber', required : false, dirs : _sys_lib_dirs)
if _libgo_lib.found() and _go_fiber_lib.found()
  dep_libgo = declare_dependency(dependencies : [_libgo_lib, _go_fiber_lib])
elif _nostrc_build != ''
  # nostrc renamed the libgo target to `nostrgo` (libnostrgo.a); older build
  # trees still have liblibgo.a.
  _libgo_a = _nostrc_build / 'libgo' / 'libnostrgo.a'
  if not fs.exists(_libgo_a)
    _libgo_a = _nostrc_build / 'libgo' / 'liblibgo.a'
  endif
  dep_libgo = declare_dependency(
    link_args : [
      _libgo_a,
      _nostrc_build / 'libgo' / 'libgo_fiber.a',
    ],
  )
  message('libgo: using build-tree static libs ' + _libgo_a + ' + libgo_fiber.a')
else
  error('libgo/go_fiber not found. Install nostrc or set -Dnostrc_root=<path>')
endif

dep_secp256k1 = cc.find_library('secp256k1', required : true, dirs : _sys_lib_dirs)
dep_websockets = cc.find_library('websockets', required : true, dirs : _sys_lib_dirs)
dep_ssl = cc.find_library('ssl', required : true, dirs : _sys_lib_dirs)
dep_crypto = cc.find_library('crypto', required : true, dirs : _sys_lib_dirs)
dep_nsync = cc.find_library('nsync', required : true, dirs : _sys_lib_dirs)
# Current nostrc names the artifact nostr-json and its pkg-config module
# libnostr-json. Prefer the selected build tree, just as for libnostr above;
# installed underscore-named dylibs may still require removed Homebrew ABIs.
_json_build_lib = ''
if _nostrc_build != ''
  foreach name : ['libnostr-json.dylib', 'libnostr-json.so', 'libnostr-json.a']
    candidate = _nostrc_build / 'libjson' / name
    if _json_build_lib == '' and fs.exists(candidate)
      _json_build_lib = candidate
    endif
  endforeach
endif
if _json_build_lib != ''
  # A typed library dependency lets Meson manage the build rpath and remove
  # it at install time. Raw -Wl,-rpath link_args would survive installation.
  dep_nostr_json = declare_dependency(
    dependencies : [
      cc.find_library('nostr-json', dirs : [_nostrc_build / 'libjson'], required : true),
      dependency('jansson'),
    ],
  )
  message('nostr-json: using build-tree library: ' + _json_build_lib)
else
  dep_nostr_json = dependency('libnostr-json', required : false, method : 'pkg-config')
  if not dep_nostr_json.found()
    dep_nostr_json = dependency('nostr-json', required : false, method : 'pkg-config')
  endif
  if not dep_nostr_json.found()
    _json_lib = cc.find_library('nostr-json', required : false, dirs : _sys_lib_dirs)
    if _json_lib.found()
      dep_nostr_json = declare_dependency(
        dependencies : [_json_lib, dependency('jansson')],
      )
    elif host_os == 'darwin'
      error('Current nostr-json not found. Install libnostr-json pkg-config metadata/library, ' +
            'or build the nostrc nostr_json target and set -Dnostrc_build_dir=<path>. ' +
            'Legacy libnostr_json.dylib is not used: it can depend on removed Homebrew ABIs.')
    else
      _json_legacy = cc.find_library('nostr_json', required : true, dirs : _sys_lib_dirs)
      warning('Using legacy nostr_json; upgrade nostrc to the canonical nostr-json library.')
      dep_nostr_json = declare_dependency(
        dependencies : [_json_legacy, dependency('jansson')],
      )
    endif
  endif
endif

# NIP-11 relay information document (requires libcurl for HTTP fetch)
dep_curl = dependency('libcurl', version : '>=7.80', required : true)
# nip17 — try installed library first, fallback to build-tree object
dep_nip17_lib = cc.find_library('nostr_nip17_core', required : false, dirs : _sys_lib_dirs)
if dep_nip17_lib.found()
  dep_nip17 = dep_nip17_lib
elif _nostrc_build != ''
  _nip17_obj = _nostrc_build / 'nips' / 'nip17' / 'CMakeFiles' / 'nostr_nip17_core.dir' / 'src' / 'nip17.c.o'
  dep_nip17 = declare_dependency(link_args : [_nip17_obj])
  message('nip17: using build-tree object: ' + _nip17_obj)
else
  error('nostr_nip17_core not found. Install nostrc or set -Dnostrc_root=<path>')
endif

dep_nip11_lib = cc.find_library('nip11', required : false, dirs : _sys_lib_dirs)
if dep_nip11_lib.found()
  dep_nip11 = dep_nip11_lib
elif _nostrc_build != ''
  _nip11_obj = _nostrc_build / 'nips' / 'nip11' / 'CMakeFiles' / 'nip11.dir' / 'src' / 'nip11.c.o'
  dep_nip11 = declare_dependency(link_args : [_nip11_obj])
  message('nip11: using build-tree object: ' + _nip11_obj)
else
  error('nip11 not found. Install nostrc or set -Dnostrc_root=<path>')
endif

# ── Platform HAL backend selection (spec §5.3) ──────────────────────────────
# Each platform provides: capture, a11y, and input backend source files + deps.

platform_hal_sources = []
platform_hal_deps = []

if host_os == 'linux'
  dep_pipewire  = dependency('libpipewire-0.3', version : '>=0.3.65')
  dep_atspi     = dependency('atspi-2',         version : '>=2.48')
  dep_udev      = dependency('libudev',         version : '>=252')
  dep_dbus      = dependency('dbus-1',          version : '>=1.14')
  # a11y_atspi.c calls g_object_unref/g_boxed_free directly; atspi-2.pc only
  # exposes gobject-2.0 privately, so link it explicitly.
  dep_gobject   = dependency('gobject-2.0')
  # uuid_generate/uuid_unparse_lower: libSystem on macOS, libuuid on Linux.
  dep_uuid      = dependency('uuid')

  platform_hal_sources += files(
    'src/core/capture_pipewire.c',
    'src/core/a11y_atspi.c',
    'src/core/input_uinput.c',
    'src/core/ipc_unix.c',
  )
  platform_hal_deps += [dep_pipewire, dep_atspi, dep_udev, dep_dbus,
                        dep_gobject, dep_uuid]

elif host_os == 'darwin'
  add_languages('objc', required : true)

  platform_hal_sources += files(
    'src/core/capture_screencapturekit.m',
    'src/core/a11y_axui.m',
    'src/core/input_cgevent.m',
    'src/core/ipc_unix.c',
  )
  # macOS uses system frameworks — link via add_project_link_arguments
  # ObjC files use ARC for correct memory management of SCK/Foundation objects
  add_project_arguments('-fobjc-arc', language : 'objc')

  add_project_link_arguments(
    '-framework', 'ScreenCaptureKit',
    '-framework', 'CoreGraphics',
    '-framework', 'AppKit',
    '-framework', 'CoreMedia',
    '-framework', 'CoreVideo',
    '-framework', 'ApplicationServices',
    language : ['c', 'cpp', 'objc'],
  )

elif host_os == 'windows'
  platform_hal_sources += files(
    'src/core/capture_dxgi.cpp',
    'src/core/a11y_uia.cpp',
    'src/core/input_sendinput.cpp',
    'src/core/ipc_win32.c',
  )
  # Windows system libraries
  cc = meson.get_compiler('c')
  platform_hal_deps += [
    cc.find_library('d3d11', required : true),
    cc.find_library('dxgi', required : true),
    cc.find_library('user32', required : true),
    cc.find_library('advapi32', required : true),
    cc.find_library('ole32', required : true),
    cc.find_library('oleaut32', required : true),
    cc.find_library('uiautomationcore', required : true),
  ]

else
  error('Unsupported platform: ' + host_os)
endif

# ── Optional wlroots virtual input (independent of signer discovery) ─────────
wayland_test_sources = []
wayland_test_deps = []
if get_option('wayland_input')
  if host_os != 'linux'
    error('wayland_input is supported only on Linux')
  endif
  dep_wayland_client = dependency('wayland-client', version : '>=1.20')
  dep_xkbcommon = dependency('xkbcommon', version : '>=1.0')
  wayland_scanner = find_program('wayland-scanner')
  foreach protocol : ['wlr-virtual-pointer-unstable-v1', 'virtual-keyboard-unstable-v1']
    xml = files('protocols' / (protocol + '.xml'))
    client_header = custom_target(protocol + '-client-header', input : xml,
      output : protocol + '-client-protocol.h',
      command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    protocol_code = custom_target(protocol + '-code', input : xml,
      output : protocol + '-protocol.c',
      command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
    server_header = custom_target(protocol + '-server-header', input : xml,
      output : protocol + '-server-protocol.h',
      command : [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
    platform_hal_sources += [client_header, protocol_code]
    wayland_test_sources += [server_header, protocol_code]
  endforeach
  platform_hal_sources += files('src/core/input_wayland.c')
  platform_hal_deps += [dep_wayland_client, dep_xkbcommon]
  wayland_test_sources += files('tests/input_wayland_server.c')
  wayland_test_deps += dependency('wayland-server', version : '>=1.20')
  add_project_arguments('-DMD_HAVE_WAYLAND_INPUT', language : ['c', 'cpp'])
endif

# ── Client-only deps ────────────────────────────────────────────────────────
if enable_client
  dep_sdl2 = dependency('sdl2', version : '>=2.28')
endif

# ── Core include path ───────────────────────────────────────────────────────
core_inc = include_directories('src/core')

# ── Core dependencies (assembled before signer blocks can append) ───────────
core_deps = [
  dep_avcodec,
  dep_avutil,
  dep_yuv,
  dep_nostr,
  dep_nip44,
  dep_nip51,
  dep_nip98,
  dep_nip19,
  dep_nip17,
  dep_nip11,
  dep_curl,
  dep_libgo,
  dep_secp256k1,
  dep_websockets,
  dep_ssl,
  dep_crypto,
  dep_nsync,
  dep_nostr_json,
  dep_cjson,
  dep_b2,
  dep_threads,
] + platform_hal_deps

# ── Signer backend deps + compile flags ─────────────────────────────────────
# Diagnose stale nostrc objects after a system libwebsockets upgrade, including
# the all-off configuration (test_nostr also creates websocket contexts).
# CMake's cache is evidence of the dependency version, not an object ABI probe.
if _nostrc_build != '' and fs.exists(_nostrc_build / 'CMakeCache.txt')
  _lws_current = dependency('libwebsockets', required : false)
  if _lws_current.found()
    _lws_ssl_headers = dependency('openssl', required : false)
    if meson.can_run_host_binaries() and _lws_ssl_headers.found()
      _lws_abi = cc.run('''
        #include <libwebsockets.h>
        #include <stdio.h>
        int main(void) {
          const char *runtime = lws_get_library_version();
          printf("headers=%s library=%s", LWS_LIBRARY_VERSION, runtime);
          int major, minor, patch;
          return sscanf(runtime, "%d.%d.%d", &major, &minor, &patch) != 3 ||
            major != LWS_LIBRARY_VERSION_MAJOR || minor != LWS_LIBRARY_VERSION_MINOR ||
            patch != LWS_LIBRARY_VERSION_PATCH;
        }
        ''', name : 'libwebsockets header/library version agreement',
        dependencies : [dep_websockets,
          _lws_current.partial_dependency(compile_args : true, includes : true),
          _lws_ssl_headers.partial_dependency(compile_args : true, includes : true)])
      if not _lws_abi.compiled() or _lws_abi.returncode() != 0
        warning('libwebsockets header/library check failed: ' + _lws_abi.stdout() +
          '. Align the headers and library before rebuilding nostrc.')
      endif
    endif
    foreach _line : fs.read(_nostrc_build / 'CMakeCache.txt').split('\n')
      if _line.startswith('LWS_VERSION:INTERNAL=')
        _lws_cached = _line.split('=')[1]
        if _lws_cached.split('-')[0] != _lws_current.version().split('-')[0]
          warning('nostrc was configured with libwebsockets ' + _lws_cached +
            ', but pkg-config now reports ' + _lws_current.version() +
            '. Reconfigure and rebuild nostrc against the current headers and library; ' +
            'stale objects can crash in lws_create_context. See docs/build-signers.md.')
        endif
      endif
    endforeach
  endif
endif

signer_c_args = []
_signer_components = {}
if enable_nip46
  _signer_components += {'nip46' : ['nostr_nip46_core'], 'nip04' : ['nip04']}
  core_deps += dependency('jansson', required : true)
  signer_c_args += '-DMD_SIGNER_ENABLE_NIP46'
endif
if enable_nip55l
  _signer_components += {
    'nip55l' : ['nostr_nip55l_core'],
    'nip04' : ['nip04'],
    'nip06' : ['nip06'],
    'nip49' : ['nostr_nip49_glib', 'nostr_nip49_core'],
  }
  core_deps += [dependency('gio-2.0', version : '>=2.76'),
                dependency('libsodium', required : true)]
  if host_os == 'darwin'
    core_deps += dependency('appleframeworks', modules : ['Security', 'CoreFoundation'])
  elif host_os == 'linux'
    core_deps += dependency('libsecret-1', required : true)
    _secret_lib = cc.find_library('gnostr-secret', dirs : _sys_lib_dirs, required : false)
    if not _secret_lib.found() and _nostrc_build != ''
      _secret_lib = cc.find_library('gnostr-secret',
        dirs : [_nostrc_build / 'gnome' / 'seahorse'], required : false)
    endif
    if not _secret_lib.found()
      error('NIP-55L needs gnostr-secret. Build the nostrc gnostr-secret target or install it.')
    endif
    core_deps += _secret_lib
  endif
  signer_c_args += '-DMD_SIGNER_ENABLE_NIP55L'
endif
if enable_nip5f
  _signer_components += {'nip5f' : ['nostr_nip5f_core']}
  signer_c_args += '-DMD_SIGNER_ENABLE_NIP5F'
endif

# nostrc does not ship pkg-config metadata for these adapters. Discover the
# installed archive/shared library first, then the CMake build-tree library.
# Unlike nip51/nip98, the adapters contain multiple objects: link the archive.
_signer_inc_paths = []
_signer_libraries = {}
foreach _component, _libraries : _signer_components
  if _nostrc_root != ''
    _signer_inc_paths += _nostrc_root / 'nips' / _component / 'include'
  endif
  if _nostrc_build != '' and fs.is_dir(_nostrc_build / 'nips' / _component / 'include')
    _signer_inc_paths += _nostrc_build / 'nips' / _component / 'include'
  endif
  # Some nostrc installs nest the public header tree below include/nostr/nipXX.
  foreach _prefix : _sys_inc_dirs + ['/usr/local/include', '/usr/include']
    if fs.is_dir(_prefix / 'nostr' / _component)
      _signer_inc_paths += _prefix / 'nostr' / _component
    endif
  endforeach
  foreach _library : _libraries
    _signer_lib = cc.find_library(_library, dirs : _sys_lib_dirs, required : false)
    if not _signer_lib.found() and _nostrc_build != ''
      _signer_lib = cc.find_library(_library,
        dirs : [_nostrc_build / 'nips' / _component], required : false)
    endif
    if not _signer_lib.found()
      error(_library + ' not found. Build the nostrc ' + _library +
            ' target and set -Dnostrc_build_dir=<path>, or install it.')
    endif
    _signer_libraries += {_library : _signer_lib}
  endforeach
endforeach
# Installed archives can lag behind the source headers. Check the entry points
# used by Metadesk with the full transitive link set before accepting them.
_signer_apis = {
  'nip04' : ['nip04', ['nostr_nip04_legacy_decrypt_enabled']],
  'nip46' : ['nostr_nip46_core', ['nostr_nip46_client_sign_event',
    'nostr_nip46_bunker_set_request_handler', 'nostr_nip46_bunker_set_transport_key',
    'nostr_nip46_bunker_request_complete']],
  'nip55l' : ['nostr_nip55l_core', ['nostr_nip55l_sign_event_json']],
  'nip5f' : ['nostr_nip5f_core', ['nostr_nip5f_client_sign_event', 'nostr_nip5f_server_start']],
}
foreach _component, _api : _signer_apis
  _library = _api[0]
  if _signer_libraries.has_key(_library)
    _other_signer_deps = []
    foreach _name, _dep : _signer_libraries
      if _name != _library
        _other_signer_deps += _dep
      endif
    endforeach
    _signer_lib = _signer_libraries[_library]
    foreach _function : _api[1]
      if not cc.has_function(_function, dependencies : [_signer_lib] + _other_signer_deps + core_deps)
        if _nostrc_build != ''
          _signer_lib = cc.find_library(_library,
            dirs : [_nostrc_build / 'nips' / _component], required : true)
        endif
        if not cc.has_function(_function, dependencies : [_signer_lib] + _other_signer_deps + core_deps)
          error(_library + ' cannot link ' + _function + '. Rebuild the nostrc target against current headers and dependencies.')
        endif
        message(_library + ': using compatible build-tree library')
        _signer_libraries += {_library : _signer_lib}
      endif
    endforeach
  endif
endforeach
foreach _name, _dep : _signer_libraries
  core_deps += _dep
endforeach
if enable_nip46 or enable_nip55l or enable_nip5f
  core_deps += declare_dependency(include_directories : include_directories(_signer_inc_paths))
endif
# Also consumed by the backend-specific integration tests below.
if enable_nip46
  dep_nip46 = _signer_libraries['nostr_nip46_core']
endif
if enable_nip55l
  dep_nip55l = _signer_libraries['nostr_nip55l_core']
endif
if enable_nip5f
  dep_nip5f = _signer_libraries['nostr_nip5f_core']
endif

# ── Vendored subprojects ────────────────────────────────────────────────────
if enable_client
  imgui_inc = include_directories('imgui_vendor')
  imgui_sources = files(
    'imgui_vendor/imgui.cpp',
    'imgui_vendor/imgui_demo.cpp',
    'imgui_vendor/imgui_draw.cpp',
    'imgui_vendor/imgui_tables.cpp',
    'imgui_vendor/imgui_widgets.cpp',
    'imgui_vendor/backends/imgui_impl_sdl2.cpp',
    'imgui_vendor/backends/imgui_impl_sdlrenderer2.cpp',
  )
  imgui_lib = static_library('imgui', imgui_sources,
    include_directories : imgui_inc,
    dependencies : [dep_sdl2],
    cpp_args : ['-DIMGUI_IMPL_API='],
  )
  dep_imgui = declare_dependency(
    link_with : imgui_lib,
    include_directories : imgui_inc,
  )
endif

# ── libmetadesk (shared core library) ───────────────────────────────────────
# Platform-agnostic sources (compiled on all platforms)
core_sources = files(
  'src/core/log.c',
  'src/core/io_util.c',
  'src/core/agent.c',
  'src/core/action.c',
  'src/core/fips_addr.c',
  'src/core/fips_control.c',
  'src/core/signer.c',
  'src/core/capture.c',
  'src/core/encode.c',
    'src/core/bitrate_ctrl.c',
    'src/core/decode.c',
  'src/core/a11y.c',
  'src/core/a11y_query.c',
  'src/core/a11y_snapshot.c',
  'src/core/input.c',
  'src/core/session.c',
  'src/core/jsonrpc.c',
  'src/core/mcp_server.c',
  'src/core/mcp_tools.c',
  'src/core/mcp_stdio.c',
  'src/core/mcp_resources.c',
  'src/core/mcp_bridge.c',
  'src/core/mcp_http.c',
  'src/core/nostr.c',
  'src/core/session_log.c',
  'src/core/packet.c',
  'src/core/stream.c',
  'src/core/secrets.c',
)

# ── Signer accounts + OS credential storage (independent of signer discovery) ──
core_sources += files(
  'src/core/accounts.c',
  'src/core/accounts_signer.c',
  'src/core/credential_store.c',
  'src/core/credential_store_secret_service.c',
  'src/core/credential_store_keychain.c',
)
accounts_c_args = []
if host_os == 'linux'
  core_deps += dependency('libsecret-1', version : '>=0.20')
  accounts_c_args += '-DMD_HAVE_LIBSECRET'
elif host_os == 'darwin'
  core_deps += dependency('appleframeworks', modules : ['Security', 'CoreFoundation'])
endif

# Append platform HAL backend sources
core_sources += platform_hal_sources

mcp_c_args = ['-DMD_MCP_BRIDGE_VERSION="' + meson.project_version() + '"']

libmetadesk = shared_library('metadesk', core_sources,
  include_directories : core_inc,
  dependencies : core_deps,
  c_args : signer_c_args + mcp_c_args + accounts_c_args,
  install : true,
  version : meson.project_version(),
)

dep_metadesk = declare_dependency(
  link_with : libmetadesk,
  include_directories : core_inc,
  dependencies : core_deps,
)

# ── Signer account tests ────────────────────────────────────────────────────
if host_os == 'linux' or host_os == 'darwin'
  test_accounts = executable('test_accounts', 'tests/test_accounts.c',
    dependencies : dep_metadesk, c_args : accounts_c_args)
  test('accounts', test_accounts, timeout : 30)
  accounts_python = import('python').find_installation('python3')
  if host_os == 'darwin'
    test('account_keychain', test_accounts, args : ['--keychain'], timeout : 30, is_parallel : false)
  else
    accounts_dbus = find_program('dbus-run-session', required : false)
    accounts_keyring = find_program('gnome-keyring-daemon', required : false)
    if accounts_dbus.found() and accounts_keyring.found()
      test('account_secret_service', accounts_python,
        args : [files('tests/test_account_keyring.py'), test_accounts], timeout : 45)
    else
      message('Private account Secret Service test unavailable: install dbus-run-session and gnome-keyring-daemon')
    endif
  endif
endif

# ── metadesk-host ───────────────────────────────────────────────────────────
host_sources = files('src/host/main.c', 'src/host/account_cli.c')
host_includes = [core_inc]
host_c_args = []

metadesk_host = executable('metadesk-host', host_sources,
  include_directories : host_includes,
  dependencies : [dep_metadesk],
  c_args : host_c_args,
  install : true,
)

if host_os == 'linux' or host_os == 'darwin'
  test('account_cli', accounts_python,
    args : [files('tests/test_account_cli.py'), metadesk_host], timeout : 30)
endif

# ── metadesk-client (human video client) ────────────────────────────────────
if enable_client
  client_sources = files(
    'src/client/main.c',
    'src/client/render.c',
    'src/client/ui.cpp',
  )
  metadesk_client = executable('metadesk-client', client_sources,
    include_directories : [core_inc],
    dependencies : [dep_metadesk, dep_sdl2, dep_imgui],
    install : true,
  )
endif

if host_os == 'linux' or host_os == 'darwin'
  test_account_startup = executable('test_account_startup',
    'tests/test_account_startup.c', dependencies : dep_metadesk,
    c_args : signer_c_args)
  startup_args = [files('tests/account_startup_fixture.py'), test_account_startup, metadesk_host]
  if enable_client
    startup_args += [metadesk_client]
  endif
  if enable_nip5f
    startup_args += ['--nip5f']
  endif
  test('account startup roles + private relay restore', accounts_python,
    args : startup_args, timeout : 120)
endif

# ── Tests ───────────────────────────────────────────────────────────────────
test_packet = executable('test_packet',
  files('tests/test_packet.c'),
  dependencies : [dep_metadesk],
)
test('packet round-trip', test_packet)

test_atspi = executable('test_atspi',
  files('tests/test_atspi.c'),
  dependencies : [dep_metadesk],
)
test('a11y tree serialisation', test_atspi)
if host_machine.system() == 'linux'
  test('a11y private bus loss', test_atspi,
    args : ['--private-bus-loss', metadesk_host.full_path()],
    depends : metadesk_host,
    timeout : 45,
  )
endif

# Live-backend checks hang/fail without an a11y bus or AX permission, so
# they are opt-in: configure with -Dlive_tests=true, run via
# `meson test --suite live`.
if get_option('live_tests')
  test('a11y live backend', test_atspi,
    env : {'MD_A11Y_LIVE_TESTS' : '1'},
    suite : 'live',
    # Cold AX/AT-SPI full-desktop walks take minutes on busy machines
    # (one IPC round-trip cluster per node) — not a hang.
    timeout : 300,
  )
endif

test_nostr = executable('test_nostr',
  files('tests/test_nostr.c'),
  dependencies : [dep_metadesk],
)
test('nostr NIP-44', test_nostr)
test('nostr ACL and AUTH private relays', find_program('python3'),
  args : [files('tests/nostr_acl_fixture.py'), test_nostr],
  env : {'PYTHONDONTWRITEBYTECODE' : '1'}, timeout : 120,
)

test_encode = executable('test_encode',
  files('tests/test_encode.c'),
  dependencies : [dep_metadesk],
)
test('encode/decode round-trip', test_encode)

test_stream = executable('test_stream',
  files('tests/test_stream.c'),
  dependencies : [dep_metadesk],
)
test('TCP stream transport', test_stream)

test_input = executable('test_input',
  files('tests/test_input.c') + wayland_test_sources,
  dependencies : [dep_metadesk] + wayland_test_deps,
)
test('input injection', test_input)

test_agent = executable('test_agent',
  files('tests/test_agent.c'),
  dependencies : [dep_metadesk],
)
test('agent action handler', test_agent)

test_mcp = executable('test_mcp',
  files('tests/test_mcp.c'),
  dependencies : [dep_metadesk],
)
test('MCP server core', test_mcp)
test('MCP accessibility queries', test_mcp, args : ['--a11y-queries'])

# ── Tools ────────────────────────────────────────────────────────────────────
# Diagnostic utilities (capture_frame, atspi_dump are useful for manual backend
# triage; encode_roundtrip overlaps tests/test_encode.c) — opt-in.
if get_option('tools')
capture_frame = executable('capture_frame',
  files('tools/capture_frame.c'),
  dependencies : [dep_metadesk],
  install : false,
)

encode_roundtrip = executable('encode_roundtrip',
  files('tools/encode_roundtrip.c'),
  dependencies : [dep_metadesk],
  install : false,
)

atspi_dump = executable('atspi_dump',
  files('tools/atspi_dump.c'),
  dependencies : [dep_metadesk],
  install : false,
)
endif

test_fips_addr = executable('test_fips_addr',
  files('tests/test_fips_addr.c'),
  dependencies : [dep_metadesk],
)
test('FIPS address derivation', test_fips_addr)

test_fips_control = executable('test_fips_control',
  files('tests/test_fips_control.c'),
  dependencies : [dep_metadesk, dependency('threads')],
)
test('FIPS control socket client seam', test_fips_control, timeout : 10)

test_secrets = executable('test_secrets',
  files('tests/test_secrets.c'),
  dependencies : [dep_metadesk, dep_threads],
)
test('1Password Connect secrets', test_secrets)

test_signer = executable('test_signer',
  files('tests/test_signer.c'),
  dependencies : [dep_metadesk],
)
test('signer abstraction', test_signer)

test_session = executable('test_session',
  files('tests/test_session.c'),
  dependencies : [dep_metadesk],
)
test('session JSON + state machine', test_session)

test_jsonrpc = executable('test_jsonrpc',
  files('tests/test_jsonrpc.c'),
  dependencies : [dep_metadesk],
)
test('JSON-RPC 2.0 message layer', test_jsonrpc)

test_mcp_stdio = executable('test_mcp_stdio',
  files('tests/test_mcp_stdio.c'),
  dependencies : [dep_metadesk],
)
test('MCP stdio transport', test_mcp_stdio, timeout : 10)

test_action = executable('test_action',
  files('tests/test_action.c'),
  dependencies : [dep_metadesk],
)
test('action parse/encode', test_action)

test_capture = executable('test_capture',
  files('tests/test_capture.c'),
  dependencies : [dep_metadesk],
)
test('capture convenience API', test_capture)

test_ipc = executable('test_ipc',
  files('tests/test_ipc.c'),
  dependencies : [dep_metadesk],
)
test('IPC Unix domain sockets', test_ipc, timeout : 15)

test_mcp_http = executable('test_mcp_http',
  files('tests/test_mcp_http.c'),
  dependencies : [dep_metadesk],
)
test('MCP HTTP+SSE transport', test_mcp_http, timeout : 15)

if enable_nip5f
  test_signer_nip5f = executable('test_signer_nip5f',
    files('tests/test_signer_nip5f.c'),
    dependencies : [dep_metadesk, dep_nip5f],
    c_args : ['-DMD_SIGNER_ENABLE_NIP5F'],
  )
  test('NIP-5F signer integration', test_signer_nip5f)
endif

if enable_nip55l
  dep_nip19_test = dep_nip19
  test_signer_nip55l = executable('test_signer_nip55l',
    files('tests/test_signer_nip55l.c'),
    dependencies : [dep_metadesk, dep_nip55l, dep_nip19_test],
    c_args : ['-DMD_SIGNER_ENABLE_NIP55L'],
  )
  test('NIP-55L signer integration', test_signer_nip55l)
endif

if enable_nip46
  dep_nip04_test = _signer_libraries['nip04']
  test_signer_nip46 = executable('test_signer_nip46',
    files('tests/test_signer_nip46.c'),
    dependencies : [dep_metadesk, dep_nip46, dep_nip04_test],
    c_args : ['-DMD_SIGNER_ENABLE_NIP46'],
  )
  test('NIP-46 signer integration', test_signer_nip46)
endif

test_bitrate_ctrl = executable('test_bitrate_ctrl',
  files('tests/test_bitrate_ctrl.c', 'src/core/bitrate_ctrl.c',
        'src/core/log.c'),
  include_directories : [core_inc],
)
test('bitrate controller AIMD', test_bitrate_ctrl)

test_session_log = executable('test_session_log',
  files('tests/test_session_log.c'),
  dependencies : [dep_metadesk],
)
test('signed session log', test_session_log)
