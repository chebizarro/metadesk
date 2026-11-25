foundation = dependency('appleframeworks', modules : ['Foundation'])
appkit = dependency('appleframeworks', modules : ['AppKit'])
notifications = dependency('appleframeworks', modules : ['Security', 'UserNotifications'])
qr_frameworks = dependency('appleframeworks', modules : ['CoreGraphics', 'CoreImage', 'ImageIO', 'Vision'])
uttypes = dependency('appleframeworks', modules : ['UniformTypeIdentifiers'])
macos_manager_model = static_library('macos-manager-model', 'model.m',
  link_with : libmetadesk,
  dependencies : [dep_metadesk.partial_dependency(compile_args : true, includes : true),
                  dep_cjson, dep_crypto, foundation])
macos_hosts_compile = dep_metadesk.partial_dependency(compile_args : true, includes : true)
macos_relay_store = static_library('macos-relay-store', 'relay_store.m',
  objc_args : '-fobjc-arc', dependencies : [macos_hosts_compile, foundation])
dep_macos_relay_store = declare_dependency(link_with : macos_relay_store, dependencies : foundation)
macos_hosts_model = static_library('macos-hosts-model', ['hosts_model.m', 'hosts_launch.m', 'access_model.m', 'identity_model.m'],
  objc_args : ['-fobjc-arc'] + signer_c_args, dependencies : [macos_hosts_compile, foundation, dep_crypto, dependency('jansson'), dep_macos_relay_store])
macos_host_qr = static_library('macos-host-qr', 'host_qr.m',
  objc_args : '-fobjc-arc', dependencies : [macos_hosts_compile, foundation, qr_frameworks])
dep_macos_host_qr = declare_dependency(link_with : macos_host_qr, dependencies : qr_frameworks)
macos_hosts_page = static_library('macos-hosts-page', ['hosts_page.m', 'access_page.m', 'identity_page.m'],
  objc_args : '-fobjc-arc', dependencies : [macos_hosts_compile, appkit, notifications, dep_cjson, dep_macos_host_qr, uttypes])
macos_manager_bin = executable('metadesk-macos-manager', 'main.m',
  objc_args : '-fobjc-arc',
  link_with : [macos_manager_model, macos_hosts_page, macos_hosts_model],
  dependencies : [dep_metadesk, appkit, dependency('jansson'), dep_macos_relay_store],
  build_rpath : meson.project_build_root(),
  install : true, install_rpath : _product_exe_rpath)

macos_manager_plist = configure_file(input : 'Info.plist.in', output : 'Info.plist',
  configuration : {'VERSION': meson.project_version()})
custom_target('macos-manager-app',
  input : [macos_manager_bin, macos_manager_plist],
  output : 'Metadesk Manager.app',
  command : [find_program('sh'), files('bundle.sh'), '@OUTPUT@', '@INPUT0@', '@INPUT1@'],
  build_by_default : true)

macos_manager_test = executable('test_macos_manager', 'test_model.m',
  link_with : macos_manager_model,
  dependencies : [dep_metadesk.partial_dependency(compile_args : true, includes : true), dep_cjson],
  include_directories : [core_inc, include_directories('../../tests')])
test('macos-manager model', macos_manager_test, suite : 'macos_manager')

macos_controller_test = executable('test_macos_managed_store_controller', '../../tests/test_managed_store_controller.c',
  dependencies : [dep_metadesk] + core_private_deps)
macos_hosts_python = find_program('python3')
macos_relay_fixture = executable('test_macos_relay_store', 'test_relay_store.m',
  objc_args : '-fobjc-arc', dependencies : [dep_metadesk, dep_macos_relay_store])
relay_vectors = files('../../tests/manager_relay_vectors.py')
test('native relay store vectors', macos_hosts_python,
  args : [relay_vectors, '--native', macos_relay_fixture], suite : 'macos_manager')
relay_test_gio = dependency('gio-2.0', version : '>=2.76', required : false)
if relay_test_gio.found()
  glib_relay_fixture = executable('test_glib_relay_store',
    '../../tests/manager_relay_glib.c', '../manager_shared/relay_store.c',
    include_directories : include_directories('../manager_shared'),
    dependencies : [dep_metadesk, relay_test_gio])
  test('GLib relay vectors and native round trip', macos_hosts_python,
    args : [relay_vectors, glib_relay_fixture, '--native', macos_relay_fixture], suite : 'macos_manager')
endif
test('macOS address book scoped relay controller', macos_hosts_python,
  args : [files('../../tests/managed_store_controller_fixture.py'), macos_controller_test],
  # Darwin's default TMPDIR can start with the /var symlink. The fixture uses
  # its path as a private accounts/store directory, which must be canonical.
  env : ['TMPDIR=' + meson.current_build_dir()], suite : 'macos_manager', timeout : 40)

macos_hosts_test = executable('test_macos_hosts_model', 'test_hosts_model.m',
  objc_args : '-fobjc-arc', link_with : macos_hosts_model,
  dependencies : [dep_metadesk, foundation, dep_macos_relay_store] + core_private_deps,
  include_directories : include_directories('../../tests'))
test('macOS Hosts model', macos_hosts_test, suite : 'macos_manager')
macos_qr_test = executable('test_macos_host_qr', 'test_host_qr.m',
  objc_args : '-fobjc-arc', link_with : macos_hosts_model,
  dependencies : [dep_metadesk, foundation, dep_macos_relay_store, dep_macos_host_qr],
  include_directories : include_directories('../../tests'))
test('macOS host QR file round trip', macos_hosts_python,
  args : [files('test_hosts_live.py'), '--qr', macos_qr_test], suite : 'macos_manager')
test('macOS shared bootstrap relay preferences', macos_hosts_python,
  args : [files('test_hosts_live.py'), '--bootstrap', macos_hosts_test], suite : 'macos_manager')
test('macOS Hosts workflow over private relay', macos_hosts_python,
  args : [files('test_hosts_live.py'), macos_hosts_test], suite : 'macos_manager', timeout : 65)
macos_hosts_launch_test = executable('test_macos_hosts_launch', 'test_hosts_launch.m',
  objc_args : '-fobjc-arc', link_with : macos_hosts_model, dependencies : [dep_metadesk, foundation, dep_macos_relay_store],
  include_directories : include_directories('../../tests'))
test('macOS Hosts launch worker', macos_hosts_python,
  args : [files('test_hosts_live.py'), '--launch', macos_hosts_launch_test], suite : 'macos_manager', timeout : 20)

macos_local_test = executable('test_macos_local_manager', 'test_local_manager.m',
  objc_args : '-fobjc-arc', link_with : macos_hosts_model,
  dependencies : [dep_metadesk, foundation, dep_macos_relay_store] + core_private_deps, include_directories : include_directories('../../tests'))
test('macOS local host decision model', macos_local_test, suite : 'macos_manager')
test('macOS management IPC over private Unix socket', macos_hosts_python,
  args : [files('test_local_manager.py'), '--ipc', macos_local_test], suite : 'macos_manager', timeout : 25)
test('macOS signer roles and fixture credentials', macos_hosts_python,
  args : [files('test_local_manager.py'), '--accounts', macos_local_test], suite : 'macos_manager', timeout : 25)

macos_page_test = executable('test_macos_manager_pages', ['test_pages.m', 'main.m'],
  objc_args : ['-fobjc-arc', '-DMD_MACOS_MANAGER_APP_TEST'],
  link_with : [macos_manager_model, macos_hosts_page, macos_hosts_model],
  dependencies : [dep_metadesk, appkit, dependency('jansson'), dep_macos_relay_store],
  include_directories : include_directories('../../tests'))
test('macOS AppKit controller actions and quit', macos_hosts_python,
  args : [files('test_local_manager.py'), '--pages', macos_page_test], suite : 'macos_manager', timeout : 25)
