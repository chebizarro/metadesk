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

# ── Options ──────────────────────────────────────────────────────────────────
cmake = import('cmake')
enable_nvenc   = get_option('nvenc')
enable_client  = get_option('client')
enable_fipsnat = get_option('fips_nat')

# ── System dependencies (pkg-config) ────────────────────────────────────────
dep_pipewire  = dependency('libpipewire-0.3', version : '>=0.3.65')
dep_avcodec   = dependency('libavcodec',      version : '>=60.0.0', required : true)
dep_avutil    = dependency('libavutil',        version : '>=58.0.0', required : true)
dep_atspi     = dependency('atspi-2',         version : '>=2.48')
dep_udev      = dependency('libudev',         version : '>=252')
dep_cjson     = dependency('libcjson',        version : '>=1.7.17')
dep_b2        = dependency('libb2',           version : '>=0.98')

# ── nostrc: Nostr C library (github.com/chebizarro/nostrc) ──────────────────
# Provides: libnostr (events, relays, keys), nip44, nip51, nip17.
# nostrc links libwebsockets, libsecp256k1, and libsodium internally.
# Look for installed pkg-config first, fall back to CMake subproject.
dep_nostr = dependency('nostr', required : false)
if not dep_nostr.found()
  # Try cmake subproject
  nostrc_proj = cmake.subproject('nostrc', required : true)
  dep_nostr = nostrc_proj.dependency('nostr')
endif
# NIP modules — try pkg-config, fall back to find_library
dep_nip44 = dependency('nostr-nip44', required : false)
if not dep_nip44.found()
  dep_nip44 = meson.get_compiler('c').find_library('nostr-nip44', required : true)
endif
dep_nip51 = dependency('nostr-nip51', required : false)
if not dep_nip51.found()
  dep_nip51 = meson.get_compiler('c').find_library('nostr-nip51', required : true)
endif
dep_nip17 = dependency('nostr-nip17', required : false)
if not dep_nip17.found()
  dep_nip17 = meson.get_compiler('c').find_library('nostr-nip17', required : true)
endif

# Optional: libyuv has no pkg-config on most distros — try system, fallback to find_library
dep_yuv = dependency('libyuv', required : false)
if not dep_yuv.found()
  dep_yuv = meson.get_compiler('c').find_library('yuv', required : true)
endif

# Client-only deps
if enable_client
  dep_sdl2 = dependency('sdl2', version : '>=2.28')
endif

# fips-nat deps
if enable_fipsnat
  dep_nice = dependency('nice', version : '>=0.1.21')
endif

# ── Vendored subprojects ────────────────────────────────────────────────────
# Dear ImGui — vendored header-only, built as a static convenience library
if enable_client
  imgui_inc = include_directories('subprojects/imgui')
  imgui_sources = files(
    'subprojects/imgui/imgui.cpp',
    'subprojects/imgui/imgui_demo.cpp',
    'subprojects/imgui/imgui_draw.cpp',
    'subprojects/imgui/imgui_tables.cpp',
    'subprojects/imgui/imgui_widgets.cpp',
    'subprojects/imgui/backends/imgui_impl_sdl2.cpp',
    'subprojects/imgui/backends/imgui_impl_sdlrenderer2.cpp',
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

# ── Core include path ───────────────────────────────────────────────────────
core_inc = include_directories('src/core')

# ── libmetadesk (shared core library) ───────────────────────────────────────
core_sources = files(
  'src/core/action.c',
  'src/core/capture.c',
  'src/core/encode.c',
  'src/core/decode.c',
  'src/core/atspi.c',
  'src/core/input.c',
  'src/core/session.c',
  'src/core/nostr.c',
  'src/core/packet.c',
  'src/core/secrets.c',
)

core_deps = [
  dep_pipewire,
  dep_avcodec,
  dep_avutil,
  dep_yuv,
  dep_nostr,
  dep_nip44,
  dep_nip51,
  dep_nip17,
  dep_atspi,
  dep_udev,
  dep_cjson,
  dep_b2,
]

libmetadesk = shared_library('metadesk', core_sources,
  include_directories : core_inc,
  dependencies : core_deps,
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
test('atspi tree serialisation', test_atspi)

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
