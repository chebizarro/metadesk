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

# Optional: libyuv has no pkg-config on most distros — try system, fallback to find_library
dep_yuv = dependency('libyuv', required : false)
if not dep_yuv.found()
  _yuv_lib = meson.get_compiler('c').find_library('yuv',
    dirs : ['/opt/homebrew/lib', '/usr/local/lib'],
    required : true)
  dep_yuv = declare_dependency(
    dependencies : _yuv_lib,
    include_directories : include_directories('/opt/homebrew/include'),
  )
endif

# ── nostrc: Nostr C library (installed from nostrc/_build) ──────────────────
cc = meson.get_compiler('c')
nostrc_inc = include_directories(
  '/opt/homebrew/include/nostr',
  '/Users/bizarro/Documents/Projects/nostrc/libgo/include',
)
dep_nostr_lib = cc.find_library('nostr', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_nostr = declare_dependency(
  dependencies : dep_nostr_lib,
  include_directories : nostrc_inc,
)
dep_nip44 = cc.find_library('nostr_nip44_core', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_nip51 = declare_dependency(
  link_args : ['/Users/bizarro/Documents/Projects/nostrc/build/nips/nip51/CMakeFiles/nostr_nip51_core.dir/src/nip51.c.o'],
)
dep_nip17 = cc.find_library('nostr', required : false,
  dirs : ['/opt/homebrew/lib'])

# nostrc transitive deps: libgo runtime, libwebsockets, openssl, secp256k1, nsync, nostr_json
dep_libgo = declare_dependency(
  link_args : [
    '/Users/bizarro/Documents/Projects/nostrc/build/libgo/liblibgo.a',
    '/Users/bizarro/Documents/Projects/nostrc/build/libgo/libgo_fiber.a',
  ],
)
dep_secp256k1 = cc.find_library('secp256k1', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_websockets = cc.find_library('websockets', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_ssl = cc.find_library('ssl', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_crypto = cc.find_library('crypto', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_nsync = cc.find_library('nsync', required : true,
  dirs : ['/opt/homebrew/lib'])
dep_nostr_json = cc.find_library('nostr_json', required : true,
  dirs : ['/opt/homebrew/lib'])

# ── Platform HAL backend selection (spec §5.3) ──────────────────────────────
# Each platform provides: capture, a11y, and input backend source files + deps.

platform_hal_sources = []
platform_hal_deps = []

if host_os == 'linux'
  dep_pipewire  = dependency('libpipewire-0.3', version : '>=0.3.65')
  dep_atspi     = dependency('atspi-2',         version : '>=2.48')
  dep_udev      = dependency('libudev',         version : '>=252')

  platform_hal_sources += files(
    'src/core/capture_pipewire.c',
    'src/core/a11y_atspi.c',
    'src/core/input_uinput.c',
    'src/core/ipc_unix.c',
  )
  platform_hal_deps += [dep_pipewire, dep_atspi, dep_udev]

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
    cc.find_library('ole32', required : true),
    cc.find_library('oleaut32', required : true),
  ]

else
  error('Unsupported platform: ' + host_os)
endif

# ── Client-only deps ────────────────────────────────────────────────────────
if enable_client
  dep_sdl2 = dependency('sdl2', version : '>=2.28')
endif

# ── fips-nat deps ───────────────────────────────────────────────────────────
if enable_fipsnat
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
  dep_libgo,
  dep_secp256k1,
  dep_websockets,
  dep_ssl,
  dep_crypto,
  dep_nsync,
  dep_nostr_json,
  dep_cjson,
  dep_b2,
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
  'src/core/signer.c',
  'src/core/capture.c',
  'src/core/encode.c',
  'src/core/decode.c',
  'src/core/a11y.c',
  'src/core/input.c',
  'src/core/session.c',
  'src/core/nostr.c',
  'src/core/packet.c',
  'src/core/stream.c',
  'src/core/secrets.c',
)

# Append platform HAL backend sources
core_sources += platform_hal_sources

libmetadesk = shared_library('metadesk', core_sources,
  include_directories : core_inc,
  dependencies : core_deps,
  c_args : signer_c_args,
  install : true,
  version : meson.project_version(),
)

dep_metadesk = declare_dependency(
  link_with : libmetadesk,
  include_directories : core_inc,
  dependencies : core_deps,
)

# ── metadesk-host ───────────────────────────────────────────────────────────
executable('metadesk-host',
  files('src/host/main.c'),
  dependencies : [dep_metadesk],
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

# ── fips-nat ────────────────────────────────────────────────────────────────
if enable_fipsnat
  fipsnat_sources = files(
    'src/fips-nat/main.c',
    'src/fips-nat/stun.c',
    'src/fips-nat/publish.c',
    'src/fips-nat/punch.c',
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

test_secrets = executable('test_secrets',
  files('tests/test_secrets.c'),
  dependencies : [dep_metadesk],
)
test('1Password Connect secrets', test_secrets)

test_signer = executable('test_signer',
  files('tests/test_signer.c'),
  dependencies : [dep_metadesk],
)
test('signer abstraction', test_signer)

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
