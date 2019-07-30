pod2man = find_program('pod2man')

if pod2man.found()
  custom_target('spice-client.1',
            output : 'spice-client.1',
            input : 'spice-client.pod',
            install : true,
            install_dir : spice_gtk_datadir / 'man' / 'man1',
            build_by_default : true,
            command : [pod2man, '-c', 'Spice-GTK Documentation', '@INPUT@', '@OUTPUT@'])
endif
