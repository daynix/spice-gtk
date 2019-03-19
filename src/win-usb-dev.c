/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2012 Red Hat, Inc.

   Red Hat Authors:
   Arnon Gilboa <agilboa@redhat.com>
   Uri Lublin   <uril@redhat.com>

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

#include <windows.h>
#include <libusb.h>
#include "win-usb-dev.h"
#include "spice-marshal.h"
#include "spice-util.h"
#include "usbutil.h"

enum {
    PROP_0,
    PROP_REDIRECTING
};

struct _GUdevClientPrivate {
    libusb_context *ctx;
    GList *udev_list;
    HWND hwnd;
    gboolean redirecting;
};

#define G_UDEV_CLIENT_WINCLASS_NAME  TEXT("G_UDEV_CLIENT")

static void g_udev_client_initable_iface_init(GInitableIface  *iface);

G_DEFINE_TYPE_WITH_CODE(GUdevClient, g_udev_client, G_TYPE_OBJECT,
                        G_ADD_PRIVATE(GUdevClient)
                        G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, g_udev_client_initable_iface_init))


typedef struct _GUdevDeviceInfo GUdevDeviceInfo;

struct _GUdevDeviceInfo {
    guint16 bus;
    guint16 addr;
    guint16 vid;
    guint16 pid;
    guint16 class;
    gchar sclass[4];
    gchar sbus[4];
    gchar saddr[4];
    gchar svid[8];
    gchar spid[8];
};

struct _GUdevDevicePrivate
{
    /* FixMe: move above fields to this structure and access them directly */
    GUdevDeviceInfo *udevinfo;
};

G_DEFINE_TYPE_WITH_PRIVATE(GUdevDevice, g_udev_device, G_TYPE_OBJECT)


enum
{
    UEVENT_SIGNAL,
    LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0, };
static GUdevClient *singleton = NULL;

static GUdevDevice *g_udev_device_new(GUdevDeviceInfo *udevinfo);
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static gboolean get_usb_dev_info(libusb_device *dev, GUdevDeviceInfo *udevinfo);

//uncomment to debug gudev device lists.
//#define DEBUG_GUDEV_DEVICE_LISTS

#ifdef DEBUG_GUDEV_DEVICE_LISTS
static void g_udev_device_print_list(GList *l, const gchar *msg);
#else
static void g_udev_device_print_list(GList *l, const gchar *msg) {}
#endif
static void g_udev_device_print(libusb_device *dev, const gchar *msg);

static gboolean g_udev_skip_search(libusb_device *dev);

GQuark g_udev_client_error_quark(void)
{
    return g_quark_from_static_string("win-gudev-client-error-quark");
}

GUdevClient *g_udev_client_new(void)
{
    if (singleton != NULL)
        return g_object_ref(singleton);

    singleton = g_initable_new(G_UDEV_TYPE_CLIENT, NULL, NULL, NULL);
    return singleton;
}

libusb_context *g_udev_client_get_context(GUdevClient *client)
{
    return client->priv->ctx;
}

/*
 * devs [in,out] an empty devs list in, full devs list out
 * Returns: number-of-devices, or a negative value on error.
 */
static ssize_t
g_udev_client_list_devices(GUdevClient *self, GList **devs,
                           GError **err, const gchar *name)
{
    gssize rc;
    libusb_device **lusb_list, **dev;
    GUdevClientPrivate *priv;
    ssize_t n;

    g_return_val_if_fail(G_UDEV_IS_CLIENT(self), -1);
    g_return_val_if_fail(devs != NULL, -2);

    priv = self->priv;

    g_return_val_if_fail(self->priv->ctx != NULL, -3);

    rc = libusb_get_device_list(priv->ctx, &lusb_list);
    if (rc < 0) {
        const char *errstr = spice_usbutil_libusb_strerror(rc);
        g_warning("%s: libusb_get_device_list failed - %s", name, errstr);
        g_set_error(err, G_UDEV_CLIENT_ERROR, G_UDEV_CLIENT_LIBUSB_FAILED,
                    "%s: Error getting device list from libusb: %s [%"G_GSSIZE_FORMAT"]",
                    name, errstr, rc);
        return -4;
    }

    n = 0;
    for (dev = lusb_list; *dev; dev++) {
        if (g_udev_skip_search(*dev)) {
            continue;
        }
        *devs = g_list_prepend(*devs, libusb_ref_device(*dev));
        n++;
    }
    libusb_free_device_list(lusb_list, 1);

    return n;
}

static void unreference_libusb_device(gpointer data)
{
    libusb_unref_device((libusb_device *)data);
}

static void g_udev_client_free_device_list(GList **devs)
{
    g_return_if_fail(devs != NULL);
    if (*devs) {
        /* the unreference_libusb_device method is required as
         * libusb_unref_device calling convention differs from glib's
         * see 558c967ec
         */
        g_list_free_full(*devs, unreference_libusb_device);
        *devs = NULL;
    }
}


static gboolean
g_udev_client_initable_init(GInitable *initable, GCancellable *cancellable,
                            GError **err)
{
    GUdevClient *self;
    GUdevClientPrivate *priv;
    WNDCLASS wcls;
    int rc;

    g_return_val_if_fail(G_UDEV_IS_CLIENT(initable), FALSE);
    g_return_val_if_fail(cancellable == NULL, FALSE);

    self = G_UDEV_CLIENT(initable);
    priv = self->priv;

    rc = libusb_init(&priv->ctx);
    if (rc < 0) {
        const char *errstr = spice_usbutil_libusb_strerror(rc);
        g_warning("Error initializing USB support: %s [%i]", errstr, rc);
        g_set_error(err, G_UDEV_CLIENT_ERROR, G_UDEV_CLIENT_LIBUSB_FAILED,
                    "Error initializing USB support: %s [%i]", errstr, rc);
        return FALSE;
    }
#ifdef G_OS_WIN32
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(priv->ctx, LIBUSB_OPTION_USE_USBDK);
#endif
#endif

    /* get initial device list */
    if (g_udev_client_list_devices(self, &priv->udev_list, err, __FUNCTION__) < 0) {
        goto g_udev_client_init_failed;
    }

    g_udev_device_print_list(priv->udev_list, "init: first list is: ");

    /* create a hidden window */
    memset(&wcls, 0, sizeof(wcls));
    wcls.lpfnWndProc = wnd_proc;
    wcls.lpszClassName = G_UDEV_CLIENT_WINCLASS_NAME;
    if (!RegisterClass(&wcls)) {
        DWORD e = GetLastError();
        g_warning("RegisterClass failed , %ld", (long)e);
        g_set_error(err, G_UDEV_CLIENT_ERROR, G_UDEV_CLIENT_WINAPI_FAILED,
                    "RegisterClass failed: %ld", (long)e);
        goto g_udev_client_init_failed;
    }
    priv->hwnd = CreateWindow(G_UDEV_CLIENT_WINCLASS_NAME,
                              NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (!priv->hwnd) {
        DWORD e = GetLastError();
        g_warning("CreateWindow failed: %ld", (long)e);
        g_set_error(err, G_UDEV_CLIENT_ERROR, G_UDEV_CLIENT_LIBUSB_FAILED,
                    "CreateWindow failed: %ld", (long)e);
        goto g_udev_client_init_failed_unreg;
    }

    return TRUE;

 g_udev_client_init_failed_unreg:
    UnregisterClass(G_UDEV_CLIENT_WINCLASS_NAME, NULL);
 g_udev_client_init_failed:
    return FALSE;
}

static void g_udev_client_initable_iface_init(GInitableIface *iface)
{
    iface->init = g_udev_client_initable_init;
}

static void g_udev_notify_device(GUdevClient *self, libusb_device *dev, gboolean add)
{
    GUdevDeviceInfo *udevinfo;
    GUdevDevice *udevice;
    udevinfo = g_new0(GUdevDeviceInfo, 1);
    if (get_usb_dev_info(dev, udevinfo)) {
        udevice = g_udev_device_new(udevinfo);
        g_signal_emit(self, signals[UEVENT_SIGNAL], 0, udevice, add);
    } else {
        g_free(udevinfo);
    }
}

static void report_one_device(gpointer data, gpointer self)
{
    g_udev_notify_device(self, data, TRUE);
}

void g_udev_client_report_devices(GUdevClient *self)
{
    g_list_foreach(self->priv->udev_list, report_one_device, self);
}

static void g_udev_client_init(GUdevClient *self)
{
    self->priv = g_udev_client_get_instance_private(self);
}

static void g_udev_client_finalize(GObject *gobject)
{
    GUdevClient *self = G_UDEV_CLIENT(gobject);
    GUdevClientPrivate *priv = self->priv;

    singleton = NULL;
    DestroyWindow(priv->hwnd);
    UnregisterClass(G_UDEV_CLIENT_WINCLASS_NAME, NULL);
    g_udev_client_free_device_list(&priv->udev_list);

    /* free libusb context initializing by libusb_init() */
    g_warn_if_fail(priv->ctx != NULL);
    libusb_exit(priv->ctx);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(g_udev_client_parent_class)->finalize)
        G_OBJECT_CLASS(g_udev_client_parent_class)->finalize(gobject);
}

static void g_udev_client_get_property(GObject     *gobject,
                                       guint        prop_id,
                                       GValue      *value,
                                       GParamSpec  *pspec)
{
    GUdevClient *self = G_UDEV_CLIENT(gobject);
    GUdevClientPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_REDIRECTING:
        g_value_set_boolean(value, priv->redirecting);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void handle_dev_change(GUdevClient *self);

static void g_udev_client_set_property(GObject       *gobject,
                                       guint          prop_id,
                                       const GValue  *value,
                                       GParamSpec    *pspec)
{
    GUdevClient *self = G_UDEV_CLIENT(gobject);
    GUdevClientPrivate *priv = self->priv;
    gboolean old_val;

    switch (prop_id) {
    case PROP_REDIRECTING:
        old_val = priv->redirecting;
        priv->redirecting = g_value_get_boolean(value);
        if (old_val && !priv->redirecting) {
            /* This is a redirection completion case.
               Inject hotplug event in case we missed device changes
               during redirection processing. */
            handle_dev_change(self);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void g_udev_client_class_init(GUdevClientClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GParamSpec *pspec;

    gobject_class->finalize = g_udev_client_finalize;
    gobject_class->get_property = g_udev_client_get_property;
    gobject_class->set_property = g_udev_client_set_property;

    signals[UEVENT_SIGNAL] =
        g_signal_new("uevent",
                     G_OBJECT_CLASS_TYPE(klass),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(GUdevClientClass, uevent),
                     NULL, NULL,
                     g_cclosure_user_marshal_VOID__POINTER_BOOLEAN,
                     G_TYPE_NONE,
                     2,
                     G_TYPE_POINTER,
                     G_TYPE_BOOLEAN);

    /**
    * GUdevClient::redirecting:
    *
    * This property indicates when a redirection operation
    * is in progress on a device. It's set back to FALSE
    * once the device is fully redirected to the guest.
    */
    pspec = g_param_spec_boolean("redirecting", "Redirecting",
                                 "USB redirection operation is in progress",
                                 FALSE,
                                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_property(gobject_class, PROP_REDIRECTING, pspec);
}

static gboolean get_usb_dev_info(libusb_device *dev, GUdevDeviceInfo *udevinfo)
{
    struct libusb_device_descriptor desc;

    g_return_val_if_fail(dev, FALSE);
    g_return_val_if_fail(udevinfo, FALSE);

    if (libusb_get_device_descriptor(dev, &desc) < 0) {
        g_warning("cannot get device descriptor %p", dev);
        return FALSE;
    }

    udevinfo->bus   = libusb_get_bus_number(dev);
    udevinfo->addr  = libusb_get_device_address(dev);
    udevinfo->class = desc.bDeviceClass;
    udevinfo->vid   = desc.idVendor;
    udevinfo->pid   = desc.idProduct;
    g_snprintf(udevinfo->sclass, sizeof(udevinfo->sclass), "%d", udevinfo->class);
    g_snprintf(udevinfo->sbus,   sizeof(udevinfo->sbus),   "%d", udevinfo->bus);
    g_snprintf(udevinfo->saddr,  sizeof(udevinfo->saddr),  "%d", udevinfo->addr);
    g_snprintf(udevinfo->svid,   sizeof(udevinfo->svid),   "%d", udevinfo->vid);
    g_snprintf(udevinfo->spid,   sizeof(udevinfo->spid),   "%d", udevinfo->pid);
    return TRUE;
}

/* comparing bus:addr and vid:pid */
static gint compare_libusb_devices(gconstpointer a, gconstpointer b)
{
    libusb_device *a_dev = (libusb_device *)a;
    libusb_device *b_dev = (libusb_device *)b;
    struct libusb_device_descriptor a_desc, b_desc;
    gboolean same_bus, same_addr, same_vid, same_pid;

    libusb_get_device_descriptor(a_dev, &a_desc);
    libusb_get_device_descriptor(b_dev, &b_desc);

    same_bus = (libusb_get_bus_number(a_dev) == libusb_get_bus_number(b_dev));
    same_addr = (libusb_get_device_address(a_dev) == libusb_get_device_address(b_dev));
    same_vid = (a_desc.idVendor == b_desc.idVendor);
    same_pid = (a_desc.idProduct == b_desc.idProduct);

    return (same_bus && same_addr && same_vid && same_pid) ? 0 : -1;
}

static void notify_dev_state_change(GUdevClient *self,
                                    GList *old_list,
                                    GList *new_list,
                                    gboolean add)
{
    GList *dev;

    for (dev = g_list_first(old_list); dev != NULL; dev = g_list_next(dev)) {
        GList *found = g_list_find_custom(new_list, dev->data, compare_libusb_devices);
        if (found == NULL) {
            g_udev_device_print(dev->data, add ? "add" : "remove");
            g_udev_notify_device(self, dev->data, add);
        }
    }
}

static void handle_dev_change(GUdevClient *self)
{
    GUdevClientPrivate *priv = self->priv;
    GError *err = NULL;
    GList *now_devs = NULL;

    if (priv->redirecting == TRUE) {
        /* On Windows, querying USB device list may return inconsistent results
           if performed in parallel to redirection flow.
           A simulated hotplug event will be injected after redirection
           completion in order to process real device list changes that may
           had taken place during redirection process. */
        return;
    }

    if(g_udev_client_list_devices(self, &now_devs, &err, __FUNCTION__) < 0) {
        g_warning("could not retrieve device list");
        return;
    }

    g_udev_device_print_list(now_devs, "handle_dev_change: current list:");
    g_udev_device_print_list(priv->udev_list, "handle_dev_change: previous list:");

    /* Unregister devices that are not present anymore */
    notify_dev_state_change(self, priv->udev_list, now_devs, FALSE);

    /* Register newly inserted devices */
    notify_dev_state_change(self, now_devs, priv->udev_list, TRUE);

    /* keep most recent info: free previous list, and keep current list */
    g_udev_client_free_device_list(&priv->udev_list);
    priv->udev_list = now_devs;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    /* Only DBT_DEVNODES_CHANGED recieved */
    if (message == WM_DEVICECHANGE) {
        handle_dev_change(singleton);
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

/*** GUdevDevice ***/

static void g_udev_device_finalize(GObject *object)
{
    GUdevDevice *device =  G_UDEV_DEVICE(object);

    g_free(device->priv->udevinfo);
    if (G_OBJECT_CLASS(g_udev_device_parent_class)->finalize != NULL)
        (* G_OBJECT_CLASS(g_udev_device_parent_class)->finalize)(object);
}

static void g_udev_device_class_init(GUdevDeviceClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *) klass;

    gobject_class->finalize = g_udev_device_finalize;
}

static void g_udev_device_init(GUdevDevice *device)
{
    device->priv = g_udev_device_get_instance_private(device);
}

static GUdevDevice *g_udev_device_new(GUdevDeviceInfo *udevinfo)
{
    GUdevDevice *device;

    g_return_val_if_fail(udevinfo != NULL, NULL);

    device =  G_UDEV_DEVICE(g_object_new(G_UDEV_TYPE_DEVICE, NULL));
    device->priv->udevinfo = udevinfo;
    return device;
}

const gchar *g_udev_device_get_property(GUdevDevice *udev, const gchar *property)
{
    GUdevDeviceInfo* udevinfo;

    g_return_val_if_fail(G_UDEV_DEVICE(udev), NULL);
    g_return_val_if_fail(property != NULL, NULL);

    udevinfo = udev->priv->udevinfo;
    g_return_val_if_fail(udevinfo != NULL, NULL);

    if (g_strcmp0(property, "BUSNUM") == 0) {
        return udevinfo->sbus;
    } else if (g_strcmp0(property, "DEVNUM") == 0) {
        return udevinfo->saddr;
    } else if (g_strcmp0(property, "DEVTYPE") == 0) {
        return "usb_device";
    } else if (g_strcmp0(property, "VID") == 0) {
        return udevinfo->svid;
    } else if (g_strcmp0(property, "PID") == 0) {
        return udevinfo->spid;
    }

    g_warn_if_reached();
    return NULL;
}

#ifdef DEBUG_GUDEV_DEVICE_LISTS
static void g_udev_device_print_list(GList *l, const gchar *msg)
{
    GList *it;

    for (it = g_list_first(l); it != NULL; it=g_list_next(it)) {
        g_udev_device_print(it->data, msg);
    }
}
#endif

static void g_udev_device_print(libusb_device *dev, const gchar *msg)
{
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(dev, &desc);

    SPICE_DEBUG("%s: %d.%d 0x%04x:0x%04x class %d", msg,
                libusb_get_bus_number(dev),
                libusb_get_device_address(dev),
                desc.idVendor, desc.idProduct, desc.bDeviceClass);
}

static gboolean g_udev_skip_search(libusb_device *dev)
{
    gboolean skip;
    uint8_t addr = libusb_get_device_address(dev);
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(dev, &desc);

    skip = ((addr == 0xff) ||  /* root hub (HCD) */
            (addr == 1) || /* root hub addr */
            (desc.bDeviceClass == LIBUSB_CLASS_HUB) || /* hub*/
            (addr == 0)); /* bad address */
    return skip;
}
