#
# project definition
#
project('spice-gtk', 'c',
         version : '0.36',
         license : 'LGPLv2.1',
         meson_version : '>= 0.49')

message('Updating submodules')
run_command('build-aux/meson/check-spice-common', check : true)

#
# global C defines
#
spice_gtk_prefix = get_option('prefix')
spice_gtk_bindir = join_paths(spice_gtk_prefix, get_option('bindir'))
spice_gtk_datadir = join_paths(spice_gtk_prefix, get_option('datadir'))
spice_gtk_localedir = join_paths(spice_gtk_datadir, 'locale')
spice_gtk_includedir = join_paths(spice_gtk_prefix, get_option('includedir'))
spice_gtk_global_cflags = ['-DHAVE_CONFIG_H',
                           '-DSPICE_COMPILATION',
                           '-DG_LOG_DOMAIN="GSpice"',
                           #'-Werror',
                           '-Wall',
                           '-Wextra',
                           '-Wno-sign-compare',
                           '-Wno-unused-parameter',
                           '-Wno-cast-function-type']

# other global vars
compiler = meson.get_compiler('c')
spice_gtk_config_data = configuration_data()
spice_gtk_include = [include_directories('.')]
spice_glib_deps = []
spice_gtk_deps = []
spice_acl_deps = []

#
# Spice common subproject
#
spice_common = subproject('spice-common', default_options : ['generate-code=client'])
spice_gtk_config_data.merge_from(spice_common.get_variable('spice_common_config_data'))
spice_glib_deps += spice_common.get_variable('spice_common_client_dep')

#
# check for system headers
#
headers = ['termios.h',
           'X11/XKBlib.h']

foreach header : headers
  if compiler.has_header(header)
    spice_gtk_config_data.set('HAVE_@0@'.format(header.underscorify().to_upper()), '1')
  endif
endforeach

spice_gtk_has_egl = compiler.has_header('epoxy/egl.h')
if spice_gtk_has_egl
    spice_gtk_config_data.set('HAVE_EPOXY_EGL_H', '1')
    spice_gtk_config_data.set('HAVE_EGL', '1') # FIXME: Use single define?
endif

#
# check for system functions
#
foreach func : ['clearenv', 'strtok_r']
  if compiler.has_function(func)
    spice_gtk_config_data.set('HAVE_@0@'.format(func.underscorify().to_upper()), '1')
  endif
endforeach

#
# check for mandatory dependencies
#
spice_protocol_version='>= 0.12.15'

glib_version = '2.46'
glib_version_info = '>= @0@'.format(glib_version)
pixman_version = '>= 0.17.7'

deps = {'spice-protocol' : spice_protocol_version,
        'glib-2.0'       : glib_version_info,
        'gio-2.0'        : glib_version_info,
        'gobject-2.0'    : glib_version_info,
        'pixman-1'       : pixman_version,
        'openssl'        : '>= 1.0.0'}

foreach dep, version : deps
  spice_glib_deps += dependency(dep, version : version)
endforeach

# mandatory dependencies, without specific version requirement
# TODO: specify minimum version for cairo, jpeg and zlib?
deps = ['cairo', 'libjpeg', 'zlib', 'json-glib-1.0']
if host_machine.system() == 'windows'
  deps += 'gio-windows-2.0'
else
  deps += 'gio-unix-2.0'
endif

foreach dep : deps
  spice_glib_deps += dependency(dep)
endforeach

deps = []
if host_machine.system() == 'windows'
  deps += ['libws2_32', 'libgdi32']
endif

foreach dep : deps
  spice_glib_deps += compiler.find_library(dep)
endforeach

#
# Non-mandatory/optional dependencies
#
optional_deps = {'celt051' : '>= 0.5.1.1',
                 'opus'    : '>= 0.9.14'}
foreach dep, version : optional_deps
  d = dependency(dep, required : get_option(dep), version : version)
  if d.found()
    spice_glib_deps += d
    spice_gtk_config_data.set('HAVE_@0@'.format(dep.underscorify().to_upper()), '1')
  endif
endforeach

# gtk
spice_gtk_has_gtk = false
gtk_version_required = '3.22'
d = dependency('gtk+-3.0', version : '>= @0@'.format(gtk_version_required),
               required: get_option('gtk'))
if d.found()
  spice_gtk_deps += d
  if host_machine.system() != 'windows'
    spice_gtk_deps += dependency('epoxy')
    spice_gtk_deps += dependency('x11')
    d = dependency('libva-x11', required: false)
    if d.found()
      spice_gtk_deps += d
      spice_gtk_config_data.set('HAVE_LIBVA', '1')
    endif
  endif
  spice_gtk_has_gtk = true
endif

# webdav
spice_gtk_has_phodav = false
d = dependency('libphodav-2.0', required: get_option('webdav'))
if d.found()
  spice_glib_deps += d
  d = dependency('libsoup-2.4', version : '>= 2.49.91', required: get_option('webdav'))
  if d.found()
    spice_glib_deps += d
    spice_gtk_config_data.set('USE_PHODAV', '1')
    spice_gtk_has_phodav = true
  endif
endif

# pulse
spice_gtk_has_pulse = false
d = dependency('libpulse-mainloop-glib', required: get_option('pulse'))
if d.found()
  warning('PulseAudio backend is deprecated and will be removed in a future release')
  spice_glib_deps += d
  spice_gtk_config_data.set('HAVE_PULSE', '1')
  spice_gtk_has_pulse = true
endif

deps = ['gstreamer-1.0', 'gstreamer-base-1.0', 'gstreamer-app-1.0', 'gstreamer-audio-1.0', 'gstreamer-video-1.0']
foreach dep : deps
  spice_glib_deps += dependency(dep)
endforeach

# builtin-mjpeg
spice_gtk_has_builtin_mjpeg = false
if get_option('builtin-mjpeg')
  spice_gtk_config_data.set('HAVE_BUILTIN_MJPEG', '1')
  spice_gtk_has_builtin_mjpeg = true
endif

# usbredir
spice_gtk_has_usbredir = false
d1 = dependency('libusbredirparser-0.5', required : get_option('usbredir'))
d2 = dependency('libusbredirhost', version : '>= 0.4.2', required : get_option('usbredir'))
d3 = dependency('libusb-1.0', version : '>= 1.0.16', required : get_option('usbredir'))
if d1.found() and d2.found() and d3.found()
  spice_glib_deps += [d1, d2, d3]
  spice_gtk_config_data.set('USE_USBREDIR', '1')
  spice_gtk_has_usbredir = true
endif

# polkit
spice_gtk_has_polkit = false
d = dependency('polkit-gobject-1', version : '>= 0.96', required : get_option('polkit'))
if d.found()
  spice_gtk_policy_dir = d.get_pkgconfig_variable('policydir')
  foreach func : ['polkit_authority_get_sync', 'polkit_authorization_result_get_dismissed']
    if compiler.has_function(func, dependencies : d)
      spice_gtk_config_data.set('HAVE_@0@'.format(func.to_upper()), '1')
    endif
  endforeach

  # TODO:  With 'auto', we should just disable polkit support if this is missing.
  if not compiler.has_function('acl_get_file')
    acl_dep = compiler.find_library('acl')
    if not compiler.has_function('acl_get_file', dependencies : acl_dep)
      error('PolicyKit support requested, but some required packages are not available')
    endif
    spice_acl_deps += acl_dep
  endif

  spice_acl_deps += d
  spice_acl_deps += dependency('gio-unix-2.0')
  spice_gtk_config_data.set('USE_POLKIT', '1')
  spice_gtk_has_polkit = true
endif

if spice_gtk_has_usbredir and not spice_gtk_has_polkit
  warning('Building with usbredir support, but *not* building the usb acl helper')
endif

# pie
spice_gtk_has_pie = false
if get_option('pie')
  spice_gtk_has_pie = true
endif

# usb-acl-helper-dir
spice_gtk_usb_acl_helper_dir = get_option('usb-acl-helper-dir')
if spice_gtk_usb_acl_helper_dir.strip() == ''
  spice_gtk_usb_acl_helper_dir = spice_gtk_bindir
endif
spice_gtk_config_data.set_quoted('ACL_HELPER_PATH', spice_gtk_usb_acl_helper_dir)

# usb-ids-path
spice_gtk_usb_ids_path = get_option('usb-ids-path')
if spice_gtk_usb_ids_path.strip() == ''
  usbutils = dependency('usbutils', required : false)
  if usbutils.found()
    spice_gtk_usb_ids_path = usbutils.get_pkgconfig_variable('usbids')
  endif
endif

if spice_gtk_usb_ids_path.strip() != ''
  spice_gtk_config_data.set('WITH_USBIDS', '1')
  spice_gtk_config_data.set_quoted('USB_IDS', spice_gtk_usb_ids_path)
endif

# coroutine
spice_gtk_coroutine = get_option('coroutine')
if spice_gtk_coroutine == 'auto'
  if host_machine.system() == 'windows'
    spice_gtk_coroutine = 'winfiber'
  else
    spice_gtk_coroutine = 'ucontext'
  endif
endif

if spice_gtk_coroutine == 'ucontext'
  foreach f : ['makecontext', 'swapcontext', 'getcontext']
    if not compiler.has_function(f)
      error('Function missing:' + f)
    endif
  endforeach
  spice_gtk_config_data.set('WITH_UCONTEXT', '1')
  if host_machine.system() == 'darwin'
    spice_gtk_config_data.set('_XOPEN_SOURCE', '1')
  endif
  endif

if spice_gtk_coroutine == 'gthread'
  spice_gtk_config_data.set('WITH_GTHREAD', '1')
endif

if spice_gtk_coroutine == 'winfiber'
  spice_gtk_config_data.set('WITH_WINFIBER', '1')
endif

# introspection
spice_gtk_has_introspection = false
d = dependency('gobject-introspection-1.0', version : '>= 0.94', required : get_option('introspection'))
if d.found()
  spice_glib_deps += d
  spice_gtk_has_introspection = true
endif

# vala (depends on introspection)
spice_gtk_has_vala = false
d = dependency('vapigen', required : get_option('vapi'))
if d.found()
  if not spice_gtk_has_introspection
    error('VAPI support requested without introspection')
  endif
  spice_gtk_has_vala = true
endif

# lz4
spice_gtk_has_lz4 = false
d = dependency('liblz4', required : get_option('lz4'))
if d.found()
  spice_glib_deps += d
  spice_gtk_config_data.set('USE_LZ4', '1')
  spice_gtk_has_lz4 = true
endif

# sasl
spice_gtk_has_sasl = false
d = dependency('libsasl2', required : get_option('sasl'))
if d.found()
  spice_glib_deps += d
  spice_gtk_config_data.set('HAVE_SASL', '1')
  spice_gtk_has_sasl = true
endif

# smartcard check
spice_gtk_has_smartcard = false
d = dependency('libcacard', version : '>= 2.5.1', required : get_option('smartcard'))
if d.found()
  spice_glib_deps += d
  spice_gtk_config_data.set('USE_SMARTCARD', '1')
endif

#
# global C defines
#
glib_encoded_version = 'GLIB_VERSION_@0@'.format(glib_version.underscorify())
spice_gtk_global_cflags += ['-DGLIB_VERSION_MIN_REQUIRED=@0@'.format(glib_encoded_version),
                            '-DGLIB_VERSION_MAX_ALLOWED=@0@'.format(glib_encoded_version)]

if spice_gtk_has_gtk
  gtk_encoded_version='GDK_VERSION_@0@'.format(gtk_version_required.underscorify())
  spice_gtk_global_cflags += ['-DGDK_VERSION_MIN_REQUIRED=@0@'.format(gtk_encoded_version),
                              '-DGDK_VERSION_MAX_ALLOWED=@0@'.format(gtk_encoded_version)]
endif

add_project_arguments(compiler.get_supported_arguments(spice_gtk_global_cflags),
                      language : 'c')

#
# Subdirectories
#
subdir('src')
subdir('tools')
subdir('tests')
if build_machine.system() == 'windows'
  message('Disabling gtk-doc while building on Windows')
else
  if find_program('gtkdoc-scan', required : get_option('gtk_doc')).found()
    subdir('doc')
  else
    message('Not building documentation as gtk-doc was not found')
  endif
endif
subdir('data')
subdir('man')
subdir('po')
subdir('vapi')

#
# write config.h
#
proj_version = meson.project_version()
proj_name = meson.project_name()
config_data = {'VERSION' : proj_version,
               'PACKAGE_VERSION' : proj_version,
               'GETTEXT_PACKAGE' : proj_name,
               'LOCALE_DIR' : spice_gtk_localedir,
               'PACKAGE_STRING' : '@0@ @1@'.format(proj_name, proj_version),
               'PACKAGE_BUGREPORT' : 'spice-devel@lists.freedesktop.org'}
foreach key, value : config_data
  spice_gtk_config_data.set_quoted(key, value)
endforeach

configure_file(output : 'config.h',
               install : false,
               configuration : spice_gtk_config_data)

#
# write spice-client-glib.pc
#
pkgconfig = import('pkgconfig')
pkgconfig.generate(spice_client_glib_lib,
                   description : 'SPICE Client GLib 2.0 library',
                   subdirs : 'spice-client-glib-2.0',
                   requires : 'spice-protocol',
                   variables : 'exec_prefix=${prefix}')

#
# write spice-client-gtk.pc
#
if spice_gtk_has_gtk
  pkgconfig.generate(spice_client_gtk_lib,
                     description : 'SPICE Client Gtk 3.0 library',
                     subdirs : 'spice-client-gtk-3.0',
                     requires : 'spice-client-glib-2.0 gtk+-3.0 >= @0@'.format(gtk_version_required),
                     variables : 'exec_prefix=${prefix}')
endif
