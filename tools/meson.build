spice_cmdline_sources = [
  'spice-cmdline.c',
  'spice-cmdline.h',
]

#
# spicy-stats and spicy-screenshot
#
foreach exe : ['spicy-stats', 'spicy-screenshot']
  executable(exe,
             sources : spice_cmdline_sources + ['@0@.c'.format(exe)],
             c_args : '-Wno-deprecated-declarations',
             install : true,
             dependencies : spice_client_glib_dep)
endforeach

#
# spicy
#
if spice_gtk_has_gtk
  spicy_sources = [
    'spicy.c',
    'spicy-connect.c',
    'spicy-connect.h',
  ]

  executable('spicy',
             sources : spicy_sources + spice_cmdline_sources,
             c_args : '-Wno-deprecated-declarations',
             install : true,
             dependencies : spice_client_gtk_dep)
endif
