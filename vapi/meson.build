if spice_gtk_has_vala
  spice_glib_vapi = gnome.generate_vapi('spice-client-glib-2.0',
                      install : true,
                      packages : ['gio-2.0', 'gstreamer-1.0'],
                      sources : spice_client_glib_gir[0])

  if spice_gtk_has_gtk
    gnome.generate_vapi('spice-client-gtk-3.0',
                        install : true,
                        packages : ['gtk+-3.0', 'gstreamer-1.0', spice_glib_vapi],
                        gir_dirs : meson.build_root() / 'src',
                        vapi_dirs : meson.current_build_dir(),
                        sources : spice_client_gtk_gir[0])
  endif
endif
