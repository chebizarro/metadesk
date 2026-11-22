project('metadesk', ['c', 'cpp'],
  version : '0.1.0',
  license : 'MIT',
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

if host_os == 'windows'
  add_project_arguments('-DSDL_MAIN_HANDLED', language : ['c', 'cpp'])
endif

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
  _macos_deps_prefix = get_option('macos_deps_prefix')
  if _macos_deps_prefix != ''
    _sys_lib_dirs = [_macos_deps_prefix / 'lib']
    _sys_inc_dirs = [_macos_deps_prefix / 'include']
    message('Private macOS dependencies: ' + _macos_deps_prefix)
  else
    _brew = run_command('brew', '--prefix', check : false)
    _brew_prefix = _brew.returncode() == 0 ? _brew.stdout().strip() : '/opt/homebrew'
    _sys_lib_dirs = [_brew_prefix / 'lib']
    _sys_inc_dirs = [_brew_prefix / 'include']
    message('Homebrew prefix: ' + _brew_prefix)
  endif
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
# Every NIP library linked below needs its own source include dir:
# <nostr/nip51/nip51.h> must not fall through to an installed, older copy
# under the Homebrew prefix (md-r4ia: nostrc 5b3c987c8 changed nip51.h).
_nostrc_nip_dirs = ['nip11', 'nip17', 'nip19', 'nip44', 'nip51', 'nip98']
_nostrc_inc_paths = []
if _nostrc_root != ''
  foreach d : ['libnostr', 'libgo'] + _nostrc_nip_dirs
    _src_inc = _nostrc_root / (d.startswith('nip') ? 'nips' / d : d) / 'include'
    if fs.is_dir(_src_inc)
      _nostrc_inc_paths += _src_inc
    endif
  endforeach
endif
# nostrc generates export-macro and version headers (nostr-export.h,
# libgo-export.h, nostr-json-export.h, nipXX version headers) into the CMake
# build tree; the source headers include them unqualified.
if _nostrc_build != ''
  foreach d : ['libnostr', 'libgo', 'libjson'] + _nostrc_nip_dirs
    _gen_inc = _nostrc_build / (d.startswith('nip') ? 'nips' / d : d) / 'include'
    if fs.is_dir(_gen_inc)
      _nostrc_inc_paths += _gen_inc
    endif
  endforeach
endif
foreach d : _sys_inc_dirs
  if fs.is_dir(d / 'nostr')
    _nostrc_inc_paths += d / 'nostr'
  endif
endforeach
nostrc_inc = include_directories(_nostrc_inc_paths)

# libnostr: prefer the build-tree library (it matches the headers above).
# Archives are linked by path; shared libraries go through cc.find_library so
# Meson manages the build rpath. Probing file names first means a build tree
# without libnostr is never silently replaced by a system copy.
dep_nostr_lib = dependency('', required : false)
if _nostrc_build != ''
  foreach _file : ['libnostr.a', 'libnostr.so', 'libnostr.dylib']
    _path = _nostrc_build / 'libnostr' / _file
    if dep_nostr_lib.found()
    elif fs.exists(_path) and _file.endswith('.a')
      dep_nostr_lib = declare_dependency(link_args : [_path])
      message('nostr: using build-tree library ' + _path)
    elif fs.exists(_path)
      dep_nostr_lib = cc.find_library('nostr', dirs : [_nostrc_build / 'libnostr'],
                                      static : false, required : true)
      message('nostr: using build-tree library ' + _path)
    endif
  endforeach
endif
if not dep_nostr_lib.found()
  dep_nostr_lib = cc.find_library('nostr', required : true, dirs : _sys_lib_dirs)
endif
dep_nostr = declare_dependency(
  dependencies : dep_nostr_lib,
  include_directories : nostrc_inc,
)
# nostrc NIP libraries (md-r4ia). For each one, in order:
#   1. The library CMake built for the target, by file name in the target's
#      build directory: lib<target>.a (linked by path), .so/.dylib (through
#      cc.find_library, so Meson manages the build rpath), or <target>.lib.
#   2. For CMake OBJECT libraries, which never produce an archive (nip51 and
#      nip17), the objects that the CMake generator's own manifest lists for
#      the target: <subdir>/CMakeFiles/<target>.dir/build.make for Unix
#      Makefiles, or the top-level build.ninja for Ninja.
#   3. An installed library, only when absent from the selected build tree.
# This prevents installed, ABI-compatible old code from masking upstream fixes
# (in particular md-ebm's NIP-5F JSON unescaping).
# The manifest is rewritten whenever CMake regenerates the target, so it
# names exactly the target's current sources. For example, nip51 is one
# object in nostrc da2e4d62b and older, and three objects since 5b3c987c8. A
# glob of CMakeFiles/ would also link objects of renamed or deleted sources
# that older builds leave behind. fs.read() makes Meson reconfigure when the
# manifest changes. The link fails at configure time, naming the target, if
# a listed object has not been built.
_nostrc_components = {
  # component : [CMake target / library name, build-tree subdir]
  'nip44' : ['nostr_nip44_core', 'nips/nip44'],
  'nip51' : ['nostr_nip51_core', 'nips/nip51'],
  'nip98' : ['nostr_nip98', 'nips/nip98'],     # NIP-98 HTTP auth
  'nip19' : ['nip19', 'nips/nip19'],           # npub allowlist decoding
  'nip17' : ['nostr_nip17_core', 'nips/nip17'],
  'nip11' : ['nip11', 'nips/nip11'],
}
_nostrc_ninja_lines = []
_nostrc_ninja_read = false
_nostrc_deps = {}
foreach _component, _spec : _nostrc_components
  _target = _spec[0]
  _dir = _nostrc_build / _spec[1]
  _dep = dependency('', required : false)
  _how = 'installed library'
  if not _dep.found() and _nostrc_build != ''
    foreach _file : ['lib' + _target + '.a', _target + '.lib',
                     'lib' + _target + '.so', 'lib' + _target + '.dylib']
      if _dep.found() or not fs.exists(_dir / _file)
      elif _file.endswith('.a') or _file.endswith('.lib')
        _dep = declare_dependency(link_args : [_dir / _file])
        _how = 'build-tree library ' + (_dir / _file)
      else
        _dep = cc.find_library(_target, dirs : [_dir], static : false, required : true)
        _how = 'build-tree library ' + (_dir / _file)
      endif
    endforeach
  endif
  if not _dep.found() and _nostrc_build != ''
    _objs = []
    _manifest = _dir / 'CMakeFiles' / (_target + '.dir') / 'build.make'
    if fs.exists(_manifest)
      # Makefile rule lines: "<target>: <subdir>/CMakeFiles/<target>.dir/<src>.o"
      foreach _line : fs.read(_manifest).split('\n')
        _line = _line.strip()
        if _line.startswith(_target + ': ') and (_line.endswith('.o') or _line.endswith('.obj'))
          _objs += _nostrc_build / _line.split(_target + ': ')[1].strip()
        endif
      endforeach
    elif fs.exists(_nostrc_build / 'build.ninja')
      # Ninja edges: "build <subdir>/CMakeFiles/<target>.dir/<src>.o: <rule> ..."
      if not _nostrc_ninja_read
        _nostrc_ninja_lines = fs.read(_nostrc_build / 'build.ninja').split('\n')
        _nostrc_ninja_read = true
      endif
      _edge = 'build ' + _spec[1] + '/CMakeFiles/' + _target + '.dir/'
      foreach _line : _nostrc_ninja_lines
        if _line.startswith(_edge)
          _out = _line.substring(6).split(':')[0]
          if _out.endswith('.o') or _out.endswith('.obj')
            _objs += _nostrc_build / _out
          endif
        endif
      endforeach
    endif
    foreach _obj : _objs
      if not fs.exists(_obj)
        error('nostrc target ' + _target + ' is configured in ' + _nostrc_build +
              ' but not built (missing ' + _obj + '). Run: cmake --build ' +
              _nostrc_build + ' --target ' + _target)
      endif
    endforeach
    if _objs.length() > 0
      _dep = declare_dependency(link_args : _objs)
      _names = []
      foreach _obj : _objs
        _names += fs.name(_obj)
      endforeach
      _how = 'build-tree objects of CMake target ' + _target + ': ' + ', '.join(_names)
    endif
  endif
  if not _dep.found()
    _dep = cc.find_library(_target, required : false, dirs : _sys_lib_dirs)
  endif
  if not _dep.found()
    error(_target + ' not found. Install nostrc, or build the nostrc ' + _target +
          ' target and set -Dnostrc_build_dir=<path> (looked for an installed library, ' +
          'a library in ' + _dir + ', and a Makefile/Ninja object manifest).')
  endif
  message(_component + ': using ' + _how)
  _nostrc_deps += {_component : _dep}
endforeach
dep_nip44 = _nostrc_deps['nip44']
dep_nip51 = _nostrc_deps['nip51']
dep_nip98 = _nostrc_deps['nip98']
dep_nip19 = _nostrc_deps['nip19']
dep_nip17 = _nostrc_deps['nip17']
dep_nip11 = _nostrc_deps['nip11']

# nostrc transitive deps: libgo runtime, libwebsockets, openssl, secp256k1, nsync, nostr_json
# libgo — the selected build tree owns the runtime as well as libnostr.
_libgo_lib = cc.find_library('libgo', required : false, dirs : _sys_lib_dirs)
_go_fiber_lib = cc.find_library('go_fiber', required : false, dirs : _sys_lib_dirs)
if _nostrc_build == '' and _libgo_lib.found() and _go_fiber_lib.found()
  dep_libgo = declare_dependency(dependencies : [_libgo_lib, _go_fiber_lib])
elif _nostrc_build != ''
  # nostrc renamed the libgo target to `nostrgo` (libnostrgo); older build
  # trees still have liblibgo. nostrgo follows BUILD_SHARED_LIBS; go_fiber is
  # always static. Archives are linked by path, shared libraries through
  # cc.find_library (build rpath).
  _libgo_parts = []
  _libgo_names = []
  foreach _candidates : [['nostrgo', 'libgo'], ['go_fiber']]
    _part = dependency('', required : false)
    foreach _name : _candidates
      foreach _file : ['lib' + _name + '.a', 'lib' + _name + '.so', 'lib' + _name + '.dylib']
        _path = _nostrc_build / 'libgo' / _file
        if _part.found() or not fs.exists(_path)
        elif _file.endswith('.a')
          _part = declare_dependency(link_args : [_path])
          _libgo_names += _file
        else
          _part = cc.find_library(_name, dirs : [_nostrc_build / 'libgo'],
                                  static : false, required : true)
          _libgo_names += _file
        endif
      endforeach
    endforeach
    if not _part.found()
      error('libgo: ' + ' or '.join(_candidates) + ' not found in ' +
            (_nostrc_build / 'libgo') + '. Build the nostrc nostrgo and go_fiber targets.')
    endif
    _libgo_parts += _part
  endforeach
  dep_libgo = declare_dependency(dependencies : _libgo_parts)
  message('libgo: using build-tree libraries ' + ' + '.join(_libgo_names) +
          ' from ' + (_nostrc_build / 'libgo'))
else
  error('libgo/go_fiber not found. Install nostrc or set -Dnostrc_root=<path>')
endif

dep_secp256k1 = cc.find_library('secp256k1', required : true, dirs : _sys_lib_dirs)
dep_websockets = cc.find_library('websockets', required : true, dirs : _sys_lib_dirs)
dep_ssl = cc.find_library('ssl', required : true, dirs : _sys_lib_dirs)
dep_crypto = cc.find_library('crypto', required : true, dirs : _sys_lib_dirs)
dep_nsync = cc.find_library('nsync', required : true, dirs : _sys_lib_dirs)
if host_os == 'darwin'
  # md-5aua: Homebrew libnsync exports a clock_gettime shim that returns
  # gettimeofday() wall time for every clock id. ld64 binds each import to the
  # first linked dylib exporting it, so name libSystem before nsync; then
  # libmetadesk, and the static nostrc code inside it, get the real clocks
  # (check: nm -m build/libmetadesk.dylib | grep clock_gettime).
  # The driver's own trailing -lSystem then draws a duplicate-library warning.
  _nsync_link_args = ['-lSystem']
  if cc.has_link_argument('-Wl,-no_warn_duplicate_libraries')
    _nsync_link_args += '-Wl,-no_warn_duplicate_libraries'
  endif
  dep_nsync = declare_dependency(link_args : _nsync_link_args,
                                 dependencies : [dep_nsync])
endif
# Current nostrc names the artifact nostr-json and its pkg-config module
# libnostr-json. Prefer the selected build tree, just as for libnostr above;
# installed underscore-named dylibs may still require removed Homebrew ABIs.
_json_build_lib = ''
if _nostrc_build != ''
  # MinGW CMake builds nostr_json as a DLL even with BUILD_SHARED_LIBS=OFF.
  # Discover its import library in the selected tree, not an installed copy.
  foreach name : ['libnostr-json.dylib', 'libnostr-json.so', 'libnostr-json.a',
                  'libnostr-json.dll.a']
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

# ── Platform HAL backend selection (spec §5.3) ──────────────────────────────
# Each platform provides: capture, a11y, and input backend source files + deps.

platform_hal_sources = []
core_private_deps = []
platform_hal_deps = []

if host_os == 'linux'
  dep_pipewire  = dependency('libpipewire-0.3', version : '>=0.3.65')
  dep_atspi     = dependency('atspi-2',         version : '>=2.48')
  dep_udev      = dependency('libudev',         version : '>=252')
  dep_xcb_input = dependency('xcb-xinput', version : '>=1.15')
  dep_xcb = dependency('xcb', version : '>=1.15')
  dep_xau = dependency('xau')
  dep_ei = dependency('libei-1.0', version : '>=1.2', required : get_option('portal_input'))
  if dep_ei.found()
    add_project_arguments('-DMD_HAVE_PORTAL_INPUT', language : ['c', 'cpp'])
    platform_hal_sources += files('src/core/input_portal.c')
    platform_hal_deps += [dep_ei, dependency('xkbcommon', version : '>=1.0')]
  endif
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
                        dep_gobject, dep_uuid, dep_xcb_input, dep_xcb, dep_xau]

elif host_os == 'darwin'
  add_languages('objc', required : true)

  platform_hal_sources += files(
    'src/core/capture_screencapturekit.m',
    'src/core/a11y_axui.m',
    'src/core/input_cgevent.m',
    'src/core/ipc_unix.c',
  )
  # macOS HAL uses system frameworks, linked privately below.
  # ObjC files use ARC for correct memory management of SCK/Foundation objects
  add_project_arguments('-fobjc-arc', language : 'objc')

  # These frameworks implement the private HAL, not the public C interface.
  # Do not inject them into every consumer's link command: doing so changes
  # Darwin's symbol-provider selection and can bind clocks to libnsync.
  core_private_deps += dependency('appleframeworks', modules : [
    'ScreenCaptureKit', 'CoreGraphics', 'AppKit', 'CoreMedia', 'CoreVideo',
    'ApplicationServices',
  ])

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
    cc.find_library('ws2_32', required : true),
    cc.find_library('rpcrt4', required : true),
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

if host_os != 'windows'
  core_deps += cc.find_library('dl', required : false)
endif

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
# The adapters are regular (archive) CMake targets, not OBJECT libraries.
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
    _signer_lib = dependency('', required : false)
    if _nostrc_build != ''
      _signer_lib = cc.find_library(_library,
        dirs : [_nostrc_build / 'nips' / _component], required : false)
    endif
    if not _signer_lib.found()
      _signer_lib = cc.find_library(_library, dirs : _sys_lib_dirs, required : false)
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
# Compiler feature probes do not inherit b_sanitize automatically. They
# must link the sanitizer runtime when testing instrumented nostrc archives.
_signer_check_args = []
foreach _sanitizer : ['address', 'thread', 'undefined', 'memory', 'leak']
  if '@0@'.format(get_option('b_sanitize')).contains(_sanitizer)
    _signer_check_args += '-fsanitize=' + _sanitizer
  endif
endforeach
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
      if not cc.has_function(_function, args : _signer_check_args,
          dependencies : [_signer_lib] + _other_signer_deps + core_deps)
        if _nostrc_build != ''
          _signer_lib = cc.find_library(_library,
            dirs : [_nostrc_build / 'nips' / _component], required : true)
        endif
        if not cc.has_function(_function, args : _signer_check_args,
          dependencies : [_signer_lib] + _other_signer_deps + core_deps)
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
# nostrc headers are target include dirs, not only dependency include dirs.
# Meson emits a target's include_directories before any dependency's, so the
# headers that match the selected nostrc build come before the bare Homebrew
# prefix that the libyuv fallback (and pkg-config deps) add. Otherwise
# <nostr/nipXX/...> resolves to installed headers from an older nostrc.
core_inc = [core_inc, nostrc_inc]
if enable_nip46 or enable_nip55l or enable_nip5f
  core_inc += include_directories(_signer_inc_paths)
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

# Darwin dependencies may themselves use @rpath install names (notably libyuv
# and build-tree nostr-json). Meson removes build rpaths at install time.
# Keep selected dependency directories for an installed prefix, and make the
# product library discoverable from a relocated DESTDIR tree. Linux producers
# retain an empty install rpath unless native packaging explicitly opts in.
_darwin_lib_rpath = ''
_product_exe_rpath = ''
if host_os == 'darwin'
  _install_dep_dirs = _sys_lib_dirs
  _yuv_libdir = dep_yuv.get_variable(pkgconfig : 'libdir', default_value : '')
  if _yuv_libdir != ''
    _install_dep_dirs += _yuv_libdir
  endif
  if _nostrc_build != ''
    _install_dep_dirs += _nostrc_build / 'libjson'
  endif
  _darwin_lib_rpath = ':'.join(['@loader_path'] + _install_dep_dirs)
  _product_lib_relative = fs.relative_to(get_option('prefix') / get_option('libdir'),
                                         get_option('prefix') / get_option('bindir'))
  _product_exe_rpath = ':'.join(['@loader_path' / _product_lib_relative] + _install_dep_dirs)
elif host_os == 'linux' and get_option('linux_package_rpath')
  if get_option('libdir') != 'lib/metadesk'
    error('linux_package_rpath requires --libdir=lib/metadesk')
  endif
  _product_exe_rpath = '$ORIGIN/../lib/metadesk'
endif

# ── libmetadesk (shared core library) ───────────────────────────────────────
# Platform-agnostic sources (compiled on all platforms)
core_sources = files(
  'src/core/log.c',
  'src/core/io_util.c',
  'src/core/md_socket.c',
  'src/core/md_compat.c',
  'src/core/md_interrupt.c',
  'src/core/md_time.c',
  'src/core/agent.c',
  'src/core/action.c',
  'src/core/fips_addr.c',
  'src/core/fips_control.c',
  'src/core/signer.c',
  'src/core/capture.c',
  'src/core/encode.c',
  'src/core/encoder_chain.c',
  'src/core/encode_openh264.c',
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
  'src/core/credential_store_windows.c',
  'src/core/win_private.c',
)
accounts_c_args = []
if host_os == 'linux'
  core_deps += dependency('libsecret-1', version : '>=0.20')
  accounts_c_args += '-DMD_HAVE_LIBSECRET'
elif host_os == 'darwin'
  core_deps += dependency('appleframeworks', modules : ['Security', 'CoreFoundation'])
elif host_os == 'windows'
  core_deps += cc.find_library('crypt32', required : true)
endif

# Append platform HAL backend sources
core_sources += platform_hal_sources

mcp_c_args = ['-DMD_MCP_BRIDGE_VERSION="' + meson.project_version() + '"']

libmetadesk = shared_library('metadesk', core_sources,
  include_directories : core_inc,
  dependencies : core_deps + core_private_deps,
  c_args : signer_c_args + mcp_c_args + accounts_c_args,
  install : true,
  install_rpath : _darwin_lib_rpath,
  version : meson.project_version(),
)

dep_metadesk = declare_dependency(
  link_with : libmetadesk,
  include_directories : core_inc,
  dependencies : core_deps,
)

# GTK remains private to the opt-in Linux companion, never libmetadesk.
if get_option('manager')
  if host_os != 'linux'
    error('metadesk-manager is currently Linux-only')
  endif
  subdir('src/manager')
endif

# ── Signer account tests ────────────────────────────────────────────────────
if host_os == 'linux' or host_os == 'darwin' or host_os == 'windows'
  test_accounts = executable('test_accounts', 'tests/test_accounts.c',
    dependencies : dep_metadesk, c_args : accounts_c_args)
  test('accounts', test_accounts, timeout : 30)
  accounts_python = import('python').find_installation('python3')
  if host_os == 'darwin'
    test('account_keychain', test_accounts, args : ['--keychain'], timeout : 30, is_parallel : false)
  elif host_os == 'linux'
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

if host_os == 'windows'
  test_win_security = executable('test_win_security', 'tests/test_win_security.c', dependencies : dep_metadesk)
  test('Windows native private storage + DPAPI', test_win_security, timeout : 30)
endif

# ── metadesk-host ───────────────────────────────────────────────────────────
# Explicit OpenH264 fetcher only; encoder startup has no downloader.
dep_oh_fetch = [dep_curl, dependency('openssl'), cc.find_library('bz2')]
if host_os == 'windows'
  dep_oh_fetch += cc.find_library('advapi32')
endif
host_sources = files('src/core/encode_openh264_fetch.c', 'src/host/main.c', 'src/host/capture_cli.c', 'src/host/account_cli.c',
  'src/host/mcp_nostr_acl.c', 'src/host/mcp_proxy.c',
  'src/host/capture_sync.c')
host_includes = [core_inc]
host_c_args = []

# Headless host access administration (md-pss).
host_sources += files('src/host/access_cli.c')

if host_os == 'windows'
  host_sources += import('windows').compile_resources('tools/win/metadesk.rc',
    include_directories : include_directories('tools/win'),
    depend_files : files('tools/win/metadesk.manifest'))
endif

metadesk_host = executable('metadesk-host', host_sources,
  include_directories : host_includes,
  dependencies : [dep_metadesk] + dep_oh_fetch,
  c_args : host_c_args,
  install : true,
  install_rpath : _product_exe_rpath,
)

if host_os == 'linux' or host_os == 'darwin'
  test('account_cli', accounts_python,
    args : [files('tests/test_account_cli.py'), metadesk_host], timeout : 30)
endif

# Headless access CLI private relay/real signer contracts (md-pss).
if host_os == 'linux' or host_os == 'darwin'
  access_signer_fixture = executable('access_signer_fixture',
    'tests/access_signer_fixture.c', dependencies : [dep_metadesk])
  test('access_cli arguments', accounts_python,
    args : [files('tests/test_access_cli.py'), metadesk_host, access_signer_fixture, '--syntax-only'],
    timeout : 30)
  if get_option('signer_nip5f')
    test('access_cli private relay', accounts_python,
      args : [files('tests/test_access_cli.py'), metadesk_host, access_signer_fixture],
      timeout : 120)
    test('access migration private relay', accounts_python,
      args : [files('tests/test_access_cli.py'), metadesk_host, access_signer_fixture, '--migration-only'],
      timeout : 120)
    # md-ah9.14/md-ah9.3: real mcp-proxy <-> NIP-98 MCP HTTP + live host allowlist.
    mcp_proxy_host_fixture = executable('mcp_proxy_host_fixture',
      files('tests/mcp_proxy_host_fixture.c', 'src/host/mcp_nostr_acl.c'),
      include_directories : [core_inc, include_directories('src/host')],
      dependencies : [dep_metadesk])
    test('mcp-proxy NIP-98 e2e private relay', accounts_python,
      args : [files('tests/test_mcp_proxy.py'), metadesk_host, mcp_proxy_host_fixture,
              access_signer_fixture],
      timeout : 120)
  endif
endif

# Opt-in Linux full-process session fixture; never installed (md-pss).
if enable_nip46 and host_os == 'linux'
  access_bunker_fixture = executable('access_bunker_fixture',
    'tests/access_bunker_fixture.c', dependencies : [dep_metadesk])
endif

# ── metadesk-client (human video client) ────────────────────────────────────
if enable_client
  client_sources = files(
    'src/client/main.c',
    'src/client/rates.c',
    'src/client/render.c',
    'src/client/ui.cpp',
  )
  metadesk_client = executable('metadesk-client', client_sources,
    include_directories : [core_inc],
    dependencies : [dep_metadesk, dep_sdl2, dep_imgui],
    install : true,
    install_rpath : _product_exe_rpath,
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

# Renderer selection plan: probed drivers skipped, GLES2 then software (md-rnc).
if enable_client
  test_render_select = executable('test_render_select',
    files('tests/test_render_select.c', 'src/client/render.c'),
    include_directories : [core_inc, include_directories('src/client')],
    dependencies : [dep_metadesk, dep_sdl2])
  test('client render driver selection', test_render_select)
  test_video_geometry = executable('test_video_geometry', 'tests/test_video_geometry.c',
    include_directories : [core_inc, include_directories('src/client'), include_directories('tests')])
  test('client video fit and input geometry', test_video_geometry)
  if get_option('tools')
    executable('render_probe', files('tools/e2e/render-probe.c', 'src/client/render.c'),
      include_directories : [core_inc, include_directories('src/client'), include_directories('tests')],
      dependencies : [dep_metadesk, dep_sdl2])
  endif
  test_client_session = executable('test_client_session',
    files('tests/test_client_session.c', 'src/client/rates.c',
          'src/client/render.c', 'src/client/ui.cpp'),
    include_directories : [core_inc, include_directories('src/client')],
    dependencies : [dep_metadesk, dep_sdl2, dep_imgui])
  test('client session refusal', test_client_session)
endif

# Client process contracts without a TTY or display (md-rk4, md-33e).
if enable_client and (host_os == 'linux' or host_os == 'darwin')
  test('client startup: piped stdout + GUI display fail-closed', accounts_python,
    args : [files('tests/test_client_startup.py'), metadesk_client], timeout : 60)
endif

# Opt-in private-keyring + disposable-netns end-to-end access/session gate (md-pss).
if enable_nip46 and enable_client and host_os == 'linux'
  test('access session e2e', accounts_python,
    args : [files('tests/access_session_e2e.py'), metadesk_host, metadesk_client, access_bunker_fixture],
    timeout : 200)
endif

test_client_rates = executable('test_client_rates',
  files('tests/test_client_rates.c', 'src/client/rates.c'),
  include_directories : [core_inc, include_directories('src/client')],
)
test('client sliding-window rates', test_client_rates)

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
  test_atspi_hit = executable('test_atspi_hit', files('tests/test_atspi_hit.c'),
    dependencies : [dep_metadesk])
  test('AT-SPI native hit adapter', test_atspi_hit)
  test('a11y private bus loss', test_atspi,
    args : ['--private-bus-loss', metadesk_host.full_path()],
    env : {'G_DEBUG' : 'fatal-criticals'},
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
# TSan needs instrumented nostrc + nsync to see their synchronization.
# No race suppressions: see docs/testing-sanitizers.md (md-d9tj).
nostr_test_env = environment({'PYTHONDONTWRITEBYTECODE' : '1'})
if '@0@'.format(get_option('b_sanitize')).contains('thread')
  _nostrc_tsan = false
  if _nostrc_build != '' and fs.exists(_nostrc_build / 'CMakeCache.txt')
    foreach _line : fs.read(_nostrc_build / 'CMakeCache.txt').split('\n')
      if _line.startswith('CMAKE_C_FLAGS') and _line.contains('-fsanitize=thread')
        _nostrc_tsan = true
      endif
    endforeach
  endif
  if _nostrc_tsan
    nostr_test_env.prepend('TSAN_OPTIONS', 'halt_on_error=1', separator : ':')
  else
    warning('TSan: nostrc is not instrumented; missing nsync synchronization ' +
      'can produce false positives. Instrument nostrc and nsync as described ' +
      'in docs/testing-sanitizers.md before interpreting race reports.')
  endif
endif
test('nostr NIP-44', test_nostr, env : nostr_test_env)
test('nostr ACL and AUTH private relays', find_program('python3'),
  args : [files('tests/nostr_acl_fixture.py'), test_nostr],
  env : nostr_test_env, timeout : 120,
)

test_encode = executable('test_encode',
  files('tests/test_encode.c'),
  dependencies : [dep_metadesk],
)
test('encode/decode round-trip', test_encode)

test_encoder_chain = executable('test_encoder_chain',
  files('tests/test_encoder_chain.c', 'src/core/encoder_chain.c'),
  include_directories : core_inc)
test('encoder chain policy', test_encoder_chain)
fake_openh264 = shared_library('fake_openh264', 'tests/fake_openh264.c',
  include_directories : core_inc, install : false)
test_encoder_loader = executable('test_encoder_loader', 'tests/test_encoder_loader.c',
  dependencies : dep_metadesk)
test('OpenH264 rejects incompatible ABI', test_encoder_loader, args : [fake_openh264.full_path()])
foreach rejected : [['unvetted_build', '2,7,0,2611'], ['older_layout', '2,5,1,2501']]
  fake_rejected = shared_library('fake_openh264_' + rejected[0], 'tests/fake_openh264.c',
    include_directories : core_inc, install : false,
    c_args : ['-DMD_FAKE_OPENH264_VERSION=' + rejected[1]])
  test('OpenH264 rejects ' + rejected[0] + ' before create', test_encoder_loader,
    args : [fake_rejected.full_path()])
endforeach
test('OpenH264 rejects missing explicit library', test_encoder_loader)
if host_os != 'windows'
  test('FFmpeg Windows patch wiring (fixtures)', find_program('python3'),
    args : [files('tests/test_ffmpeg_packaging.py')], timeout : 30)
endif


# md-ezto/md-pvlj: encoder size + input bounds follow capture restarts.
test_capture_sync = executable('test_capture_sync',
  files('tests/test_capture_sync.c', 'src/host/capture_sync.c'),
  include_directories : [core_inc, include_directories('src/host')],
  dependencies : [dep_metadesk],
)
test('capture geometry sync', test_capture_sync, timeout : 60)

test_stream = executable('test_stream',
  files('tests/test_stream.c'),
  dependencies : [dep_metadesk],
)
test('TCP stream transport', test_stream)

# md-5aua: one monotonic clock; on macOS it must not be libnsync's wall time.
test_time = executable('test_time',
  files('tests/test_time.c'),
  dependencies : [dep_metadesk],
)
test('monotonic clock', test_time, timeout : 30)
if host_os == 'darwin'
  _clock_bins = [libmetadesk, metadesk_host, test_time]
  if enable_client
    _clock_bins += [metadesk_client]
  endif
  test('clock_gettime binds to libSystem', find_program('sh'),
    args : ['-c', '''
      nm -m "$1" | grep -q "_clock_gettime (from libSystem)" ||
        { echo "$1: clock_gettime is not imported from libSystem"; exit 1; }
      for f; do
        if nm -m "$f" | grep "_clock_gettime" | grep -q "from libnsync"; then
          echo "$f binds clock_gettime to libnsync's wall-time shim"; exit 1
        fi
      done''', 'sh'] + _clock_bins)
endif

test_input = executable('test_input',
  files('tests/test_input.c') + wayland_test_sources,
  dependencies : [dep_metadesk] + wayland_test_deps,
)
test('input injection', test_input)
# Compile the real selection logic against device-free backend factories.
# Exercise portal choice even on hosts without libei, never native injection.
test_input_selection = executable('test_input_selection',
  files('tests/test_input_selection.c', 'src/core/input.c', 'src/core/log.c'),
  include_directories : core_inc,
  c_args : ['-DMD_HAVE_PORTAL_INPUT'],
  dependencies : [dep_threads],
)
test('input backend selection', test_input_selection)

if host_os == 'linux'
  dep_input_x11 = dependency('x11')
  test_input_desktop = executable('test_input_desktop',
    'tests/test_input_desktop.c', dependencies : [dep_metadesk, dep_input_x11])
  test('input first-key desktop (opt-in)', test_input_desktop, timeout : 180)
  if dep_ei.found()
    dep_eis_test = dependency('libeis-1.0', version : '>=1.2', required : false)
    if dep_eis_test.found()
      test_input_portal = executable('test_input_portal', 'tests/test_input_portal.c',
        dependencies : [dep_metadesk, dep_eis_test, dep_dbus, dependency('xkbcommon')])
      test('input portal private bus + EIS', find_program('python3'),
        args : [files('tests/run_input_portal.py'), test_input_portal], timeout : 30)
    endif
  endif

endif


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

test_host_binding = executable('test_host_binding',
  files('tests/test_host_binding.c', 'src/core/encode_openh264_fetch.c', 'src/host/mcp_nostr_acl.c', 'src/host/mcp_proxy.c',
    'src/host/capture_sync.c', 'src/host/capture_cli.c'),
  dependencies : [dep_metadesk] + dep_oh_fetch)
test('host_binding', test_host_binding, timeout : 30)
if enable_nip5f and (host_os == 'linux' or host_os == 'darwin')
  test('NIP-5F host session quoted seal', accounts_python,
    args : [files('tests/test_nip5f_session.py'), test_host_binding, access_signer_fixture],
    timeout : 90)
endif
if enable_nip5f and enable_client and (host_os == 'linux' or host_os == 'darwin')
  test('private allowlist real host/client loopback', accounts_python,
    args : [files('tests/access_private_session.py'), metadesk_host, metadesk_client, access_signer_fixture],
    timeout : 90)
endif
test('session correlation retaining private relay', find_program('python3'),
  args : [files('tests/test_session_history.py'), test_host_binding],
  env : {'PYTHONDONTWRITEBYTECODE' : '1'},
  timeout : 180,
)
if host_os == 'linux' or host_os == 'darwin'
  test('capability grants private relay', accounts_python,
    args : [files('tests/test_capability_grants.py'), test_host_binding, access_signer_fixture],
    timeout : 180, is_parallel : false)
endif

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

stdio_test_source = host_os == 'windows' ? 'tests/test_mcp_stdio_win.c' : 'tests/test_mcp_stdio.c'
test_mcp_stdio = executable('test_mcp_stdio',
  files(stdio_test_source),
  dependencies : [dep_metadesk],
)
test('MCP stdio transport', test_mcp_stdio, timeout : 10)
if host_os == 'windows'
  # Compile the unchanged production I/O sources without unrelated media DLLs;
  # their asynchronous startup makes whole-product handle counts non-local.
  test_io_util_win = executable('test_io_util_win',
    files('tests/test_io_util_win.c', 'src/core/io_util.c',
      'src/core/md_socket.c', 'src/core/md_time.c'),
    include_directories : core_inc,
    dependencies : [dep_threads, cc.find_library('ws2_32')])
  test('Windows bounded I/O ownership', test_io_util_win, timeout : 15)
endif

test_action = executable('test_action',
  files('tests/test_action.c'),
  dependencies : [dep_metadesk],
)
test('action parse/encode', test_action)

test_capture = executable('test_capture',
  files('tests/test_capture.c'),
  dependencies : [dep_metadesk] + core_private_deps,
)
test('capture convenience API', test_capture)

# md-2p10: ScreenCaptureKit reports a stream the system stopped as broken.
if host_os == 'darwin'
  test_capture_sck = executable('test_capture_sck',
    files('tests/test_capture_sck.m'),
    dependencies : [dep_metadesk] + core_private_deps,
  )
  test('ScreenCaptureKit system stop', test_capture_sck, timeout : 30)
  test_axui_hit = executable('test_axui_hit', files('tests/test_axui_hit.m'),
    dependencies : [dep_metadesk] + core_private_deps)
  test('AX native hit adapter', test_axui_hit)
  test_agent_axui = executable('test_agent_axui',
    files('tests/test_agent_axui.m'), dependencies : [dep_metadesk] + core_private_deps)
  test('AX owned clicks and native video', test_agent_axui,
    timeout : 300, suite : 'live')
endif

test_ipc = executable('test_ipc',
  files('tests/test_ipc.c'),
  dependencies : [dep_metadesk],
)
test(host_os == 'windows' ? 'IPC Windows named pipes' : 'IPC Unix domain sockets', test_ipc, timeout : 15)

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

if host_os == 'darwin'
  test('macOS packaging minimum-version parser', import('python').find_installation('python3'),
    args : [files('packaging/macos/test-packaging.py')])
endif

if host_os == 'darwin'
  # Meson installs absolute product install names even with install_rpath.
  # Rewrite only our library/executables, leaving third-party IDs untouched.
  _installed_bins = ['metadesk-host']
  if enable_client
    _installed_bins += 'metadesk-client'
  endif
  meson.add_install_script(import('python').find_installation('python3'),
    files('packaging/macos/fix-install-names.py'),
    get_option('libdir'), get_option('bindir'), _installed_bins)
endif

# Offline fetch fixtures include the implementation to exercise private I/O
# without adding network overrides or unauthenticated install APIs to the CLI.
test_oh_fetch = executable('test_openh264_fetch', 'tests/test_openh264_fetch.c',
  include_directories : [core_inc] + (_sys_inc_dirs.length() > 0 ? [include_directories(_sys_inc_dirs[0])] : []),
  dependencies : dep_oh_fetch)
test('OpenH264 offline fetch', test_oh_fetch)

test('OpenH264 explicit fetch CLI offline', find_program('python3'),
  args : [files('tests/test_openh264_fetch_cli.py'), metadesk_host], timeout : 35)
