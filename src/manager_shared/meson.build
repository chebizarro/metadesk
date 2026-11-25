manager_relay_gio = dependency('gio-2.0', version : '>=2.76')
manager_relay_inc = include_directories('.')
manager_relay_store = static_library('manager-relay-store', 'relay_store.c',
  include_directories : core_inc, dependencies : manager_relay_gio)
dep_manager_relay_store = declare_dependency(link_with : manager_relay_store,
  include_directories : manager_relay_inc, dependencies : manager_relay_gio)
