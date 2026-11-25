foundation = dependency('appleframeworks', modules : ['Foundation'])
appkit = dependency('appleframeworks', modules : ['AppKit'])
macos_manager_model = static_library('macos-manager-model', 'model.m',
  link_with : libmetadesk,
  dependencies : [dep_metadesk.partial_dependency(compile_args : true, includes : true),
                  dep_cjson, dep_crypto, foundation])
macos_hosts_compile = dep_metadesk.partial_dependency(compile_args : true, includes : true)
macos_hosts_model = static_library('macos-hosts-model', ['hosts_model.m', 'hosts_launch.m'],
  objc_args : '-fobjc-arc', dependencies : [macos_hosts_compile, foundation, dep_crypto, dependency('jansson')])
macos_hosts_page = static_library('macos-hosts-page', 'hosts_page.m',
  objc_args : '-fobjc-arc', dependencies : [macos_hosts_compile, appkit])
macos_manager_bin = executable('metadesk-macos-manager', 'main.m',
  link_with : [macos_manager_model, macos_hosts_page, macos_hosts_model],
  dependencies : [dep_metadesk, appkit, dependency('jansson')],
  build_rpath : meson.project_build_root())

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
test('macOS address book scoped relay controller', macos_hosts_python,
  args : [files('../../tests/managed_store_controller_fixture.py'), macos_controller_test],
  # Darwin's default TMPDIR can start with the /var symlink. The fixture uses
  # its path as a private accounts/store directory, which must be canonical.
  env : ['TMPDIR=' + meson.current_build_dir()], suite : 'macos_manager', timeout : 40)

macos_hosts_test = executable('test_macos_hosts_model', 'test_hosts_model.m',
  objc_args : '-fobjc-arc', link_with : macos_hosts_model,
  dependencies : [dep_metadesk, foundation] + core_private_deps,
  include_directories : include_directories('../../tests'))
test('macOS Hosts model', macos_hosts_test, suite : 'macos_manager')
test('macOS Hosts workflow over private relay', macos_hosts_python,
  args : [files('test_hosts_live.py'), macos_hosts_test], suite : 'macos_manager', timeout : 65)
macos_hosts_launch_test = executable('test_macos_hosts_launch', 'test_hosts_launch.m',
  objc_args : '-fobjc-arc', link_with : macos_hosts_model, dependencies : [dep_metadesk, foundation],
  include_directories : include_directories('../../tests'))
test('macOS Hosts launch worker', macos_hosts_python,
  args : [files('test_hosts_live.py'), '--launch', macos_hosts_launch_test], suite : 'macos_manager', timeout : 20)
