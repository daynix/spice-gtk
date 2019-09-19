tests_sources = [
    'util.c',
    'coroutine.c',
    'session.c',
    'uri.c',
    'file-transfer.c',
]

if spice_gtk_has_phodav
  tests_sources += 'pipe.c'
endif

if spice_gtk_has_usbredir
  tests_sources += 'cd-emu.c'
endif

if spice_gtk_has_polkit
  tests_sources += [
    'usb-acl-helper.c',
    'mock-acl-helper.c',
  ]
endif

# create a static library from a shared one extracting all objects
# this allows to rewrite part of it if necessary for mocking
test_lib = \
  static_library('test-lib',
                 objects : spice_client_glib_lib.extract_all_objects())

foreach src : tests_sources
  name = 'test-@0@'.format(src).split('.')[0]
  exe = executable(name,
                   sources : src,
                   c_args : '-DTESTDIR="@0@"'.format(meson.current_build_dir()),
                   link_with : test_lib,
                   dependencies : spice_client_glib_dep)
  if not name.contains('mock-acl-helper')
    test(name, exe)
  endif
endforeach
