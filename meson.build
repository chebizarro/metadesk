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

# ── Options ──────────────────────────────────────────────────────────────────
cmake = import('cmake')
enable_nvenc   = get_option('nvenc')
enable_client  = get_option('client')
enable_fipsnat = get_option('fips_nat')
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
#
# Use -Dnostrc_root=<path> to specify the nostrc project root for uninstalled
# components. Use -Dnostrc_build_dir=<path> to override the build directory.
_nostrc_root = get_option('nostrc_root')
_nostrc_build = get_option('nostrc_build_dir')
if _nostrc_build == '' and _nostrc_root != ''
  _nostrc_build = _nostrc_root / 'build'
endif

# nostrc installed headers + libgo headers (if from source tree)
_nostrc_inc_paths = []
foreach d : _sys_inc_dirs
  _nostrc_inc_paths += d / 'nostr'
endforeach
if _nostrc_root != ''
  _nostrc_inc_paths += _nostrc_root / 'libgo' / 'include'
  _nostrc_inc_paths += _nostrc_root / 'nips' / 'nip11' / 'include'
endif
nostrc_inc = include_directories(_nostrc_inc_paths)

dep_nostr_lib = cc.find_library('nostr', required : true, dirs : _sys_lib_dirs)
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

dep_nip17 = cc.find_library('nostr', required : false, dirs : _sys_lib_dirs)

# nostrc transitive deps: libgo runtime, libwebsockets, openssl, secp256k1, nsync, nostr_json
# libgo — try installed library first, fallback to build-tree static libs
_libgo_lib = cc.find_library('libgo', required : false, dirs : _sys_lib_dirs)
_go_fiber_lib = cc.find_library('go_fiber', required : false, dirs : _sys_lib_dirs)
if _libgo_lib.found() and _go_fiber_lib.found()
  dep_libgo = declare_dependency(dependencies : [_libgo_lib, _go_fiber_lib])
elif _nostrc_build != ''
  dep_libgo = declare_dependency(
    link_args : [
      _nostrc_build / 'libgo' / 'liblibgo.a',
      _nostrc_build / 'libgo' / 'libgo_fiber.a',
    ],
  )
  message('libgo: using build-tree static libs from ' + _nostrc_build / 'libgo')
else
  error('libgo/go_fiber not found. Install nostrc or set -Dnostrc_root=<path>')
endif

dep_secp256k1 = cc.find_library('secp256k1', required : true, dirs : _sys_lib_dirs)
dep_websockets = cc.find_library('websockets', required : true, dirs : _sys_lib_dirs)
dep_ssl = cc.find_library('ssl', required : true, dirs : _sys_lib_dirs)
dep_crypto = cc.find_library('crypto', required : true, dirs : _sys_lib_dirs)
dep_nsync = cc.find_library('nsync', required : true, dirs : _sys_lib_dirs)
dep_nostr_json = cc.find_library('nostr_json', required : true, dirs : _sys_lib_dirs)

# NIP-11 relay information document (requires libcurl for HTTP fetch)
dep_curl = dependency('libcurl', version : '>=7.80', required : true)
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

  platform_hal_sources += files(
    'src/core/capture_pipewire.c',
    'src/core/a11y_atspi.c',
    'src/core/input_uinput.c',
    'src/core/ipc_unix.c',
  )
  platform_hal_deps += [dep_pipewire, dep_atspi, dep_udev, dep_dbus]

elif host_os == 'darwin'
  add_languages('objc', required : true)

  platform_hal_sources += files(
    'src/core/capture_screencapturekit.m',
    'src/core/a11y_axui.m',
    'src/core/input_cgevent.m',
    'src/core/encode_videotoolbox.m',
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
    '-framework', 'VideoToolbox',
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

# ── Client-only deps ────────────────────────────────────────────────────────
if enable_client
  dep_sdl2 = dependency('sdl2', version : '>=2.28')
endif

# ── legacy fips-nat deps ────────────────────────────────────────────────────
# fips-nat is deprecated; FIPS daemon discovery/traversal is the supported path.
# Keep this opt-in build target until the replacement path is verified in-tree.
if enable_fipsnat
  message('Building deprecated legacy fips-nat target; prefer the FIPS daemon control path')
  dep_nice = dependency('nice', version : '>=0.1.21')
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
signer_c_args = []
if enable_nip46
  dep_nip46 = dependency('nostr-nip46', required : false)
  if not dep_nip46.found()
    dep_nip46 = meson.get_compiler('c').find_library('nostr-nip46', required : true)
  endif
  core_deps += dep_nip46
  signer_c_args += '-DMD_SIGNER_ENABLE_NIP46'
endif
if enable_nip55l
  dep_nip55l = dependency('nostr-nip55l', required : false)
  if not dep_nip55l.found()
    dep_nip55l = meson.get_compiler('c').find_library('nostr-nip55l', required : true)
  endif
  dep_gio = dependency('gio-2.0', version : '>=2.76')
  core_deps += [dep_nip55l, dep_gio]
  signer_c_args += '-DMD_SIGNER_ENABLE_NIP55L'
endif
if enable_nip5f
  dep_nip5f = dependency('nostr-nip5f', required : false)
  if not dep_nip5f.found()
    dep_nip5f = meson.get_compiler('c').find_library('nostr-nip5f', required : true)
  endif
  core_deps += dep_nip5f
  signer_c_args += '-DMD_SIGNER_ENABLE_NIP5F'
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

# Append platform HAL backend sources
core_sources += platform_hal_sources

mcp_c_args = ['-DMD_MCP_BRIDGE_VERSION="' + meson.project_version() + '"']

libmetadesk = shared_library('metadesk', core_sources,
  include_directories : core_inc,
  dependencies : core_deps,
  c_args : signer_c_args + mcp_c_args,
  install : true,
  version : meson.project_version(),
)

dep_metadesk = declare_dependency(
  link_with : libmetadesk,
  include_directories : core_inc,
  dependencies : core_deps,
)

# ── metadesk-host ───────────────────────────────────────────────────────────
host_sources = files('src/host/main.c')
host_includes = [core_inc]
host_c_args = []

if enable_fipsnat
  host_sources += files('src/fips-nat/fipsnat_ipc.c', 'src/fips-nat/stun.c')
  host_includes += include_directories('src/fips-nat')
  host_c_args += ['-DMD_ENABLE_FIPSNAT']
endif

executable('metadesk-host', host_sources,
  include_directories : host_includes,
  dependencies : [dep_metadesk],
  c_args : host_c_args,
  install : true,
)

# ── metadesk-client (human video client) ────────────────────────────────────
if enable_client
  client_sources = files(
    'src/client/main.c',
    'src/client/render.c',
    'src/client/ui.cpp',
  )
  executable('metadesk-client', client_sources,
    include_directories : [core_inc],
    dependencies : [dep_metadesk, dep_sdl2, dep_imgui],
    install : true,
  )
endif

# ── legacy fips-nat (deprecated, opt-in) ─────────────────────────────────────
if enable_fipsnat
  fipsnat_sources = files(
    'src/fips-nat/main.c',
    'src/fips-nat/stun.c',
    'src/fips-nat/publish.c',
    'src/fips-nat/punch.c',
    'src/fips-nat/turn.c',
    'src/fips-nat/fipsnat_ipc.c',
  )
  executable('fips-nat', fipsnat_sources,
    include_directories : [core_inc],
    dependencies : [dep_metadesk, dep_nice],
    install : true,
  )
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

test_nostr = executable('test_nostr',
  files('tests/test_nostr.c'),
  dependencies : [dep_metadesk],
)
test('nostr NIP-44', test_nostr)

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
  files('tests/test_input.c'),
  dependencies : [dep_metadesk],
)
test('input injection', test_input)

test_agent = executable('test_agent',
  files('tests/test_agent.c'),
  dependencies : [dep_metadesk],
)
test('agent action handler', test_agent)

# ── Tools ────────────────────────────────────────────────────────────────────
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

test_mcp = executable('test_mcp',
  files('tests/test_mcp.c'),
  dependencies : [dep_metadesk],
)
test('MCP server core', test_mcp)

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
  dep_nip19_test = dependency('nostr-nip19', required : false)
  if not dep_nip19_test.found()
    dep_nip19_test = meson.get_compiler('c').find_library('nip19', required : true)
  endif
  test_signer_nip55l = executable('test_signer_nip55l',
    files('tests/test_signer_nip55l.c'),
    dependencies : [dep_metadesk, dep_nip55l, dep_nip19_test],
    c_args : ['-DMD_SIGNER_ENABLE_NIP55L'],
  )
  test('NIP-55L signer integration', test_signer_nip55l)
endif

if enable_nip46
  dep_nip04_test = dependency('nostr-nip04', required : false)
  if not dep_nip04_test.found()
    dep_nip04_test = meson.get_compiler('c').find_library('nip04', required : true)
  endif
  test_signer_nip46 = executable('test_signer_nip46',
    files('tests/test_signer_nip46.c'),
    dependencies : [dep_metadesk, dep_nip46, dep_nip04_test],
    c_args : ['-DMD_SIGNER_ENABLE_NIP46'],
  )
  test('NIP-46 signer integration', test_signer_nip46)
endif

# ── STUN tests (no libnice needed — raw RFC 5389) ────────────────────────────
test_stun = executable('test_stun',
  files('tests/test_stun.c', 'src/fips-nat/stun.c'),
  include_directories : [include_directories('src/fips-nat')],
)
test('STUN binding discovery', test_stun)

test_punch = executable('test_punch',
  files('tests/test_punch.c', 'src/fips-nat/punch.c', 'src/fips-nat/stun.c'),
  include_directories : [include_directories('src/fips-nat')],
)
test('UDP hole punch', test_punch, timeout : 10)

test_publish = executable('test_publish',
  files('tests/test_publish.c', 'src/fips-nat/publish.c', 'src/fips-nat/stun.c'),
  include_directories : [core_inc, include_directories('src/fips-nat')],
  dependencies : [dep_metadesk],
)
test('NAT endpoint publication', test_publish)

test_fipsnat_ipc = executable('test_fipsnat_ipc',
  files('tests/test_fipsnat_ipc.c', 'src/fips-nat/fipsnat_ipc.c',
        'src/fips-nat/stun.c'),
  include_directories : [core_inc, include_directories('src/fips-nat')],
  dependencies : [dep_metadesk],
)
test('fips-nat IPC protocol', test_fipsnat_ipc)

test_turn = executable('test_turn',
  files('tests/test_turn.c', 'src/fips-nat/turn.c', 'src/fips-nat/stun.c'),
  include_directories : [include_directories('src/fips-nat')],
)
test('TURN relay client', test_turn)

test_bitrate_ctrl = executable('test_bitrate_ctrl',
  files('tests/test_bitrate_ctrl.c', 'src/core/bitrate_ctrl.c'),
  include_directories : [core_inc],
)
test('bitrate controller AIMD', test_bitrate_ctrl)

test_session_log = executable('test_session_log',
  files('tests/test_session_log.c'),
  dependencies : [dep_metadesk],
)
test('signed session log', test_session_log)
