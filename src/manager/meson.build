dep_manager_gio = dependency('gio-2.0', version : '>=2.76')
dep_manager_gtk = dependency('gtk4', version : '>=4.14')
dep_manager_adw = dependency('libadwaita-1', version : '>=1.5')
manager_inc = include_directories('.')
# Consume the shared core API, not its private raw nostrc OBJECT link inputs.
dep_manager_core = declare_dependency(link_with : libmetadesk,
  include_directories : core_inc)
manager_model = static_library('metadesk-manager-model', ['model.c', 'signer-status.c', 'host-client.c'],
  dependencies : [dep_manager_core, dep_manager_gio, dep_nip19, dep_cjson])
dep_manager_model = declare_dependency(link_with : manager_model,
  include_directories : manager_inc,
  dependencies : [dep_manager_core, dep_manager_gio, dep_nip19, dep_cjson])
gnome = import('gnome')
manager_resources = gnome.compile_resources('manager-resources', 'manager.gresource.xml',
  source_dir : meson.current_source_dir(), c_name : 'md_manager')
manager_ui = static_library('metadesk-manager-ui', ['window.c', 'access-page.c', manager_resources],
  dependencies : [dep_manager_model, dep_manager_gtk, dep_manager_adw])
dep_manager_ui = declare_dependency(link_whole : manager_ui,
  dependencies : [dep_manager_model, dep_manager_gtk, dep_manager_adw])
executable('metadesk-manager', 'main.c', dependencies : dep_manager_ui, install : true)
install_data('org.metadesk.Manager.desktop',
  install_dir : get_option('datadir') / 'applications')
manager_test = executable('test_manager_model', '../../tests/test_manager_model.c',
  dependencies : dep_manager_model)
test('manager-model', manager_test, suite : 'manager', timeout : 30)
manager_smoke = executable('test_manager_ui', '../../tests/test_manager_ui.c',
  dependencies : dep_manager_ui)
manager_dbus = find_program('dbus-run-session', required : false)
manager_xvfb = find_program('xvfb-run', required : false)
if manager_dbus.found() and manager_xvfb.found()
  manager_python = import('python').find_installation('python3')
  test('manager-ui', manager_python,
    args : [files('../../tests/run_manager_ui.py'), manager_dbus.full_path(),
            manager_xvfb.full_path(), manager_smoke], suite : 'manager', timeout : 45)
else
  message('manager-ui smoke test unavailable: install dbus-run-session and xvfb-run')
endif
manager_host_test = executable('test_manager_host', '../../tests/test_manager_host.c',
  dependencies : dep_manager_model)
test('manager-host-ipc', manager_host_test, suite : 'manager', timeout : 30)
manager_access_test = executable('test_manager_access', '../../tests/test_manager_access.c',
  dependencies : dep_manager_ui)
if manager_dbus.found() and manager_xvfb.found()
  test('manager-access-ui', manager_python,
    args : [files('../../tests/run_manager_ui.py'), manager_dbus.full_path(),
            manager_xvfb.full_path(), manager_access_test], suite : 'manager', timeout : 45)
endif
