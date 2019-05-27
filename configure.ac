AC_PREREQ([2.57])

AC_INIT([spice-gtk], [m4_esyscmd(build-aux/git-version-gen .tarball-version)],
        [spice-devel@lists.freedesktop.org])

AC_CONFIG_MACRO_DIR([m4])
m4_include([subprojects/spice-common/m4/spice-deps.m4])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([foreign dist-bzip2 -Wall -Werror -Wno-portability])
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
LT_INIT([disable-static win32-dll])
AM_MAINTAINER_MODE

GETTEXT_PACKAGE=AC_PACKAGE_TARNAME
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE], "$GETTEXT_PACKAGE", [GETTEXT package name])
AM_GNU_GETTEXT_VERSION([0.18.2])
AM_GNU_GETTEXT([external])


GTK_DOC_CHECK([1.14],[--flavour no-tmpl])

AC_PROG_CC
AC_PROG_CC_C99
if test "x$ac_cv_prog_cc_c99" = xno; then
    AC_MSG_ERROR([C99 compiler is required.])
fi


AC_CHECK_PROG([STOW], [stow], [yes], [no])
AS_IF([test "x$STOW" = "xyes" && test -d /usr/local/stow], [
    AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME} ***])
    ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}"
])

AC_PROG_INSTALL
AC_CANONICAL_HOST
AC_PROG_LIBTOOL
AM_PROG_CC_C_O
AC_C_BIGENDIAN
AM_PATH_PYTHON(,, [:])
RRA_LD_VERSION_SCRIPT

AC_MSG_CHECKING([for native Win32])
case "$host_os" in
     *mingw*|*cygwin*)
        os_win32=yes
        gio_os=gio-windows-2.0
        red_target=Windows
        ;;
     *)
        os_win32=no
        gio_os=gio-unix-2.0
        red_target=Unix
        ;;
esac
AC_MSG_RESULT([$os_win32])
AM_CONDITIONAL([OS_WIN32],[test "$os_win32" = "yes"])

AC_MSG_CHECKING([for native macOS])
case "$host_os" in
     *darwin*)
        os_mac=yes
        # Workaround gtk+ exposing Objective C: https://gitlab.gnome.org/GNOME/gtk/issues/1737
        CFLAGS="${CFLAGS} -ObjC"
        ;;
     *)
        os_mac=no
        ;;
esac
AC_MSG_RESULT([$os_mac])
AM_CONDITIONAL([OS_MAC],[test "$os_mac" = "yes"])

AC_CHECK_HEADERS([sys/socket.h sys/types.h netinet/in.h arpa/inet.h])
AC_CHECK_HEADERS([termios.h])
AC_CHECK_HEADERS([epoxy/egl.h],
                 [have_egl=yes],
                 [have_egl=no])
AC_MSG_CHECKING([if we can use EGL in libepoxy])
AC_MSG_RESULT([$have_egl])
AM_CONDITIONAL([HAVE_EGL],[test "$have_egl" = "yes"])
AS_IF([test "$have_egl" = "yes"],
       AC_DEFINE([HAVE_EGL], [1], [Define if supporting EGL]))

AC_CHECK_LIBM
AC_SUBST(LIBM)

AC_CONFIG_SUBDIRS([subprojects/spice-common])
PKG_CHECK_MODULES([SPICE_PROTOCOL], [spice-protocol >= 0.12.15])

COMMON_CFLAGS='-I${top_builddir}/subprojects/spice-common/ -I${top_srcdir}/subprojects/spice-common/ ${SPICE_PROTOCOL_CFLAGS}'
AC_SUBST(COMMON_CFLAGS)

SPICE_COMMON_DIR='${top_builddir}/subprojects/spice-common'
AC_SUBST(SPICE_COMMON_DIR)

SPICE_GTK_MAJOR_VERSION=`echo $PACKAGE_VERSION | cut -d. -f1`
SPICE_GTK_MINOR_VERSION=`echo $PACKAGE_VERSION | cut -d. -f2`
SPICE_GTK_MICRO_VERSION=`echo $PACKAGE_VERSION | cut -d. -f3 | cut -d- -f1`
AS_IF([test "x$SPICE_GTK_MICRO_VERSION" = "x"], [SPICE_GTK_MICRO_VERSION=0])

AC_SUBST(SPICE_GTK_MAJOR_VERSION)
AC_SUBST(SPICE_GTK_MINOR_VERSION)
AC_SUBST(SPICE_GTK_MICRO_VERSION)

dnl =========================================================================
dnl Chek optional features

SPICE_GLIB_REQUIRES=""
SPICE_GLIB_LIBS=""
SPICE_GTK_REQUIRES=""

PKG_CHECK_MODULES(PIXMAN, pixman-1 >= 0.17.7)

SPICE_GLIB_REQUIRES="${SPICE_GLIB_REQUIRES} pixman-1 >= 0.17.7"

SPICE_CHECK_OPENSSL

PKG_CHECK_EXISTS(openssl,
  [SPICE_GLIB_REQUIRES="${SPICE_GLIB_REQUIRES} openssl"],
  [SPICE_GLIB_LIBS="${SPICE_GLIB_LIBS} ${OPENSSL_LIBS}"])

SPICE_CHECK_RECORDER
SPICE_CHECK_SASL

AC_MSG_CHECKING([which gtk+ version to compile against])
AC_ARG_WITH([gtk],
  [AS_HELP_STRING([--with-gtk=@<:@3.0/no@:>@],[which gtk+ version to compile against @<:@default=3.0@:>@])],
  [case "$with_gtk" in
     3.0) AC_MSG_RESULT([$with_gtk]) ;;
     no) AC_MSG_RESULT([none]) ;;
     *) AC_MSG_ERROR([invalid gtk version specified]) ;;
   esac],
  [with_gtk=3.0])

case "$with_gtk" in
  3.0) GTK_REQUIRED=3.22
       GTK_ENCODED_VERSION="GDK_VERSION_3_22"
       ;;
  no)
       AS_IF([test x$enable_gtk_doc = xyes],
             [AC_MSG_ERROR([Without GTK+, gtk-doc must be disabled])])
esac

AC_SUBST([GTK_REQUIRED])
AM_CONDITIONAL([WITH_GTK],[test "$with_gtk" != "no"])

AS_IF([test "x$with_gtk" != "xno"],
      [AS_IF([test "x$os_win32" = "xyes"],
             [PKG_CHECK_MODULES(GTK, gtk+-3.0 >= $GTK_REQUIRED)],
             [PKG_CHECK_MODULES(GTK, gtk+-3.0 >= $GTK_REQUIRED epoxy)])]
      [GTK_CFLAGS="$GTK_CFLAGS -DGDK_VERSION_MIN_REQUIRED=$GTK_ENCODED_VERSION \
                               -DGDK_VERSION_MAX_ALLOWED=$GTK_ENCODED_VERSION"])
SPICE_GTK_REQUIRES="${SPICE_GTK_REQUIRES} gtk+-3.0 >= $GTK_REQUIRED"

PKG_CHECK_EXISTS([gtk+-x11-$with_gtk], [PKG_CHECK_MODULES(X11, x11)])
AC_CHECK_HEADERS([X11/XKBlib.h])

AC_CHECK_FUNCS(clearenv strtok_r)

# Keep these two definitions in agreement.
GLIB2_REQUIRED="2.46"
GLIB2_ENCODED_VERSION="GLIB_VERSION_2_46"

PKG_CHECK_MODULES(GLIB2, glib-2.0 >= $GLIB2_REQUIRED)
GLIB2_CFLAGS="$GLIB2_CFLAGS -DGLIB_VERSION_MIN_REQUIRED=$GLIB2_ENCODED_VERSION \
  -DGLIB_VERSION_MAX_ALLOWED=$GLIB2_ENCODED_VERSION"

PKG_CHECK_MODULES(GOBJECT2, gobject-2.0)

PKG_CHECK_MODULES(GIO, gio-2.0 >= 2.36 $gio_os)

PKG_CHECK_MODULES(CAIRO, cairo >= 1.2.0)

PKG_CHECK_MODULES(GTHREAD, gthread-2.0)

PKG_CHECK_MODULES(JSON, json-glib-1.0)

PKG_CHECK_EXISTS([libva-x11], [
    PKG_CHECK_MODULES(LIBVA, libva-x11)
    AC_DEFINE([HAVE_LIBVA], [1], [Define if libva is available])
])

AC_ARG_ENABLE([webdav],
  AS_HELP_STRING([--enable-webdav=@<:@auto/yes/no@:>@],
                 [Enable webdav support @<:@default=auto@:>@]),
  [],
  [enable_webdav="auto"])

if test "x$enable_webdav" = "xno"; then
  have_phodav="no"
else
  PKG_CHECK_MODULES(PHODAV, [libphodav-2.0 glib-2.0 >= 2.43.90 libsoup-2.4 >= 2.49.91], [have_phodav=yes], [have_phodav=no])

  if test "x$have_phodav" = "xno" && test "x$enable_webdav" = "xyes"; then
    AC_MSG_ERROR([webdav support explicitly requested, but some required packages are not available])
  fi
fi
AS_IF([test "x$have_phodav" = "xyes"],
       AC_DEFINE([USE_PHODAV], [1], [Define if supporting phodav]))

AM_CONDITIONAL([WITH_PHODAV], [test "x$have_phodav" = "xyes"])

AC_ARG_ENABLE([pulse],
  AS_HELP_STRING([--enable-pulse=@<:@yes/auto/no@:>@], [Enable the PulseAudio backend @<:@default=auto@:>@]),
  [],
  [enable_pulse="auto"])
AS_IF([test "x$enable_pulse" != "xno"],
      [PKG_CHECK_MODULES(PULSE, [libpulse libpulse-mainloop-glib],
         [AC_DEFINE([HAVE_PULSE], 1, [Have PulseAudio support?])
          enable_pulse="yes"],
         [AS_IF([test "x$enable_pulse" = "xyes"],
                AC_MSG_ERROR([PulseAudio requested but not found]))
          enable_pulse="no"
      ])
])
AM_CONDITIONAL([HAVE_PULSE], [test "x$enable_pulse" = "xyes"])

GST_REQ="1.10"
SPICE_CHECK_GSTREAMER(GSTAUDIO, 1.0,
    [gstreamer-1.0 >= $GST_REQ gstreamer-base-1.0 >= $GST_REQ gstreamer-app-1.0 >= $GST_REQ gstreamer-audio-1.0 >= $GST_REQ],
    [SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0,
         [gst-plugins-base 1.0], [audioconvert audioresample appsink])
     SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0,
         [gst-plugins-good 1.0], [autoaudiosrc])
     AS_IF([test x"$missing_gstreamer_elements" = "xyes"],
         SPICE_WARNING([The GStreamer 1.0 audio backend can be built but may not work.]))
    ],
    [AC_MSG_ERROR([Required GStreamer packages missing or system version is below $GST_REQ])])

SPICE_CHECK_GSTREAMER(GSTVIDEO, 1.0,
    [gstreamer-1.0 >= $GST_REQ gstreamer-base-1.0 >= $GST_REQ gstreamer-app-1.0 >= $GST_REQ gstreamer-video-1.0 >= $GST_REQ],
    [missing_gstreamer_elements=""
     SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0,
         [gst-plugins-base 1.0], [appsrc videoconvert appsink])
     SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0,
         [gst-plugins-good 1.0], [jpegdec vp8dec vp9dec])
     SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0,
         [gst-plugins-bad 1.0], [h264parse h265parse])
     SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0,
         [gstreamer-libav 1.0], [avdec_h264 avdec_h265])
     AS_IF([test x"$missing_gstreamer_elements" = "xyes"],
         SPICE_WARNING([The GStreamer video decoder can be built but may not work.]))
    ],
    [AC_MSG_ERROR([Required GStreamer packages missing or system version is below $GST_REQ])])

AC_ARG_ENABLE([builtin-mjpeg],
  AS_HELP_STRING([--enable-builtin-mjpeg], [Enable the builtin mjpeg video decoder @<:@default=yes@:>@]),
  [],
  enable_builtin_mjpeg="yes")
AS_IF([test "x$enable_builtin_mjpeg" = "xyes"],
      [AC_DEFINE([HAVE_BUILTIN_MJPEG], 1, [Use the builtin mjpeg decoder?])])
AM_CONDITIONAL(HAVE_BUILTIN_MJPEG, [test "x$enable_builtin_mjpeg" != "xno"])

AC_CHECK_LIB(jpeg, jpeg_destroy_decompress,
    AC_MSG_CHECKING([for jpeglib.h])
    AC_TRY_CPP(
[#include <stdio.h>
#undef PACKAGE
#undef VERSION
#undef HAVE_STDLIB_H
#include <jpeglib.h>],
        JPEG_LIBS='-ljpeg'
        AC_MSG_RESULT($jpeg_ok),
	AC_MSG_ERROR([jpeglib.h not found])),
    AC_MSG_ERROR([libjpeg not found]))
AC_SUBST(JPEG_LIBS)

AC_CHECK_LIB(z, deflate, Z_LIBS='-lz', AC_MSG_ERROR([zlib not found]))
AC_SUBST(Z_LIBS)

SPICE_CHECK_SMARTCARD
AM_CONDITIONAL([WITH_SMARTCARD], [test "x$have_smartcard" = "xyes"])

AC_ARG_ENABLE([usbredir],
  AS_HELP_STRING([--enable-usbredir=@<:@auto/yes/no@:>@],
                 [Enable usbredir support @<:@default=auto@:>@]),
  [],
  [enable_usbredir="auto"])

if test "x$enable_usbredir" = "xno"; then
  have_usbredir="no"
else
  PKG_CHECK_MODULES([USBREDIR],
                    [libusb-1.0 >= 1.0.16 libusbredirhost >= 0.7.1 libusbredirparser-0.5 >= 0.7.1],
                    [have_usbredir=yes],
                    [have_usbredir=no])
  if test "x$have_usbredir" = "xno" && test "x$enable_usbredir" = "xyes"; then
    AC_MSG_ERROR([usbredir support explicitly requested, but some required packages are not available])
  fi

  if test "x$have_usbredir" = "xyes"; then
    AC_DEFINE([USE_USBREDIR], [1], [Define if supporting usbredir proxying])
  fi
fi
AM_CONDITIONAL([WITH_USBREDIR], [test "x$have_usbredir" = "xyes"])

AC_ARG_ENABLE([polkit],
  AS_HELP_STRING([--enable-polkit=@<:@auto/yes/no@:>@],
                 [Enable PolicyKit support (for the usb acl helper)@<:@default=auto@:>@]),
  [],
  [enable_polkit="auto"])

if test "x$have_usbredir" = "xyes" && test "x$enable_polkit" != "xno"; then
  PKG_CHECK_MODULES([POLKIT], [polkit-gobject-1 >= 0.96],
                              [have_polkit=yes],
                              [have_polkit=no])
  AC_CHECK_HEADER([sys/acl.h], [], [have_polkit=no])
  AC_CHECK_LIB([c], [acl_get_file], [ACL_LIBS=""], [
    AC_CHECK_LIB([acl], [acl_get_file], [ACL_LIBS=-lacl], [have_polkit=no])])
  if test "x$enable_polkit" = "xyes" && test "x$have_polkit" = "xno"; then
    AC_MSG_ERROR([PolicyKit support explicitly requested, but some required packages are not available])
  fi

  if test "x$have_polkit" = "xyes"; then
    AC_SUBST(ACL_LIBS)
    AC_DEFINE([USE_POLKIT], [1], [Define if supporting polkit])
  fi
  AM_CONDITIONAL([WITH_POLKIT], [test "x$have_polkit" = "xyes"])
  POLICYDIR=`${PKG_CONFIG} polkit-gobject-1 --variable=policydir`
  AC_SUBST(POLICYDIR)
  # Check for polkit_authority_get_sync()
  AC_CHECK_LIB([polkit-gobject-1], [polkit_authority_get_sync], ac_have_pk_auth_get_sync="1", ac_have_pk_auth_get_sync="0")
  AC_DEFINE_UNQUOTED([HAVE_POLKIT_AUTHORITY_GET_SYNC], $ac_have_pk_auth_get_sync, [Define if you have a polkit with polkit_authority_get_sync()])
  AC_CHECK_LIB([polkit-gobject-1], [polkit_authorization_result_get_dismissed], ac_have_pk_authorization_result_get_dismissed="1", ac_have_pk_authorization_result_get_dismissed="0")
  AC_DEFINE_UNQUOTED([HAVE_POLKIT_AUTHORIZATION_RESULT_GET_DISMISSED], $ac_have_pk_authorization_result_get_dismissed, [Define if you have a polkit with polkit_authorization_result_get_dismissed()])
else
  AM_CONDITIONAL(WITH_POLKIT, false)
fi

if test "x$have_usbredir" = "xyes" && test "x$have_polkit" != "xyes"; then
  AC_MSG_WARN([Building with usbredir support, but *not* building the usb acl helper])
fi

AC_ARG_ENABLE([pie],
  AS_HELP_STRING([--enable-pie=@<:@auto/yes/no@:>@],
                 [Enable position-independent-executable support (for the usb acl helper)@<:@default=auto@:>@]),
  [],
  [enable_pie="auto"])

if test "x$have_polkit" = "xyes" && test "x$enable_pie" != "xno"; then
  save_CFLAGS="$CFLAGS"
  save_LDFLAGS="$LDFLAGS"
  CFLAGS="$CFLAGS -fPIE"
  LDFLAGS="$LDFLAGS -pie -Wl,-z,relro -Wl,-z,now"
  AC_MSG_CHECKING([for PIE support])
  AC_LINK_IFELSE([AC_LANG_SOURCE([void main () {}])],
                 [have_pie=yes],
                 [have_pie=no])
  AC_MSG_RESULT([$have_pie])
  if test "x$have_pie" = "xno" && test "x$enable_pie" = "xyes"; then
    AC_MSG_ERROR([pie support explicitly requested, but your toolchain does not support it])
  fi
  if test "x$have_pie" = "xyes"; then
    PIE_CFLAGS="-fPIE"
    PIE_LDFLAGS="-pie -Wl,-z,relro -Wl,-z,now"
    AC_SUBST(PIE_CFLAGS)
    AC_SUBST(PIE_LDFLAGS)
  fi
  CFLAGS="$save_CFLAGS"
  LDFLAGS="$save_LDFLAGS"
fi

AC_ARG_WITH([usb-acl-helper-dir],
  AS_HELP_STRING([--with-usb-acl-helper-dir=DIR],
                 [Directory where the USB ACL helper binary should be installed]),
  [ACL_HELPER_DIR="$with_usb_acl_helper_dir"],
  [ACL_HELPER_DIR="${bindir}/"])
AC_SUBST([ACL_HELPER_DIR])

AC_ARG_WITH([usb-ids-path],
  AC_HELP_STRING([--with-usb-ids-path],
                 [Specify the path to usb.ids @<:@default=auto@:>@]),
  [USB_IDS="$with_usb_ids_path"],
  [USB_IDS="auto"])
AC_MSG_CHECKING([for usb.ids])
if test "x$USB_IDS" = "xauto"; then
  if test -n "$PKG_CONFIG"; then
    USB_IDS=$($PKG_CONFIG --variable=usbids usbutils)
  else
    USB_IDS=
  fi
fi
if test -n "$USB_IDS"; then
  AC_MSG_RESULT([$USB_IDS])
  AC_SUBST(USB_IDS)
  AC_DEFINE([WITH_USBIDS], [1], [Define if compiling with usb.ids support])
else
  AC_MSG_RESULT([not found])
fi

AC_ARG_WITH([coroutine],
  AS_HELP_STRING([--with-coroutine=@<:@ucontext/gthread/winfiber/auto@:>@],
                 [use ucontext or GThread for coroutines @<:@default=auto@:>@]),
  [],
  [with_coroutine=auto])

case $with_coroutine in
  ucontext|gthread|winfiber|auto) ;;
  *) AC_MSG_ERROR(Unsupported coroutine type)
esac

if test "$with_coroutine" = "auto"; then
  if test "$os_win32" = "yes"; then
    with_coroutine=winfiber
  elif test "$os_mac" = "yes"; then
    with_coroutine=ucontext
    AC_DEFINE([_XOPEN_SOURCE], [1], [Define _XOPEN_SOURCE on macOS for ucontext])
  else
    with_coroutine=ucontext
  fi
fi

if test "$with_coroutine" = "ucontext"; then
  AC_CHECK_FUNC(makecontext, [],[with_coroutine=gthread])
  AC_CHECK_FUNC(swapcontext, [],[with_coroutine=gthread])
  AC_CHECK_FUNC(getcontext, [],[with_coroutine=gthread])
fi

WITH_UCONTEXT=0
WITH_GTHREAD=0
WITH_WINFIBER=0

case $with_coroutine in
  ucontext) WITH_UCONTEXT=1 ;;
  gthread) WITH_GTHREAD=1 ;;
  winfiber) WITH_WINFIBER=1 ;;
  *) AC_MSG_ERROR(Unsupported coroutine type)
esac

AC_DEFINE_UNQUOTED([WITH_UCONTEXT],[$WITH_UCONTEXT], [Whether to use ucontext coroutine impl])
AM_CONDITIONAL(WITH_UCONTEXT, [test "x$WITH_UCONTEXT" = "x1"])

AC_DEFINE_UNQUOTED([WITH_WINFIBER],[$WITH_WINFIBER], [Whether to use fiber coroutine impl])
AM_CONDITIONAL(WITH_WINFIBER, [test "x$WITH_WINFIBER" = "x1"])

AC_DEFINE_UNQUOTED([WITH_GTHREAD],[$WITH_GTHREAD], [Whether to use gthread coroutine impl])
AM_CONDITIONAL(WITH_GTHREAD, [test "x$WITH_GTHREAD" = "x1"])

AM_CONDITIONAL([HAVE_INTROSPECTION], [test "0" = "1"])
m4_ifdef([GOBJECT_INTROSPECTION_CHECK],[
    PKG_CHECK_EXISTS([GOBJECT_INTROSPECTION],
            [gobject-introspection-1.0 >= 0.9.4],
            [has_symbol_prefix=yes], [:])
    GOBJECT_INTROSPECTION_CHECK([0.6.7])
])
AM_CONDITIONAL([G_IR_SCANNER_SYMBOL_PREFIX], [test "x$has_symbol_prefix" = "xyes"])

AC_ARG_ENABLE([vala],
  AS_HELP_STRING([--enable-vala], [Check for vala requirements @<:@default=no@:>@]),
  [],
  enable_vala="no")

VALA_REQUIRED=0.14
if test x$enable_vala = xyes ; then
    # check for vala
    AM_PROG_VALAC([$VALA_REQUIRED])
    AC_PATH_PROG(VAPIGEN, vapigen, no)
    if test "x$VAPIGEN" == "xno"; then
        AC_MSG_ERROR([Cannot find the "vapigen" binary in your PATH])
    fi
    AC_SUBST(VAPIGEN)
fi

AM_CONDITIONAL(WITH_VALA, [test "x$enable_vala" = "xyes"])

VAPIDIR="${datadir}/vala/vapi"
AC_SUBST(VAPIDIR)

AC_ARG_ENABLE([alignment-checks],
  AS_HELP_STRING([--enable-alignment-checks],
                 [Enable runtime checks for cast alignment @<:@default=no@:>@]),
  [],
  enable_alignment_checks="no")
AS_IF([test "x$enable_alignment_checks" = "xyes"],
      [AC_DEFINE([SPICE_DEBUG_ALIGNMENT], 1, [Enable runtime checks for cast alignment])])

SPICE_CHECK_LZ4

dnl ===========================================================================
dnl check compiler flags

# some glib/gstreamer enums use 1 << 31
dontwarn="-Wshift-overflow=2"

SPICE_COMPILE_WARNINGS([$dontwarn])

SPICE_CFLAGS="$SPICE_CFLAGS $WARN_CFLAGS"

AC_SUBST(SPICE_CFLAGS)

SPICE_GLIB_CFLAGS="$PIXMAN_CFLAGS $PULSE_CFLAGS $GSTAUDIO_CFLAGS $GSTVIDEO_CFLAGS $GLIB2_CFLAGS $GIO_CFLAGS $GOBJECT2_CFLAGS $OPENSSL_CFLAGS $SASL_CFLAGS"
SPICE_GTK_CFLAGS="$SPICE_GLIB_CFLAGS $GTK_CFLAGS "

AC_SUBST(SPICE_GLIB_CFLAGS)
AC_SUBST(SPICE_GTK_CFLAGS)

AC_SUBST(SPICE_GLIB_REQUIRES)
AC_SUBST(SPICE_GLIB_LIBS)
AC_SUBST(SPICE_GTK_REQUIRES)

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_OUTPUT([
Makefile
spice-client-glib-2.0.pc
spice-client-gtk-3.0.pc
data/Makefile
po/Makefile.in
src/Makefile
src/spice-version.h
tools/Makefile
doc/Makefile
doc/reference/Makefile
man/Makefile
vapi/Makefile
tests/Makefile
])

dnl ==========================================================================
AC_MSG_NOTICE([

        Spice-Gtk $VERSION
        ==============

        prefix:                   ${prefix}
        c compiler:               ${CC}
        Target:                   ${red_target}

        Gtk:                      ${with_gtk}
        Coroutine:                ${with_coroutine}
        PulseAudio:               ${enable_pulse}
        SASL support:             ${have_sasl}
        Smartcard support:        ${have_smartcard}
        USB redirection support:  ${have_usbredir} ${with_usbredir_hotplug}
        WebDAV support:           ${have_phodav}
        LZ4 support:              ${have_lz4}

        Now type 'make' to build $PACKAGE

])
SPICE_PRINT_MESSAGES

AS_IF([test "x$enable_pulse" = "xyes"],
  AC_MSG_WARN([PulseAudio backend is deprecated and will be removed in a future release]))
