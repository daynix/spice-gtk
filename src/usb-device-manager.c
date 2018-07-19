/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2011, 2012 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <glib-object.h>
#include <errno.h>

#ifdef USE_USBREDIR

#ifdef G_OS_WIN32
#include <windows.h>
#include "usbdk_api.h"
#endif

#if defined(USE_GUDEV)
#include <gudev/gudev.h>
#elif defined(G_OS_WIN32)
#include "win-usb-dev.h"
#define USE_GUDEV /* win-usb-dev.h provides a fake gudev interface */
#elif !defined USE_LIBUSB_HOTPLUG
#error "Expecting one of USE_GUDEV or USE_LIBUSB_HOTPLUG to be defined"
#endif

#include "channel-usbredir-priv.h"
#include "usbutil.h"

#endif

#include "spice-session-priv.h"
#include "spice-client.h"
#include "spice-marshal.h"
#include "usb-device-manager-priv.h"

#include <glib/gi18n-lib.h>

#ifndef G_OS_WIN32 /* Linux -- device id is bus.addr */
#define DEV_ID_FMT "at %u.%u"
#else /* Windows -- device id is vid:pid */
#define DEV_ID_FMT "0x%04x:0x%04x"
#endif

/**
 * SECTION:usb-device-manager
 * @short_description: USB device management
 * @title: Spice USB Manager
 * @section_id:
 * @see_also:
 * @stability: Stable
 * @include: spice-client.h
 *
 * #SpiceUsbDeviceManager monitors USB redirection channels and USB
 * devices plugging/unplugging. If #SpiceUsbDeviceManager:auto-connect
 * is set to %TRUE, it will automatically connect newly plugged USB
 * devices to available channels.
 *
 * There should always be a 1:1 relation between #SpiceUsbDeviceManager objects
 * and #SpiceSession objects. Therefor there is no
 * spice_usb_device_manager_new, instead there is
 * spice_usb_device_manager_get() which ensures this 1:1 relation.
 */

/* ------------------------------------------------------------------ */
/* gobject glue                                                       */

#define SPICE_USB_DEVICE_MANAGER_GET_PRIVATE(obj)                                  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), SPICE_TYPE_USB_DEVICE_MANAGER, SpiceUsbDeviceManagerPrivate))

enum {
    PROP_0,
    PROP_SESSION,
    PROP_AUTO_CONNECT,
    PROP_AUTO_CONNECT_FILTER,
    PROP_REDIRECT_ON_CONNECT,
    PROP_FREE_CHANNELS,
    PROP_SHARE_CD
};

enum
{
    DEVICE_ADDED,
    DEVICE_REMOVED,
    AUTO_CONNECT_FAILED,
    DEVICE_ERROR,
    DEVICE_CHANGED,
    LAST_SIGNAL,
};

struct _SpiceUsbDeviceManagerPrivate {
    SpiceSession *session;
    gboolean auto_connect;
    gchar *auto_connect_filter;
    gchar *redirect_on_connect;
#ifdef USE_USBREDIR
    SpiceUsbBackend *context;
    int event_listeners;
    GThread *event_thread;
    gint event_thread_run;
    struct usbredirfilter_rule *auto_conn_filter_rules;
    struct usbredirfilter_rule *redirect_on_connect_rules;
    int auto_conn_filter_rules_count;
    int redirect_on_connect_rules_count;
#ifdef USE_GUDEV
    GUdevClient *udev;
    SpiceUsbBackendDevice **coldplug_list; /* Avoid needless reprobing during init */
#else
    gboolean redirecting; /* Handled by GUdevClient in the gudev case */
#endif
#ifdef G_OS_WIN32
    usbdk_api_wrapper     *usbdk_api;
    HANDLE                 usbdk_hider_handle;
#endif
#endif
    GPtrArray *devices;
    GPtrArray *channels;
};

enum {
    SPICE_USB_DEVICE_STATE_NONE = 0, /* this is also DISCONNECTED */
    SPICE_USB_DEVICE_STATE_CONNECTING,
    SPICE_USB_DEVICE_STATE_CONNECTED,
    SPICE_USB_DEVICE_STATE_DISCONNECTING,
    SPICE_USB_DEVICE_STATE_INSTALLING,
    SPICE_USB_DEVICE_STATE_UNINSTALLING,
    SPICE_USB_DEVICE_STATE_INSTALLED,
    SPICE_USB_DEVICE_STATE_MAX
};

#ifdef USE_USBREDIR

// this is the structure behind SpiceUsbDevice
typedef struct _SpiceUsbDeviceInfo {
    guint8  busnum;
    guint8  devaddr;
    guint16 vid;
    guint16 pid;
    gboolean isochronous;
#ifdef G_OS_WIN32
    guint8  state;
#else
    SpiceUsbBackendDevice *bdev;
#endif
    gint    ref;
} SpiceUsbDeviceInfo;


static void channel_new(SpiceSession *session, SpiceChannel *channel,
                        gpointer user_data);
static void channel_destroy(SpiceSession *session, SpiceChannel *channel,
                            gpointer user_data);
#ifdef USE_GUDEV
static void spice_usb_device_manager_uevent_cb(GUdevClient     *client,
                                               const gchar     *action,
                                               GUdevDevice     *udevice,
                                               gpointer         user_data);
static void spice_usb_device_manager_add_udev(SpiceUsbDeviceManager  *self,
                                              GUdevDevice            *udev);
#else
static void spice_usb_device_manager_hotplug_cb(
    void *data,
    SpiceUsbBackendDevice *bdev,
    gboolean added);
#endif

static SpiceUsbDeviceInfo *spice_usb_device_new(SpiceUsbBackendDevice *bdev);
static SpiceUsbDevice *spice_usb_device_ref(SpiceUsbDevice *device);
static void spice_usb_device_unref(SpiceUsbDevice *device);

#ifdef G_OS_WIN32
static void _usbdk_hider_update(SpiceUsbDeviceManager *manager);
static void _usbdk_hider_clear(SpiceUsbDeviceManager *manager);
#endif

static gboolean spice_usb_manager_device_equal_bdev(SpiceUsbDeviceManager *manager,
                                                      SpiceUsbDevice *device,
                                                    SpiceUsbBackendDevice *bdev);
static SpiceUsbBackendDevice*
spice_usb_device_manager_device_to_bdev(SpiceUsbDeviceManager *self,
                                          SpiceUsbDevice *device);

static void
_spice_usb_device_manager_connect_device_async(SpiceUsbDeviceManager *self,
                                               SpiceUsbDevice *device,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data);

static
void _connect_device_async_cb(GObject *gobject,
                              GAsyncResult *channel_res,
                              gpointer user_data);
static
void disconnect_device_sync(SpiceUsbDeviceManager *self,
                            SpiceUsbDevice *device);

static
void on_device_change(void *self, SpiceUsbBackendDevice *bdev);

G_DEFINE_BOXED_TYPE(SpiceUsbDevice, spice_usb_device,
                    (GBoxedCopyFunc)spice_usb_device_ref,
                    (GBoxedFreeFunc)spice_usb_device_unref)

static void
_set_redirecting(SpiceUsbDeviceManager *self, gboolean is_redirecting)
{
#ifdef USE_GUDEV
    g_object_set(self->priv->udev, "redirecting", is_redirecting, NULL);
#else
    self->priv->redirecting = is_redirecting;
#endif
}

#else
G_DEFINE_BOXED_TYPE(SpiceUsbDevice, spice_usb_device, g_object_ref, g_object_unref)
#endif

/**
 * spice_usb_device_manager_is_redirecting:
 * @self: the #SpiceUsbDeviceManager manager
 *
 * Checks whether a device is being redirected
 *
 * Returns: %TRUE if device redirection negotiation flow is in progress
 *
 * Since: 0.32
 */
gboolean spice_usb_device_manager_is_redirecting(SpiceUsbDeviceManager *self)
{
#ifdef USE_USBREDIR

#ifdef USE_GUDEV
    gboolean redirecting;
    g_object_get(self->priv->udev, "redirecting", &redirecting, NULL);
    return redirecting;
#else
    return self->priv->redirecting;
#endif

#else
    return FALSE;
#endif
}

static void spice_usb_device_manager_initable_iface_init(GInitableIface *iface);

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE_WITH_CODE(SpiceUsbDeviceManager, spice_usb_device_manager, G_TYPE_OBJECT,
     G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, spice_usb_device_manager_initable_iface_init));

static void spice_usb_device_manager_init(SpiceUsbDeviceManager *self)
{
    SpiceUsbDeviceManagerPrivate *priv;

    priv = SPICE_USB_DEVICE_MANAGER_GET_PRIVATE(self);
    self->priv = priv;

#if defined(G_OS_WIN32) && defined(USE_USBREDIR)
    if (usbdk_is_driver_installed()) {
        priv->usbdk_api = usbdk_api_load();
    } else {
        spice_debug("UsbDk driver is not installed");
    }
#endif
    priv->channels = g_ptr_array_new();
#ifdef USE_USBREDIR
    priv->devices  = g_ptr_array_new_with_free_func((GDestroyNotify)
                                                    spice_usb_device_unref);
#endif
}

static gboolean spice_usb_device_manager_initable_init(GInitable  *initable,
                                                       GCancellable  *cancellable,
                                                       GError        **err)
{
#ifdef USE_USBREDIR
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(initable);
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    GList *list;
    GList *it;
#ifdef USE_GUDEV
    const gchar *const subsystems[] = {"usb", NULL};
#endif

    /* Initialize spice backend */
    priv->context = spice_usb_backend_initialize();
    if (!priv->context) {
        return FALSE;
    }
    spice_usb_backend_set_device_change_callback(priv->context,
        self, on_device_change);

    /* Start listening for usb devices plug / unplug */
#ifdef USE_GUDEV
    priv->udev = g_udev_client_new(subsystems);
    if (priv->udev == NULL) {
        g_warning("Error initializing GUdevClient");
        return FALSE;
    }
    g_signal_connect(G_OBJECT(priv->udev), "uevent",
                     G_CALLBACK(spice_usb_device_manager_uevent_cb), self);
    /* Do coldplug (detection of already connected devices) */
    priv->coldplug_list = spice_usb_backend_get_device_list(priv->context);
    list = g_udev_client_query_by_subsystem(priv->udev, "usb");
    for (it = g_list_first(list); it; it = g_list_next(it)) {
        spice_usb_device_manager_add_udev(self, it->data);
        g_object_unref(it->data);
    }
    g_list_free(list);
    spice_usb_backend_free_device_list(priv->coldplug_list);
    priv->coldplug_list = NULL;
#else
    if (!spice_usb_backend_handle_hotplug(priv->context,
        self, spice_usb_device_manager_hotplug_cb)) {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
            "Error initializing USB hotplug support");
        return FALSE;
    }
    spice_usb_device_manager_start_event_listening(self, NULL);
#endif

    /* Start listening for usb channels connect/disconnect */
    spice_g_signal_connect_object(priv->session, "channel-new", G_CALLBACK(channel_new), self, G_CONNECT_AFTER);
    g_signal_connect(priv->session, "channel-destroy",
                     G_CALLBACK(channel_destroy), self);
    list = spice_session_get_channels(priv->session);
    for (it = g_list_first(list); it != NULL; it = g_list_next(it)) {
        channel_new(priv->session, it->data, (gpointer*)self);
    }
    g_list_free(list);

    return TRUE;
#else
    g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                        _("USB redirection support not compiled in"));
    return FALSE;
#endif
}

static void spice_usb_device_manager_dispose(GObject *gobject)
{
#ifdef USE_USBREDIR
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(gobject);
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

#ifdef USE_LIBUSB_HOTPLUG
    // TODO: check in case the initial spice_usb_backend_handle_hotplug fails

    spice_usb_device_manager_stop_event_listening(self);
    if (g_atomic_int_get(&priv->event_thread_run)) {
        /* Force termination of the event thread even if there were some
            * mismatched spice_usb_device_manager_{start,stop}_event_listening
            * calls. Otherwise, the usb event thread will be leaked, and will
            * try to use the libusb context we destroy in finalize(), which would
            * cause a crash */
            g_warn_if_reached();
            g_atomic_int_set(&priv->event_thread_run, FALSE);

    /* This also wakes up the libusb_handle_events() in the event_thread */
    spice_usb_backend_handle_hotplug(priv->context, NULL, NULL);
    }
#endif
    if (priv->event_thread) {
        g_warn_if_fail(g_atomic_int_get(&priv->event_thread_run) == FALSE);
        g_thread_join(priv->event_thread);
        priv->event_thread = NULL;
    }
#endif

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_usb_device_manager_parent_class)->dispose)
        G_OBJECT_CLASS(spice_usb_device_manager_parent_class)->dispose(gobject);
}

static void spice_usb_device_manager_finalize(GObject *gobject)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(gobject);
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

    g_ptr_array_unref(priv->channels);
    if (priv->devices)
        g_ptr_array_unref(priv->devices);

#ifdef USE_USBREDIR
#ifdef USE_GUDEV
    g_clear_object(&priv->udev);
#endif
    g_return_if_fail(priv->event_thread == NULL);
    if (priv->context) {
        spice_usb_backend_finalize(priv->context);
    }
    free(priv->auto_conn_filter_rules);
    free(priv->redirect_on_connect_rules);
#ifdef G_OS_WIN32
    _usbdk_hider_clear(self);
    usbdk_api_unload(priv->usbdk_api);
#endif
#endif

    g_free(priv->auto_connect_filter);
    g_free(priv->redirect_on_connect);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_usb_device_manager_parent_class)->finalize)
        G_OBJECT_CLASS(spice_usb_device_manager_parent_class)->finalize(gobject);
}

static void spice_usb_device_manager_initable_iface_init(GInitableIface *iface)
{
    iface->init = spice_usb_device_manager_initable_init;
}

static void spice_usb_device_manager_get_property(GObject     *gobject,
                                                  guint        prop_id,
                                                  GValue      *value,
                                                  GParamSpec  *pspec)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(gobject);
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        g_value_set_object(value, priv->session);
        break;
    case PROP_AUTO_CONNECT:
        g_value_set_boolean(value, priv->auto_connect);
        break;
    case PROP_AUTO_CONNECT_FILTER:
        g_value_set_string(value, priv->auto_connect_filter);
        break;
    case PROP_REDIRECT_ON_CONNECT:
        g_value_set_string(value, priv->redirect_on_connect);
        break;
    case PROP_SHARE_CD:
        /* get_property is not needed */
        g_value_set_string(value, "");
        break;
    case PROP_FREE_CHANNELS: {
        int free_channels = 0;
#ifdef USE_USBREDIR
        int i;
        for (i = 0; i < priv->channels->len; i++) {
            SpiceUsbredirChannel *channel = g_ptr_array_index(priv->channels, i);

            if (!spice_usbredir_channel_get_device(channel))
                free_channels++;
        }
#endif
        g_value_set_int(value, free_channels);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_usb_device_manager_set_property(GObject       *gobject,
                                                  guint          prop_id,
                                                  const GValue  *value,
                                                  GParamSpec    *pspec)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(gobject);
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        priv->session = g_value_get_object(value);
        break;
    case PROP_AUTO_CONNECT:
        priv->auto_connect = g_value_get_boolean(value);
#if defined(G_OS_WIN32) && defined(USE_USBREDIR)
        _usbdk_hider_update(self);
#endif
        break;
    case PROP_AUTO_CONNECT_FILTER: {
        const gchar *filter = g_value_get_string(value);
#ifdef USE_USBREDIR
        struct usbredirfilter_rule *rules;
        int r, count;

        r = usbredirfilter_string_to_rules(filter, ",", "|", &rules, &count);
        if (r) {
            if (r == -ENOMEM)
                g_error("Failed to allocate memory for auto-connect-filter");
            g_warning("Error parsing auto-connect-filter string, keeping old filter");
            break;
        }

        SPICE_DEBUG("auto-connect filter set to %s", filter);
        free(priv->auto_conn_filter_rules);
        priv->auto_conn_filter_rules = rules;
        priv->auto_conn_filter_rules_count = count;
#endif
        g_free(priv->auto_connect_filter);
        priv->auto_connect_filter = g_strdup(filter);

#if defined(G_OS_WIN32) && defined(USE_USBREDIR)
        _usbdk_hider_update(self);
#endif
        break;
    }
    case PROP_REDIRECT_ON_CONNECT: {
        const gchar *filter = g_value_get_string(value);
#ifdef USE_USBREDIR
        struct usbredirfilter_rule *rules = NULL;
        int r = 0, count = 0;

        if (filter)
            r = usbredirfilter_string_to_rules(filter, ",", "|",
                                               &rules, &count);
        if (r) {
            if (r == -ENOMEM)
                g_error("Failed to allocate memory for redirect-on-connect");
            g_warning("Error parsing redirect-on-connect string, keeping old filter");
            break;
        }

        SPICE_DEBUG("redirect-on-connect filter set to %s", filter);
        free(priv->redirect_on_connect_rules);
        priv->redirect_on_connect_rules = rules;
        priv->redirect_on_connect_rules_count = count;
#endif
        g_free(priv->redirect_on_connect);
        priv->redirect_on_connect = g_strdup(filter);
        break;
    }
    case PROP_SHARE_CD:
    {
#ifdef USE_USBREDIR
        SpiceUsbDeviceLunInfo info = { 0 };
        const gchar *name = g_value_get_string(value);
        /* the string is temporary, no need to keep it */
        SPICE_DEBUG("share_cd set to %s", name);
        info.started = TRUE;
        info.loaded = TRUE;
        info.file_path = name;
        spice_usb_backend_add_cd_lun(priv->context, &info);
#endif
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_usb_device_manager_class_init(SpiceUsbDeviceManagerClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GParamSpec *pspec;

    gobject_class->dispose      = spice_usb_device_manager_dispose;
    gobject_class->finalize     = spice_usb_device_manager_finalize;
    gobject_class->get_property = spice_usb_device_manager_get_property;
    gobject_class->set_property = spice_usb_device_manager_set_property;

    /**
     * SpiceUsbDeviceManager:session:
     *
     * #SpiceSession this #SpiceUsbDeviceManager is associated with
     *
     **/
    g_object_class_install_property
        (gobject_class, PROP_SESSION,
         g_param_spec_object("session",
                             "Session",
                             "SpiceSession",
                             SPICE_TYPE_SESSION,
                             G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                             G_PARAM_STATIC_STRINGS));

    /**
     * SpiceUsbDeviceManager:auto-connect:
     *
     * Set this to TRUE to automatically redirect newly plugged in device.
     *
     * Note when #SpiceGtkSession's auto-usbredir property is TRUE, this
     * property is controlled by #SpiceGtkSession.
     */
    pspec = g_param_spec_boolean("auto-connect", "Auto Connect",
                                 "Auto connect plugged in USB devices",
                                 FALSE,
                                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_AUTO_CONNECT, pspec);

    /**
     * SpiceUsbDeviceManager:auto-connect-filter:
     *
     * Set a string specifying a filter to use to determine which USB devices
     * to autoconnect when plugged in, a filter consists of one or more rules.
     * Where each rule has the form of:
     *
     * @class,@vendor,@product,@version,@allow
     *
     * Use -1 for @class/@vendor/@product/@version to accept any value.
     *
     * And the rules themselves are concatenated like this:
     *
     * @rule1|@rule2|@rule3
     *
     * The default setting filters out HID (class 0x03) USB devices from auto
     * connect and auto connects anything else. Note the explicit allow rule at
     * the end, this is necessary since by default all devices without a
     * matching filter rule will not auto-connect.
     *
     * Filter strings in this format can be easily created with the RHEV-M
     * USB filter editor tool.
     */
    pspec = g_param_spec_string("auto-connect-filter", "Auto Connect Filter ",
               "Filter determining which USB devices to auto connect",
               "0x03,-1,-1,-1,0|-1,-1,-1,-1,1",
               G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_AUTO_CONNECT_FILTER,
                                    pspec);

    /**
     * SpiceUsbDeviceManager:redirect-on-connect:
     *
     * Set a string specifying a filter selecting USB devices to automatically
     * redirect after a Spice connection has been established.
     *
     * See #SpiceUsbDeviceManager:auto-connect-filter for the filter string
     * format.
     */
    pspec = g_param_spec_string("redirect-on-connect", "Redirect on connect",
               "Filter selecting USB devices to redirect on connect", NULL,
               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_REDIRECT_ON_CONNECT,
                                    pspec);

    /**
    * SpiceUsbDeviceManager:cd-share-on-connect:
    *
    * Set a string specifying a filename or wildcard of ISO files to
    * share via CD sharing after a Spice connection has been established.
    *
    */
    pspec = g_param_spec_string("cd-share", "Share ISO file as CD",
        "File nameto share", NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_SHARE_CD, pspec);

    /**
     * SpiceUsbDeviceManager:free-channels:
     *
     * Get the number of available channels for redirecting USB devices.
     *
     * Since: 0.31
     */
    pspec = g_param_spec_int("free-channels", "Free channels",
               "The number of available channels for redirecting USB devices",
               0,
               G_MAXINT,
               0,
               G_PARAM_READABLE);
    g_object_class_install_property(gobject_class, PROP_FREE_CHANNELS,
                                    pspec);

    /**
     * SpiceUsbDeviceManager::device-added:
     * @manager: the #SpiceUsbDeviceManager that emitted the signal
     * @device: #SpiceUsbDevice boxed object corresponding to the added device
     *
     * The #SpiceUsbDeviceManager::device-added signal is emitted whenever
     * a new USB device has been plugged in.
     **/
    signals[DEVICE_ADDED] =
        g_signal_new("device-added",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceUsbDeviceManagerClass, device_added),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__BOXED,
                     G_TYPE_NONE,
                     1,
                     SPICE_TYPE_USB_DEVICE);

    /**
     * SpiceUsbDeviceManager::device-removed:
     * @manager: the #SpiceUsbDeviceManager that emitted the signal
     * @device: #SpiceUsbDevice boxed object corresponding to the removed device
     *
     * The #SpiceUsbDeviceManager::device-removed signal is emitted whenever
     * an USB device has been removed.
     **/
    signals[DEVICE_REMOVED] =
        g_signal_new("device-removed",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceUsbDeviceManagerClass, device_removed),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__BOXED,
                     G_TYPE_NONE,
                     1,
                     SPICE_TYPE_USB_DEVICE);

    /**
     * SpiceUsbDeviceManager::auto-connect-failed:
     * @manager: the #SpiceUsbDeviceManager that emitted the signal
     * @device: #SpiceUsbDevice boxed object corresponding to the device which failed to auto connect
     * @error: #GError describing the reason why the autoconnect failed
     *
     * The #SpiceUsbDeviceManager::auto-connect-failed signal is emitted
     * whenever the auto-connect property is true, and a newly plugged in
     * device could not be auto-connected.
     **/
    signals[AUTO_CONNECT_FAILED] =
        g_signal_new("auto-connect-failed",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceUsbDeviceManagerClass, auto_connect_failed),
                     NULL, NULL,
                     g_cclosure_user_marshal_VOID__BOXED_BOXED,
                     G_TYPE_NONE,
                     2,
                     SPICE_TYPE_USB_DEVICE,
                     G_TYPE_ERROR);

    /**
     * SpiceUsbDeviceManager::device-error:
     * @manager: #SpiceUsbDeviceManager that emitted the signal
     * @device:  #SpiceUsbDevice boxed object corresponding to the device which has an error
     * @error:   #GError describing the error
     *
     * The #SpiceUsbDeviceManager::device-error signal is emitted whenever an
     * error happens which causes a device to no longer be available to the
     * guest.
     **/
    signals[DEVICE_ERROR] =
        g_signal_new("device-error",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceUsbDeviceManagerClass, device_error),
                     NULL, NULL,
                     g_cclosure_user_marshal_VOID__BOXED_BOXED,
                     G_TYPE_NONE,
                     2,
                     SPICE_TYPE_USB_DEVICE,
                     G_TYPE_ERROR);

    /**
    * SpiceUsbDeviceManager::device-changed:
    * @manager: #SpiceUsbDeviceManager that emitted the signal
    * @device:  #SpiceUsbDevice boxed object corresponding to the device which was changed
    *
    * The #SpiceUsbDeviceManager::device-changed signal is emitted whenever
    * the change happens with one or more logical CD units of the device.
    * Applicable only to emulated CD sharing devices
    **/
    signals[DEVICE_CHANGED] =
        g_signal_new("device-changed",
            G_OBJECT_CLASS_TYPE(gobject_class),
            G_SIGNAL_RUN_FIRST,
            G_STRUCT_OFFSET(SpiceUsbDeviceManagerClass, device_changed),
            NULL, NULL,
            g_cclosure_marshal_VOID__BOXED,
            G_TYPE_NONE,
            1,
            SPICE_TYPE_USB_DEVICE);

    g_type_class_add_private(klass, sizeof(SpiceUsbDeviceManagerPrivate));
}

#ifdef USE_USBREDIR

/* ------------------------------------------------------------------ */
/* gudev / backend Helper functions                                    */

#ifdef USE_GUDEV
static gboolean spice_usb_device_manager_get_udev_bus_n_address(
    SpiceUsbDeviceManager *manager, GUdevDevice *udev,
    int *bus, int *address)
{
    const gchar *bus_str, *address_str;

    *bus = *address = 0;

   /* Linux or UsbDk backend on Windows*/
    bus_str = g_udev_device_get_property(udev, "BUSNUM");
    address_str = g_udev_device_get_property(udev, "DEVNUM");

    if (bus_str)
        *bus = atoi(bus_str);
    if (address_str)
        *address = atoi(address_str);

    return *bus && *address;
}
#endif

#endif // USE_USBREDIR

/**
 * spice_usb_device_get_libusb_device:
 * @device: #SpiceUsbDevice to get the descriptor information of
 *
 * Finds the %libusb_device associated with the @device.
 *
 * Returns: (transfer none): the %libusb_device associated to %SpiceUsbDevice.
 *
 * Since: 0.27
 **/
gconstpointer
spice_usb_device_get_libusb_device(const SpiceUsbDevice *device G_GNUC_UNUSED)
{
#ifdef USE_USBREDIR
#ifndef G_OS_WIN32
    const SpiceUsbDeviceInfo *info = (const SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, FALSE);

    return spice_usb_backend_device_get_libdev(info->bdev);
#endif
#endif
    return NULL;
}

#ifdef USE_USBREDIR
/* callbacks                                                          */

static void channel_new(SpiceSession *session, SpiceChannel *channel,
                        gpointer user_data)
{
    SpiceUsbDeviceManager *self = user_data;

    if (!SPICE_IS_USBREDIR_CHANNEL(channel))
        return;

    spice_usbredir_channel_set_context(SPICE_USBREDIR_CHANNEL(channel),
                                       self->priv->context);
    spice_channel_connect(channel);
    g_ptr_array_add(self->priv->channels, channel);

    /*
     * add a reference to ourself, to make sure the backend device context is
     * alive as long as the channel is.
     * TODO: moving to gusb could help here too.
     */
    g_object_ref(self);
    g_object_weak_ref(G_OBJECT(channel), (GWeakNotify)g_object_unref, self);
}

static void channel_destroy(SpiceSession *session, SpiceChannel *channel,
                            gpointer user_data)
{
    SpiceUsbDeviceManager *self = user_data;

    if (!SPICE_IS_USBREDIR_CHANNEL(channel))
        return;

    g_ptr_array_remove(self->priv->channels, channel);
}

static void spice_usb_device_manager_auto_connect_cb(GObject      *gobject,
                                                     GAsyncResult *res,
                                                     gpointer      user_data)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(gobject);
    SpiceUsbDevice *device = user_data;
    GError *err = NULL;

    spice_usb_device_manager_connect_device_finish(self, res, &err);
    if (err) {
        gchar *desc = spice_usb_device_get_description(device, NULL);
        g_prefix_error(&err, "Could not auto-redirect %s: ", desc);
        g_free(desc);

        SPICE_DEBUG("%s", err->message);
        g_signal_emit(self, signals[AUTO_CONNECT_FAILED], 0, device, err);
        g_error_free(err);
    }
    spice_usb_device_unref(device);
}

static gboolean
spice_usb_device_manager_device_match(SpiceUsbDeviceManager *self, SpiceUsbDevice *device,
                                      const int bus, const int address)
{
    return (spice_usb_device_get_busnum(device) == bus &&
            spice_usb_device_get_devaddr(device) == address);
}

#ifdef USE_GUDEV
static gboolean
spice_usb_device_manager_bdev_match(SpiceUsbDeviceManager *self, SpiceUsbBackendDevice *dev,
                                      const int bus, const int address)
{
    const UsbDeviceInformation* info = spice_usb_backend_device_get_info(dev);
    /* match functions for Linux/UsbDk -- match by bus.addr */
    return (info->bus == bus && info->address == address);
}
#endif

static SpiceUsbDevice*
spice_usb_device_manager_find_device(SpiceUsbDeviceManager *self,
                                     const int bus, const int address)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    SpiceUsbDevice *curr, *device = NULL;
    guint i;

    for (i = 0; i < priv->devices->len; i++) {
        curr = g_ptr_array_index(priv->devices, i);
        if (spice_usb_device_manager_device_match(self, curr, bus, address)) {
            device = curr;
            break;
        }
    }
    return device;
}

static void spice_usb_device_manager_add_dev(
    SpiceUsbDeviceManager  *self,
    SpiceUsbBackendDevice          *bdev)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    SpiceUsbDevice *device;

    /* Skip hubs */
    if (spice_usb_backend_device_is_hub(bdev))
        return;

    device = (SpiceUsbDevice*)spice_usb_device_new(bdev);
    if (!device)
        return;

    g_ptr_array_add(priv->devices, device);

    if (priv->auto_connect) {
        gboolean can_redirect, auto_ok;

        can_redirect = spice_usb_device_manager_can_redirect_device(
                                        self, device, NULL);

        auto_ok = spice_usb_backend_device_check_filter(
            bdev,
            priv->auto_conn_filter_rules,
            priv->auto_conn_filter_rules_count);

        if (can_redirect && auto_ok)
            spice_usb_device_manager_connect_device_async(self,
                                   device, NULL,
                                   spice_usb_device_manager_auto_connect_cb,
                                   spice_usb_device_ref(device));
    }

    SPICE_DEBUG("device added %04x:%04x (%p)",
                spice_usb_device_get_vid(device),
                spice_usb_device_get_pid(device),
                device);
    g_signal_emit(self, signals[DEVICE_ADDED], 0, device);
}

static void spice_usb_device_manager_remove_dev(SpiceUsbDeviceManager *self,
                                                guint bus, guint address)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    SpiceUsbDevice *device;

    device = spice_usb_device_manager_find_device(self, bus, address);
    if (!device) {
        g_warning("Could not find USB device to remove " DEV_ID_FMT,
                  bus, address);
        return;
    }

    /* TODO: check usage of the sync version is ok here */
    disconnect_device_sync(self, device);

    SPICE_DEBUG("device removed %04x:%04x (%p)",
                spice_usb_device_get_vid(device),
                spice_usb_device_get_pid(device),
                device);
    spice_usb_device_ref(device);
    g_ptr_array_remove(priv->devices, device);
    g_signal_emit(self, signals[DEVICE_REMOVED], 0, device);
    spice_usb_device_unref(device);
}

#ifdef USE_GUDEV
static void spice_usb_device_manager_add_udev(SpiceUsbDeviceManager  *self,
                                              GUdevDevice            *udev)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    SpiceUsbBackendDevice *devarrived = NULL, **dev_list = NULL;
    SpiceUsbDevice *device;
    const gchar *devtype;
    int i, bus, address;

    devtype = g_udev_device_get_property(udev, "DEVTYPE");
    /* Check if this is a usb device (and not an interface) */
    if (!devtype || strcmp(devtype, "usb_device"))
        return;

    if (!spice_usb_device_manager_get_udev_bus_n_address(self, udev, &bus, &address)) {
        g_warning("USB device without bus number or device address");
        return;
    }

    device = spice_usb_device_manager_find_device(self, bus, address);
    if (device) {
        SPICE_DEBUG("USB device 0x%04x:0x%04x at %d.%d already exists, ignored",
                    spice_usb_device_get_vid(device),
                    spice_usb_device_get_pid(device),
                    spice_usb_device_get_busnum(device),
                    spice_usb_device_get_devaddr(device));
        return;
    }

    if (priv->coldplug_list)
        dev_list = priv->coldplug_list;
    else
        dev_list = spice_usb_backend_get_device_list(priv->context);

    for (i = 0; dev_list && dev_list[i]; i++) {
        if (spice_usb_device_manager_bdev_match(self, dev_list[i], bus, address)) {
            devarrived = dev_list[i];
            break;
        }
    }

    if (devarrived)
        spice_usb_device_manager_add_dev(self, devarrived);
    else
        g_warning("Could not find USB device to add " DEV_ID_FMT,
                  (guint) bus, (guint) address);

    if (!priv->coldplug_list)
        spice_usb_backend_free_device_list(dev_list);
}

static void spice_usb_device_manager_remove_udev(SpiceUsbDeviceManager  *self,
                                                 GUdevDevice            *udev)
{
    int bus, address;

    if (!spice_usb_device_manager_get_udev_bus_n_address(self, udev, &bus, &address))
        return;

    spice_usb_device_manager_remove_dev(self, bus, address);
}

static void spice_usb_device_manager_uevent_cb(GUdevClient     *client,
                                               const gchar     *action,
                                               GUdevDevice     *udevice,
                                               gpointer         user_data)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(user_data);

    if (g_str_equal(action, "add"))
        spice_usb_device_manager_add_udev(self, udevice);
    else if (g_str_equal (action, "remove"))
        spice_usb_device_manager_remove_udev(self, udevice);
}
#else
struct hotplug_idle_cb_args {
    SpiceUsbDeviceManager *self;
    SpiceUsbBackendDevice *device;
    gboolean device_added;
};

static gboolean spice_usb_device_manager_hotplug_idle_cb(gpointer user_data)
{
    struct hotplug_idle_cb_args *args = user_data;
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(args->self);

    if (args->device_added) {
        spice_usb_device_manager_add_dev(self, args->device);
    } else {
        const UsbDeviceInformation *info = spice_usb_backend_device_get_info(args->device);
        spice_usb_device_manager_remove_dev(self,
            info->bus,
            info->address);
    }
    spice_usb_backend_device_release(args->device);
    g_object_unref(self);
    g_free(args);
    return FALSE;
}

/* Can be called from both the main-thread as well as the event_thread */
static void spice_usb_device_manager_hotplug_cb(
    void *user_data,
    SpiceUsbBackendDevice *bdev,
    gboolean added)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(user_data);
    struct hotplug_idle_cb_args *args = g_malloc0(sizeof(*args));

    args->self = g_object_ref(self);
    spice_usb_backend_device_acquire(bdev);
    args->device_added = added;
    args->device = bdev;
    g_idle_add(spice_usb_device_manager_hotplug_idle_cb, args);
}
#endif // USE_USBREDIR

static void spice_usb_device_manager_channel_connect_cb(
    GObject *gobject, GAsyncResult *channel_res, gpointer user_data)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(gobject);
    GTask *task = G_TASK(user_data);
    GError *err = NULL;

    spice_usbredir_channel_connect_device_finish(channel, channel_res, &err);
    if (err)
        g_task_return_error(task, err);
    else
        g_task_return_boolean(task, TRUE);

    g_object_unref(task);
}

/* ------------------------------------------------------------------ */
/* private api                                                        */

static gpointer spice_usb_device_manager_usb_ev_thread(gpointer user_data)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(user_data);
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

    while (g_atomic_int_get(&priv->event_thread_run)) {
        if (!spice_usb_backend_handle_events(priv->context)) {
            break;
        }
    }

    return NULL;
}

gboolean spice_usb_device_manager_start_event_listening(
    SpiceUsbDeviceManager *self, GError **err)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

    g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

    priv->event_listeners++;
    if (priv->event_listeners > 1)
        return TRUE;

    /* We don't join the thread when we stop event listening, as the
       libusb_handle_events call in the thread won't exit until the
       libusb_close call for the device is made from usbredirhost_close. */
    if (priv->event_thread) {
         g_thread_join(priv->event_thread);
         priv->event_thread = NULL;
    }
    g_atomic_int_set(&priv->event_thread_run, TRUE);
    priv->event_thread = g_thread_new("usb_ev_thread",
                                      spice_usb_device_manager_usb_ev_thread,
                                      self);
    return priv->event_thread != NULL;
}

void spice_usb_device_manager_stop_event_listening(
    SpiceUsbDeviceManager *self)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;

    g_return_if_fail(priv->event_listeners > 0);

    priv->event_listeners--;
    if (priv->event_listeners == 0)
        g_atomic_int_set(&priv->event_thread_run, FALSE);
}

void spice_usb_device_manager_check_redir_on_connect(
    SpiceUsbDeviceManager *self, SpiceChannel *channel)
{
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    GTask *task;
    SpiceUsbDevice *device;
    SpiceUsbBackendDevice *dev;
    guint i;

    if (priv->redirect_on_connect == NULL)
        return;

    for (i = 0; i < priv->devices->len; i++) {
        device = g_ptr_array_index(priv->devices, i);

        if (spice_usb_device_manager_is_device_connected(self, device))
            continue;

        dev = spice_usb_device_manager_device_to_bdev(self, device);
#ifdef G_OS_WIN32
        if (dev == NULL)
            continue;
#endif
        if (spice_usb_backend_device_check_filter(
            dev,
            priv->redirect_on_connect_rules,
            priv->redirect_on_connect_rules_count) == 0) {
            /* Note: re-uses spice_usb_device_manager_connect_device_async's
               completion handling code! */
            task = g_task_new(self,
                              NULL,
                              spice_usb_device_manager_auto_connect_cb,
                              spice_usb_device_ref(device));

            spice_usbredir_channel_connect_device_async(
                               SPICE_USBREDIR_CHANNEL(channel),
                               dev, device, NULL,
                               spice_usb_device_manager_channel_connect_cb,
                               task);
            spice_usb_backend_device_release(dev);
            return; /* We've taken the channel! */
        }

        spice_usb_backend_device_release(dev);
    }
}

void spice_usb_device_manager_device_error(
    SpiceUsbDeviceManager *self, SpiceUsbDevice *device, GError *err)
{
    g_return_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self));
    g_return_if_fail(device != NULL);

    g_signal_emit(self, signals[DEVICE_ERROR], 0, device, err);
}
#endif

static SpiceUsbredirChannel *spice_usb_device_manager_get_channel_for_dev(
    SpiceUsbDeviceManager *manager, SpiceUsbDevice *device)
{
#ifdef USE_USBREDIR
    SpiceUsbDeviceManagerPrivate *priv = manager->priv;
    guint i;

    for (i = 0; i < priv->channels->len; i++) {
        SpiceUsbredirChannel *channel = g_ptr_array_index(priv->channels, i);
        spice_usbredir_channel_lock(channel);
        SpiceUsbBackendDevice *dev = spice_usbredir_channel_get_device(channel);
        if (spice_usb_manager_device_equal_bdev(manager, device, dev)) {
            spice_usbredir_channel_unlock(channel);
            return channel;
        }
        spice_usbredir_channel_unlock(channel);
    }
#endif
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public api                                                         */

/**
 * spice_usb_device_manager_get_devices_with_filter:
 * @manager: the #SpiceUsbDeviceManager manager
 * @filter: (allow-none): filter string for selecting which devices to return,
 *      see #SpiceUsbDeviceManager:auto-connect-filter for the filter
 *      string format
 *
 * Finds devices associated with the @manager complying with the @filter
 *
 * Returns: (element-type SpiceUsbDevice) (transfer full): a
 * %GPtrArray array of %SpiceUsbDevice
 *
 * Since: 0.20
 */
GPtrArray* spice_usb_device_manager_get_devices_with_filter(
    SpiceUsbDeviceManager *self, const gchar *filter)
{
    GPtrArray *devices_copy = NULL;

    g_return_val_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self), NULL);

#ifdef USE_USBREDIR
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    struct usbredirfilter_rule *rules = NULL;
    int r, count = 0;
    guint i;

    if (filter) {
        r = usbredirfilter_string_to_rules(filter, ",", "|", &rules, &count);
        if (r) {
            if (r == -ENOMEM)
                g_error("Failed to allocate memory for filter");
            g_warning("Error parsing filter, ignoring");
            rules = NULL;
            count = 0;
        }
    }

    devices_copy = g_ptr_array_new_with_free_func((GDestroyNotify)
                                                  spice_usb_device_unref);
    for (i = 0; i < priv->devices->len; i++) {
        SpiceUsbDevice *device = g_ptr_array_index(priv->devices, i);

        if (rules) {
            SpiceUsbBackendDevice *bdev =
                spice_usb_device_manager_device_to_bdev(self, device);
#ifdef G_OS_WIN32
            if (bdev == NULL)
                continue;
#endif
            if (spice_usb_backend_device_check_filter(bdev, rules, count) != 0)
                continue;
        }
        g_ptr_array_add(devices_copy, spice_usb_device_ref(device));
    }

    free(rules);
#endif

    return devices_copy;
}

/**
 * spice_usb_device_manager_get_devices:
 * @manager: the #SpiceUsbDeviceManager manager
 *
 * Finds devices associated with the @manager
 *
 * Returns: (element-type SpiceUsbDevice) (transfer full): a %GPtrArray array of %SpiceUsbDevice
 */
GPtrArray* spice_usb_device_manager_get_devices(SpiceUsbDeviceManager *self)
{
    return spice_usb_device_manager_get_devices_with_filter(self, NULL);
}

/**
 * spice_usb_device_manager_is_device_connected:
 * @manager: the #SpiceUsbDeviceManager manager
 * @device: a #SpiceUsbDevice
 *
 * Finds if the @device is connected.
 *
 * Returns: %TRUE if @device has an associated USB redirection channel
 */
gboolean spice_usb_device_manager_is_device_connected(SpiceUsbDeviceManager *self,
                                                      SpiceUsbDevice *device)
{
    g_return_val_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self), FALSE);
    g_return_val_if_fail(device != NULL, FALSE);

    return !!spice_usb_device_manager_get_channel_for_dev(self, device);
}

#ifdef USE_USBREDIR

static gboolean
_spice_usb_device_manager_connect_device_finish(SpiceUsbDeviceManager *self,
                                                GAsyncResult *res,
                                                GError **error)
{
    GTask *task = G_TASK(res);

    g_return_val_if_fail(g_task_is_valid(task, G_OBJECT(self)), FALSE);

    return g_task_propagate_boolean(task, error);
}

static void
_spice_usb_device_manager_connect_device_async(SpiceUsbDeviceManager *self,
                                               SpiceUsbDevice *device,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data)
{
    GTask *task;

    g_return_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self));
    g_return_if_fail(device != NULL);

    SPICE_DEBUG("connecting device %p", device);

    task = g_task_new(self, cancellable, callback, user_data);

    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    SpiceUsbBackendDevice *bdev;
    guint i;

    if (spice_usb_device_manager_is_device_connected(self, device)) {
        g_task_return_new_error(task,
                            SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "Cannot connect an already connected usb device");
        goto done;
    }

    for (i = 0; i < priv->channels->len; i++) {
        SpiceUsbredirChannel *channel = g_ptr_array_index(priv->channels, i);

        if (spice_usbredir_channel_get_device(channel))
            continue; /* Skip already used channels */

        bdev = spice_usb_device_manager_device_to_bdev(self, device);
#ifdef G_OS_WIN32
        if (bdev == NULL) {
            /* Most likely, the device was plugged out at driver installation
             * time, and its remove-device event was ignored.
             * So remove the device now
             */
            SPICE_DEBUG("libdev does not exist for %p -- removing", device);
            spice_usb_device_ref(device);
            g_ptr_array_remove(priv->devices, device);
            g_signal_emit(self, signals[DEVICE_REMOVED], 0, device);
            spice_usb_device_unref(device);
            g_task_return_new_error(task,
                                    SPICE_CLIENT_ERROR,
                                    SPICE_CLIENT_ERROR_FAILED,
                                    _("Device was not found"));
            goto done;
        }
#endif
        spice_usbredir_channel_connect_device_async(channel,
                                 bdev,
                                 device,
                                 cancellable,
                                 spice_usb_device_manager_channel_connect_cb,
                                 task);
        spice_usb_backend_device_release(bdev);
        return;
    }

    g_task_return_new_error(task,
                            SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            _("No free USB channel"));
done:
    g_object_unref(task);
}

#endif

/**
 * spice_usb_device_manager_connect_device_async:
 * @self: a #SpiceUsbDeviceManager.
 * @device: a #SpiceUsbDevice to redirect
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to ignore
 * @callback: a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 *
 * Asynchronously connects the @device. When completed, @callback will be called.
 * Then it is possible to call spice_usb_device_manager_connect_device_finish()
 * to get the result of the operation.
 */
void spice_usb_device_manager_connect_device_async(SpiceUsbDeviceManager *self,
                                             SpiceUsbDevice *device,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
    g_return_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self));

#ifdef USE_USBREDIR

    GTask *task =
        g_task_new(G_OBJECT(self), cancellable, callback, user_data);

    _set_redirecting(self, TRUE);
    _spice_usb_device_manager_connect_device_async(self,
                                                   device,
                                                   cancellable,
                                                   _connect_device_async_cb,
                                                   task);
#endif
}

/**
 * spice_usb_device_manager_connect_device_finish:
 * @self: a #SpiceUsbDeviceManager.
 * @res: a #GAsyncResult
 * @err: (allow-none): a return location for a #GError, or %NULL.
 *
 * Finishes an async operation. See spice_usb_device_manager_connect_device_async().
 *
 * Returns: %TRUE if connection is successful
 */
gboolean spice_usb_device_manager_connect_device_finish(
    SpiceUsbDeviceManager *self, GAsyncResult *res, GError **err)
{
    GTask *task = G_TASK(res);

    g_return_val_if_fail(g_task_is_valid(task, self),
                         FALSE);

    return g_task_propagate_boolean(task, err);
}

/**
 * spice_usb_device_manager_disconnect_device_finish:
 * @self: a #SpiceUsbDeviceManager.
 * @res: a #GAsyncResult
 * @err: (allow-none): a return location for a #GError, or %NULL.
 *
 * Finishes an async operation. See spice_usb_device_manager_disconnect_device_async().
 *
 * Returns: %TRUE if disconnection is successful
 */
gboolean spice_usb_device_manager_disconnect_device_finish(
    SpiceUsbDeviceManager *self, GAsyncResult *res, GError **err)
{
    GTask *task = G_TASK(res);

    g_return_val_if_fail(g_task_is_valid(task, G_OBJECT(self)), FALSE);

    return g_task_propagate_boolean(task, err);
}

#ifdef USE_USBREDIR
static
void _connect_device_async_cb(GObject *gobject,
                              GAsyncResult *channel_res,
                              gpointer user_data)
{
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(gobject);
    GTask *task = user_data;
    GError *error = NULL;

    _set_redirecting(self, FALSE);
    if (_spice_usb_device_manager_connect_device_finish(self, channel_res, &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
    g_object_unref(task);
}
#endif

static void disconnect_device_sync(SpiceUsbDeviceManager *self,
                                   SpiceUsbDevice *device)
{
    g_return_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self));
    g_return_if_fail(device != NULL);

    SPICE_DEBUG("disconnecting device %p", device);

#ifdef USE_USBREDIR
    SpiceUsbredirChannel *channel;

    channel = spice_usb_device_manager_get_channel_for_dev(self, device);
    if (channel)
        spice_usbredir_channel_disconnect_device(channel);

#endif
}

/**
 * spice_usb_device_manager_disconnect_device:
 * @manager: the #SpiceUsbDeviceManager manager
 * @device: a #SpiceUsbDevice to disconnect
 *
 * Disconnects the @device.
 */
void spice_usb_device_manager_disconnect_device(SpiceUsbDeviceManager *self,
                                                SpiceUsbDevice *device)
{
    disconnect_device_sync(self, device);
}

typedef struct _disconnect_cb_data
{
    SpiceUsbDeviceManager  *self;
    SpiceUsbDevice         *device;
} disconnect_cb_data;

#ifdef USE_USBREDIR
static
void _disconnect_device_async_cb(GObject *gobject,
                                 GAsyncResult *channel_res,
                                 gpointer user_data)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(gobject);
    GTask *task = user_data;
    GError *err = NULL;
    disconnect_cb_data *data = g_task_get_task_data(task);
    SpiceUsbDeviceManager *self = SPICE_USB_DEVICE_MANAGER(data->self);

    _set_redirecting(self, FALSE);

    spice_usbredir_channel_disconnect_device_finish(channel, channel_res, &err);
    if (err)
        g_task_return_error(task, err);
    else
        g_task_return_boolean(task, TRUE);

    g_object_unref(task);
}
#endif

/**
 * spice_usb_device_manager_disconnect_device_async:
 * @self: the #SpiceUsbDeviceManager manager.
 * @device: a connected #SpiceUsbDevice to disconnect.
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to @callback.
 *
 * Asynchronously disconnects the @device. When completed, @callback will be called.
 * Then it is possible to call spice_usb_device_manager_disconnect_device_finish()
 * to get the result of the operation.
 *
 * Since: 0.32
 */
void spice_usb_device_manager_disconnect_device_async(SpiceUsbDeviceManager *self,
                                                      SpiceUsbDevice *device,
                                                      GCancellable *cancellable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer user_data)
{
#ifdef USE_USBREDIR
    GTask *nested;
    g_return_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self));

    g_return_if_fail(device != NULL);
    g_return_if_fail(spice_usb_device_manager_is_device_connected(self, device));

    SPICE_DEBUG("disconnecting device %p", device);

    SpiceUsbredirChannel *channel;

    _set_redirecting(self, TRUE);

    channel = spice_usb_device_manager_get_channel_for_dev(self, device);
    nested  = g_task_new(G_OBJECT(self), cancellable, callback, user_data);
    disconnect_cb_data *data = g_new(disconnect_cb_data, 1);
    data->self = self;
    data->device = device;
    g_task_set_task_data(nested, data, g_free);

    spice_usbredir_channel_disconnect_device_async(channel, cancellable,
                                                   _disconnect_device_async_cb,
                                                   nested);
#endif
}

/**
 * spice_usb_device_manager_can_redirect_device:
 * @self: the #SpiceUsbDeviceManager manager
 * @device: a #SpiceUsbDevice to disconnect
 * @err: (allow-none): a return location for a #GError, or %NULL.
 *
 * Checks whether it is possible to redirect the @device.
 *
 * Returns: %TRUE if @device can be redirected
 */
gboolean
spice_usb_device_manager_can_redirect_device(SpiceUsbDeviceManager  *self,
                                             SpiceUsbDevice         *device,
                                             GError                **err)
{
#ifdef USE_USBREDIR
    const struct usbredirfilter_rule *guest_filter_rules = NULL;
    SpiceUsbDeviceManagerPrivate *priv = self->priv;
    int i, guest_filter_rules_count;

    g_return_val_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self), FALSE);
    g_return_val_if_fail(device != NULL, FALSE);
    g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

    if (!spice_session_get_usbredir_enabled(priv->session)) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            _("USB redirection is disabled"));
        return FALSE;
    }

    if (!priv->channels->len) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            _("The connected VM is not configured for USB redirection"));
        return FALSE;
    }

    /* Skip the other checks for already connected devices */
    if (spice_usb_device_manager_is_device_connected(self, device))
        return TRUE;

    /* We assume all channels have the same filter, so we just take the
       filter from the first channel */
    spice_usbredir_channel_get_guest_filter(
        g_ptr_array_index(priv->channels, 0),
        &guest_filter_rules, &guest_filter_rules_count);

    if (guest_filter_rules) {
        gboolean filter_ok;
        SpiceUsbBackendDevice *bdev;

        bdev = spice_usb_device_manager_device_to_bdev(self, device);
#ifdef G_OS_WIN32
        if (bdev == NULL) {
            g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                                _("Some USB devices were not found"));
            return FALSE;
        }
#endif
        filter_ok = (spice_usb_backend_device_check_filter(
                            bdev,
                            guest_filter_rules, guest_filter_rules_count) == 0);
        spice_usb_backend_device_release(bdev);
        if (!filter_ok) {
            g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                                _("Some USB devices are blocked by host policy"));
            return FALSE;
        }
    }

    /* Check if there are free channels */
    for (i = 0; i < priv->channels->len; i++) {
        SpiceUsbredirChannel *channel = g_ptr_array_index(priv->channels, i);
        spice_usbredir_channel_lock(channel);

        if (!spice_usbredir_channel_get_device(channel)){
            spice_usbredir_channel_unlock(channel);
            break;
        }
        spice_usbredir_channel_unlock(channel);
    }
    if (i == priv->channels->len) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            _("There are no free USB channels"));
        return FALSE;
    }

    return TRUE;
#else
    g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                        _("USB redirection support not compiled in"));
    return FALSE;
#endif
}

/**
 * spice_usb_device_get_description:
 * @device: #SpiceUsbDevice to get the description of
 * @format: (allow-none): an optional printf() format string with
 * positional parameters
 *
 * Get a string describing the device which is suitable as a description of
 * the device for the end user. The returned string should be freed with
 * g_free() when no longer needed.
 *
 * The @format positional parameters are the following:
 * - '%%1$s' manufacturer
 * - '%%2$s' product
 * - '%%3$s' descriptor (a [vendor_id:product_id] string)
 * - '%%4$d' bus
 * - '%%5$d' address
 *
 * (the default format string is "%%s %%s %%s at %%d-%%d")
 *
 * Returns: a newly-allocated string holding the description, or %NULL if failed
 */
gchar *spice_usb_device_get_description(SpiceUsbDevice *device, const gchar *format)
{
#ifdef USE_USBREDIR
    guint16 bus, address, vid, pid;
    gchar *description, *descriptor, *manufacturer = NULL, *product = NULL;

    g_return_val_if_fail(device != NULL, NULL);

    bus     = spice_usb_device_get_busnum(device);
    address = spice_usb_device_get_devaddr(device);
    vid     = spice_usb_device_get_vid(device);
    pid     = spice_usb_device_get_pid(device);

    if ((vid > 0) && (pid > 0)) {
        descriptor = g_strdup_printf("[%04x:%04x]", vid, pid);
    } else {
        descriptor = g_strdup("");
    }

    spice_usb_util_get_device_strings(bus, address, vid, pid,
                                      &manufacturer, &product, TRUE);

    if (!format)
        format = _("%s %s %s at %d-%d");

    description = g_strdup_printf(format, manufacturer, product, descriptor, bus, address);

    g_free(manufacturer);
    g_free(descriptor);
    g_free(product);

    return description;
#else
    return NULL;
#endif
}

void spice_usb_device_get_info(SpiceUsbDeviceManager  *self,
                               SpiceUsbDevice *device,
                               SpiceUsbDeviceDescription *info)
{
#ifdef USE_USBREDIR
    g_return_if_fail(device != NULL);
    info->vendor = info->product;
    info->bus = spice_usb_device_get_busnum(device);
    info->address = spice_usb_device_get_devaddr(device);
    info->vendor_id = spice_usb_device_get_vid(device);
    info->product_id = spice_usb_device_get_pid(device);

    if (spice_usb_device_manager_is_device_cd(self, device)) {
        info->vendor = g_strdup("RedHat Inc.");
        info->product = g_strdup("Spice CD drive");
        return;
    }
    spice_usb_util_get_device_strings(info->bus, info->address,
        info->vendor_id, info->product_id, &info->vendor, &info->product, FALSE);
    if (!info->vendor) {
        info->vendor = g_strdup_printf("[%04X]", info->vendor_id);
    }
    if (!info->product) {
        info->product = g_strdup_printf("[%04X]", info->product_id);
    }
#endif
}

#ifdef USE_USBREDIR

/*
 * SpiceUsbDeviceInfo
 */
static SpiceUsbDeviceInfo *spice_usb_device_new(SpiceUsbBackendDevice *bdev)
{
    SpiceUsbDeviceInfo *info;
    const UsbDeviceInformation *devinfo;

    g_return_val_if_fail(bdev != NULL, NULL);
    devinfo = spice_usb_backend_device_get_info(bdev);

    info = g_new0(SpiceUsbDeviceInfo, 1);

    info->busnum  = devinfo->bus;
    info->devaddr = devinfo->address;
    info->vid = devinfo->vid;
    info->pid = devinfo->pid;
    info->ref = 1;
    info->isochronous = devinfo->isochronous;
#ifndef G_OS_WIN32
    info->bdev = bdev;
    spice_usb_backend_device_acquire(bdev);
#endif

    return info;
}

guint8 spice_usb_device_get_busnum(const SpiceUsbDevice *device)
{
    const SpiceUsbDeviceInfo *info = (const SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, 0);

    return info->busnum;
}

guint8 spice_usb_device_get_devaddr(const SpiceUsbDevice *device)
{
    const SpiceUsbDeviceInfo *info = (const SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, 0);

    return info->devaddr;
}

guint16 spice_usb_device_get_vid(const SpiceUsbDevice *device)
{
    const SpiceUsbDeviceInfo *info = (const SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, 0);

    return info->vid;
}

guint16 spice_usb_device_get_pid(const SpiceUsbDevice *device)
{
    const SpiceUsbDeviceInfo *info = (const SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, 0);

    return info->pid;
}

gboolean spice_usb_device_is_isochronous(const SpiceUsbDevice *device)
{
    const SpiceUsbDeviceInfo *info = (const SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, 0);

    return info->isochronous;
}

#ifdef G_OS_WIN32
static
gboolean _usbdk_hider_prepare(SpiceUsbDeviceManager *manager)
{
    SpiceUsbDeviceManagerPrivate *priv = manager->priv;

    g_return_val_if_fail(priv->usbdk_api != NULL, FALSE);

    if (priv->usbdk_hider_handle == NULL) {
        priv->usbdk_hider_handle = usbdk_create_hider_handle(priv->usbdk_api);
        if (priv->usbdk_hider_handle == NULL) {
            g_warning("Failed to instantiate UsbDk hider interface");
            return FALSE;
        }
    }

    return TRUE;
}

static
void _usbdk_hider_clear(SpiceUsbDeviceManager *manager)
{
    SpiceUsbDeviceManagerPrivate *priv = manager->priv;

    g_return_if_fail(priv->usbdk_api != NULL);

    if (priv->usbdk_hider_handle != NULL) {
        usbdk_clear_hide_rules(priv->usbdk_api, priv->usbdk_hider_handle);
        usbdk_close_hider_handle(priv->usbdk_api, priv->usbdk_hider_handle);
        priv->usbdk_hider_handle = NULL;
    }
}

static
void _usbdk_hider_update(SpiceUsbDeviceManager *manager)
{
    SpiceUsbDeviceManagerPrivate *priv = manager->priv;

    g_return_if_fail(priv->usbdk_api != NULL);

    if (priv->auto_connect_filter == NULL) {
        SPICE_DEBUG("No autoredirect rules, no hider setup needed");
        _usbdk_hider_clear(manager);
        return;
    }

    if (!spice_session_get_usbredir_enabled(priv->session)) {
        SPICE_DEBUG("USB redirection disabled, no hider setup needed");
        _usbdk_hider_clear(manager);
        return;
    }

    if (!priv->auto_connect) {
        SPICE_DEBUG("Auto-connect disabled, no hider setup needed");
        _usbdk_hider_clear(manager);
        return;
    }

    if(_usbdk_hider_prepare(manager)) {
        usbdk_api_set_hide_rules(priv->usbdk_api,
                                 priv->usbdk_hider_handle,
                                 priv->auto_connect_filter);
    }
}

#endif

static SpiceUsbDevice *spice_usb_device_ref(SpiceUsbDevice *device)
{
    SpiceUsbDeviceInfo *info = (SpiceUsbDeviceInfo *)device;

    g_return_val_if_fail(info != NULL, NULL);
    g_atomic_int_inc(&info->ref);
    return device;
}

static void spice_usb_device_unref(SpiceUsbDevice *device)
{
    gboolean ref_count_is_0;

    SpiceUsbDeviceInfo *info = (SpiceUsbDeviceInfo *)device;

    g_return_if_fail(info != NULL);

    ref_count_is_0 = g_atomic_int_dec_and_test(&info->ref);
    if (ref_count_is_0) {
#ifndef G_OS_WIN32
        spice_usb_backend_device_release(info->bdev);
#endif
        g_free(info);
    }
}

#ifndef G_OS_WIN32 /* Linux -- directly compare libdev */
static gboolean
spice_usb_manager_device_equal_bdev(SpiceUsbDeviceManager *manager,
                                      SpiceUsbDevice *device,
                                      SpiceUsbBackendDevice *bdev)
{
    SpiceUsbDeviceInfo *info = (SpiceUsbDeviceInfo *)device;

    if ((device == NULL) || (bdev == NULL))
        return FALSE;

    return spice_usb_backend_devices_same(info->bdev, bdev);
}
#else /* Windows -- compare vid:pid of device and libdev */
static gboolean
spice_usb_manager_device_equal_bdev(SpiceUsbDeviceManager *manager,
                                    SpiceUsbDevice *device,
                                    SpiceUsbBackendDevice  *bdev)
{
    int busnum, devaddr;

    if ((device == NULL) || (bdev == NULL))
        return FALSE;

    busnum = spice_usb_device_get_busnum(device);
    devaddr = spice_usb_device_get_devaddr(device);
    return spice_usb_device_manager_bdev_match(manager, bdev,
                                                 busnum, devaddr);
}
#endif

/*
 * Caller must spice_usb_backend_device_release the SpiceUsbBackendDevice returned by this function.
 * Returns a SpiceUsbBackendDevice, or NULL upon failure
 */
static SpiceUsbBackendDevice *
spice_usb_device_manager_device_to_bdev(SpiceUsbDeviceManager *self,
                                          SpiceUsbDevice *device)
{
#ifdef G_OS_WIN32
    /*
     * On win32 we need to do this the hard and slow way, by asking libusb to
     * re-enumerate all devices and then finding a matching device.
     * We cannot cache the libusb_device like we do under Linux since the
     * driver swap we do under windows invalidates the cached libdev.
     */

    SpiceUsbBackendDevice *d, **devlist;
    int i;

    g_return_val_if_fail(SPICE_IS_USB_DEVICE_MANAGER(self), NULL);
    g_return_val_if_fail(device != NULL, NULL);
    g_return_val_if_fail(self->priv != NULL, NULL);
    g_return_val_if_fail(self->priv->context != NULL, NULL);

    devlist = spice_usb_backend_get_device_list(self->priv->context);
    if (!devlist)
        return NULL;

    for (i = 0; (d = devlist[i]) != NULL; i++) {
        if (spice_usb_manager_device_equal_bdev(self, device, d)) {
            spice_usb_backend_device_acquire(d);
            break;
        }
    }

    spice_usb_backend_free_device_list(devlist);

    return d;

#else
    /* Simply return a ref to the cached libdev */
    SpiceUsbDeviceInfo *info = (SpiceUsbDeviceInfo *)device;

    spice_usb_backend_device_acquire(info->bdev);
    return info->bdev;
#endif
}

static void on_device_change(void *user_data, SpiceUsbBackendDevice *bdev)
{
    SpiceUsbDeviceManager *self = user_data;
    const UsbDeviceInformation *info = spice_usb_backend_device_get_info(bdev);
    SpiceUsbDevice *device = spice_usb_device_manager_find_device(self, info->bus, info->address);
    if (device) {
        SPICE_DEBUG("%s dev:%u:%u", __FUNCTION__, info->bus, info->address);
        g_signal_emit(self, signals[DEVICE_CHANGED], 0, device);
    } else {
        SPICE_DEBUG("%s %u:%u not found in usb device manager", __FUNCTION__, info->bus, info->address);
    }
}

guint spice_usb_device_manager_is_device_cd(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device)
{
    guint val = 0;
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        const UsbDeviceInformation *info = spice_usb_backend_device_get_info(bdev);
        val = info->max_luns;
        spice_usb_backend_device_release(bdev);
    }
    return val;
}

gboolean spice_usb_device_manager_add_cd_lun(SpiceUsbDeviceManager *self,
    SpiceUsbDeviceLunInfo *lun_info)
{
    return spice_usb_backend_add_cd_lun(self->priv->context, lun_info);
}

gboolean
spice_usb_device_manager_device_lun_remove(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device,
    guint lun)
{
    gboolean b = FALSE;
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        b = spice_usb_backend_remove_cd_lun(self->priv->context, bdev, lun);
        spice_usb_backend_device_release(bdev);
    }
    return b;
}

gboolean
spice_usb_device_manager_device_lun_get_info(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device,
    guint lun,
    SpiceUsbDeviceLunInfo *lun_info)
{
    gboolean b = FALSE;
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        b = spice_usb_backend_get_cd_lun_info(bdev, lun, lun_info);
        spice_usb_backend_device_release(bdev);
    }
    return b;
}

gboolean
spice_usb_device_manager_device_lun_load(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device,
    guint lun,
    gboolean load)
{
    gboolean b = FALSE;
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        b = spice_usb_backend_load_cd_lun(self->priv->context, bdev, lun, load);
        spice_usb_backend_device_release(bdev);
    }
    return b;
}

gboolean
spice_usb_device_manager_device_lun_change_media(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device,
    guint lun,
    const SpiceUsbDeviceLunInfo *lun_info)
{
    gboolean b = FALSE;
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        b = spice_usb_backend_change_cd_lun(self->priv->context, bdev, lun, lun_info);
        spice_usb_backend_device_release(bdev);
    }
    return b;
}

gboolean
spice_usb_device_manager_device_lun_lock(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device,
    guint lun,
    gboolean lock)
{
    gboolean b = FALSE;
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        b = spice_usb_backend_lock_cd_lun(self->priv->context, bdev, lun, lock);
        spice_usb_backend_device_release(bdev);
    }
    return b;
}

GArray *spice_usb_device_manager_get_device_luns(SpiceUsbDeviceManager *self,
    SpiceUsbDevice *device)
{
    GArray *indices = g_array_new(FALSE, TRUE, sizeof(guint));
    SpiceUsbBackendDevice *bdev = spice_usb_device_manager_device_to_bdev(self, device);
    if (bdev) {
        uint32_t value = spice_usb_backend_get_cd_luns_bitmask(bdev);
        guint i = 0;
        while (value) {
            if (value & 1) {
                g_array_append_val(indices, i);
            }
            value >>= 1;
            i++;
        }
        spice_usb_backend_device_release(bdev);
    }
    return indices;
}

#endif /* USE_USBREDIR */
