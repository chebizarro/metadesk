foundation = dependency('appleframeworks', modules : ['Foundation'])
appkit = dependency('appleframeworks', modules : ['AppKit'])
macos_manager_model = static_library('macos-manager-model', 'model.m',
  link_with : libmetadesk,
  dependencies : [dep_metadesk.partial_dependency(compile_args : true, includes : true),
                  dep_cjson, dep_crypto, foundation])
macos_manager_bin = executable('metadesk-macos-manager', 'main.m',
  link_with : macos_manager_model,
  dependencies : appkit,
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
