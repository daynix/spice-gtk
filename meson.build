#
# project definition
#
project('spice-gtk', 'c',
         version : run_command('build-aux/git-version-gen', '${MESON_SOURCE_ROOT}/.tarball-version', check : true).stdout().strip(),
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
spice_gtk_deps = []
spice_gtk_host_system = host_machine.system()

#
# Spice common subproject
#
spice_common = subproject('spice-common', default_options : ['generate-code=client'])
spice_gtk_config_data.merge_from(spice_common.get_variable('spice_common_config_data'))
spice_gtk_deps += spice_common.get_variable('spice_common_client_dep')

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
  spice_gtk_deps += dependency(dep, version : version)
endforeach

# mandatory dependencies, without specific version requirement
# TODO: specify minimum version for cairo, jpeg and zlib?
deps = ['cairo', 'libjpeg', 'zlib', 'json-glib-1.0']
if spice_gtk_host_system == 'windows'
  deps += 'gio-windows-2.0'
else
  deps += 'gio-unix-2.0'
endif

foreach dep : deps
  spice_gtk_deps += dependency(dep)
endforeach

deps = ['librt', 'libm']
if spice_gtk_host_system == 'windows'
  deps += ['libws2_32', 'libgdi32']
endif

foreach dep : deps
  spice_gtk_deps += compiler.find_library(dep)
endforeach

#
# Non-mandatory/optional dependencies
#
optional_deps = {'celt051' : '>= 0.5.1.1',
                 'opus'    : '>= 0.9.14'}
foreach dep, version : optional_deps
  d = dependency(dep, required : get_option(dep), version : version)
  if d.found()
    spice_gtk_deps += d
    spice_gtk_config_data.set('HAVE_@0@'.format(dep.underscorify().to_upper()), '1')
  endif
endforeach

# gtk
spice_gtk_has_gtk = false
gtk_version_required = '3.22'
if get_option('gtk')
  spice_gtk_deps += dependency('gtk+-3.0', version : '>= @0@'.format(gtk_version_required))
  spice_gtk_deps += dependency('x11')
  if spice_gtk_host_system != 'windows'
    spice_gtk_deps += dependency('epoxy')
  endif
  spice_gtk_has_gtk = true
endif

# webdav
spice_gtk_has_phodav = false
if get_option('webdav')
  spice_gtk_deps += dependency('libphodav-2.0')
  spice_gtk_deps += dependency('libsoup-2.4', version : '>= 2.49.91')
  spice_gtk_config_data.set('USE_PHODAV', '1')
  spice_gtk_has_phodav = true
endif

# pulse
spice_gtk_has_pulse = false
if get_option('pulse')
  deps = ['libpulse', 'libpulse-mainloop-glib']
  foreach dep : deps
    spice_gtk_deps += dependency(dep)
  endforeach
  spice_gtk_config_data.set('HAVE_PULSE', '1')
  spice_gtk_has_pulse = true
endif

# gstaudio
gst_base_deps = ['gstreamer-1.0', 'gstreamer-base-1.0', 'gstreamer-app-1.0']
spice_gtk_has_gstaudio = false
if get_option('gstaudio')
  deps = gst_base_deps + ['gstreamer-audio-1.0']
  foreach dep : deps
    spice_gtk_deps += dependency(dep)
  endforeach
  spice_gtk_config_data.set('HAVE_GSTAUDIO', '1')
  spice_gtk_has_gstaudio = true
endif

# gstvideo
spice_gtk_has_gstvideo = false
if get_option('gstvideo')
  deps = ['gstreamer-video-1.0']
  if not spice_gtk_has_gstaudio
    deps += gst_base_deps
  endif
  foreach dep : deps
    spice_gtk_deps += dependency(dep)
  endforeach
  spice_gtk_config_data.set('HAVE_GSTVIDEO', '1')
  spice_gtk_has_gstvideo = true
endif

# builtin-mjpeg
spice_gtk_has_builtin_mjpeg = false
if get_option('builtin-mjpeg')
  spice_gtk_config_data.set('HAVE_BUILTIN_MJPEG', '1')
  spice_gtk_has_builtin_mjpeg = true
endif

if not spice_gtk_has_gstvideo and not spice_gtk_has_builtin_mjpeg
  warning('No builtin MJPEG or GStreamer decoder, video will not be streamed')
endif

# usbredir
spice_gtk_has_usbredir = false
if get_option('usbredir')
  usb_dep = dependency('libusbredirparser-0.5', required : false)
  if not usb_dep.found()
    usb_dep = dependency('libusbredirparser', version : '>= 0.4')
  endif
  spice_gtk_deps += usb_dep

  deps = {'libusbredirhost' : '>= 0.4.2',
          'libusb-1.0' : '>= 1.0.9'}

  foreach dep, version : deps
    usb_dep = dependency(dep, version : version)
    spice_gtk_deps += usb_dep
  endforeach

  if spice_gtk_host_system != 'windows'
    if usb_dep.version().version_compare('>= 1.0.16')
      spice_gtk_config_data.set('USE_LIBUSB_HOTPLUG', '1')
    else
      spice_gtk_deps += dependency('gudev-1.0')
      spice_gtk_config_data.set('USE_GUDEV', '1')
    endif
  endif

  spice_gtk_config_data.set('USE_USBREDIR', '1')
  spice_gtk_has_usbredir = true
endif

# polkit
spice_gtk_has_polkit = false
if get_option('polkit')
  polkit_dep = dependency('polkit-gobject-1', version : '>= 0.96')# ,required : false)
  if polkit_dep.found()
    spice_gtk_policy_dir = polkit_dep.get_pkgconfig_variable('policydir')
    foreach func : ['polkit_authority_get_sync', 'polkit_authorization_result_get_dismissed']
      if compiler.has_function(func, dependencies : polkit_dep)
        spice_gtk_config_data.set('HAVE_@0@'.format(func.to_upper()), '1')
      endif
    endforeach

    if not compiler.has_function('acl_get_file')
      acl_dep = compiler.find_library('acl')
      if not compiler.has_function('acl_get_file', dependencies : acl_dep)
        error('PolicyKit support requested, but some required packages are not available')
      endif
      spice_gtk_deps += acl_dep
    endif
  endif

  spice_gtk_deps += polkit_dep
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
if spice_gtk_coroutine == 'ucontext'
  if compiler.has_function('makecontext') and compiler.has_function('swapcontext') and compiler.has_function('getcontext')
    spice_gtk_config_data.set('WITH_UCONTEXT', '1')
    if spice_gtk_host_system == 'darwin'
      spice_gtk_config_data.set('_XOPEN_SOURCE', '1')
    endif
  else
    spice_gtk_coroutine = 'gthread'
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
if get_option('introspection')
  spice_gtk_deps += dependency('gobject-introspection-1.0', version : '>= 0.94')
  spice_gtk_has_introspection = true
endif

# vala (depends on introspection)
spice_gtk_has_vala = false
if spice_gtk_has_introspection and get_option('vapi')
  vapigen_dep = dependency('vapigen')
  vapidir = vapigen_dep.get_pkgconfig_variable('vapidir')
  vapigen = dependency('vapigen').get_pkgconfig_variable('vapigen')
  spice_gtk_has_vala = true
endif

# dbus
if get_option('dbus')
  spice_gtk_config_data.set('USE_GDBUS', '1')
else
  warning('No D-Bus support, desktop integration and USB redirection may not work properly')
endif

# lz4
spice_gtk_has_lz4 = false
if get_option('lz4')
  lz4_dep = dependency('liblz4', required : false, version : '>= 129')
  if not lz4_dep.found()
    lz4_dep = dependency('liblz4', version : '>= 1.7.3')
  endif

  spice_gtk_deps += lz4_dep
  spice_gtk_config_data.set('USE_LZ4', '1')
  spice_gtk_has_lz4 = true
endif

# sasl
spice_gtk_has_sasl = false
if get_option('sasl')
  spice_gtk_deps += dependency('libsasl2')
  spice_gtk_config_data.set('HAVE_SASL', '1')
  spice_gtk_has_sasl = true
endif

# smartcard check
spice_gtk_has_smartcard = false
if get_option('smartcard')
  smartcard_dep = dependency('libcacard', required : false, version : '>= 2.5.1')
  if smartcard_dep.found()
    spice_gtk_deps += smartcard_dep
    spice_gtk_config_data.set('USE_SMARTCARD', '1')
  else
    smartcard012_dep = dependency('libcacard', required : false, version : '>= 0.1.2')
    if smartcard012_dep.found()
      spice_gtk_deps += smartcard012_dep
      spice_gtk_config_data.set('USE_SMARTCARD_012', '1')
    endif
  endif

  spice_gtk_has_smartcard = smartcard_dep.found() or smartcard012_dep.found()
  if not spice_gtk_has_smartcard
    error('Building with smartcard support but dependency not found')
  endif
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
subdir('doc')
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
