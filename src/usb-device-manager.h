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
#ifndef __SPICE_USB_DEVICE_MANAGER_H__
#define __SPICE_USB_DEVICE_MANAGER_H__

#if !defined(__SPICE_CLIENT_H_INSIDE__) && !defined(SPICE_COMPILATION)
#warning "Only <spice-client.h> can be included directly"
#endif

#include "spice-client.h"
#include <gio/gio.h>

G_BEGIN_DECLS

#define SPICE_TYPE_USB_DEVICE_MANAGER            (spice_usb_device_manager_get_type ())
#define SPICE_USB_DEVICE_MANAGER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SPICE_TYPE_USB_DEVICE_MANAGER, SpiceUsbDeviceManager))
#define SPICE_USB_DEVICE_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SPICE_TYPE_USB_DEVICE_MANAGER, SpiceUsbDeviceManagerClass))
#define SPICE_IS_USB_DEVICE_MANAGER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SPICE_TYPE_USB_DEVICE_MANAGER))
#define SPICE_IS_USB_DEVICE_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), SPICE_TYPE_USB_DEVICE_MANAGER))
#define SPICE_USB_DEVICE_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SPICE_TYPE_USB_DEVICE_MANAGER, SpiceUsbDeviceManagerClass))

#define SPICE_TYPE_USB_DEVICE                    (spice_usb_device_get_type())

typedef struct _SpiceUsbDeviceManager SpiceUsbDeviceManager;
typedef struct _SpiceUsbDeviceManagerClass SpiceUsbDeviceManagerClass;
typedef struct _SpiceUsbDeviceManagerPrivate SpiceUsbDeviceManagerPrivate;

/**
 * SpiceUsbDevice:
 *
 * The #SpiceUsbDevice struct is opaque and cannot be accessed directly.
 */
typedef struct _SpiceUsbDevice SpiceUsbDevice;

/**
 * SpiceUsbDeviceManager:
 *
 * The #SpiceUsbDeviceManager struct is opaque and should not be accessed directly.
 */
struct _SpiceUsbDeviceManager
{
    GObject parent;

    /*< private >*/
    SpiceUsbDeviceManagerPrivate *priv;
    /* Do not add fields to this struct */
};

/**
 * SpiceUsbDeviceManagerClass:
 * @parent_class: Parent class.
 * @device_added: Signal class handler for the #SpiceUsbDeviceManager::device-added signal.
 * @device_removed: Signal class handler for the #SpiceUsbDeviceManager::device-removed signal.
 * @auto_connect_failed: Signal class handler for the #SpiceUsbDeviceManager::auto-connect-failed signal.
 * @device_error: Signal class handler for the #SpiceUsbDeviceManager::device_error signal.
 * @device_change: Signal class handler for the #SpiceUsbDeviceManager::device_change signal.
 *
 * Class structure for #SpiceUsbDeviceManager.
 */
struct _SpiceUsbDeviceManagerClass
{
    GObjectClass parent_class;

    /* signals */
    void (*device_added) (SpiceUsbDeviceManager *manager,
                          SpiceUsbDevice *device);
    void (*device_removed) (SpiceUsbDeviceManager *manager,
                            SpiceUsbDevice *device);
    void (*auto_connect_failed) (SpiceUsbDeviceManager *manager,
                                 SpiceUsbDevice *device, GError *error);
    void (*device_error) (SpiceUsbDeviceManager *manager,
                          SpiceUsbDevice *device, GError *error);
    void (*device_change) (SpiceUsbDeviceManager *manager,
                          SpiceUsbDevice *device);

    /*< private >*/
    /*
     * If adding fields to this struct, remove corresponding
     * amount of padding to avoid changing overall struct size
     */
    gchar _spice_reserved[SPICE_RESERVED_PADDING - 1 * sizeof(void *)];
};

GType spice_usb_device_get_type(void);
GType spice_usb_device_manager_get_type(void);

typedef struct _spice_usb_device_info
{
    guint16 bus;
    guint16 address;
    guint16 vendor_id;
    guint16 product_id;
    // (OUT) allocated strings for vendor and product
    gchar *vendor;
    gchar *product;
} spice_usb_device_info;

gchar *spice_usb_device_get_description(SpiceUsbDevice *device, const gchar *format);
gboolean spice_usb_device_get_info(SpiceUsbDevice *device, spice_usb_device_info *info);
gconstpointer spice_usb_device_get_libusb_device(const SpiceUsbDevice *device);

SpiceUsbDeviceManager *spice_usb_device_manager_get(SpiceSession *session,
                                                    GError **err);

GPtrArray *spice_usb_device_manager_get_devices(SpiceUsbDeviceManager *manager);
GPtrArray* spice_usb_device_manager_get_devices_with_filter(
    SpiceUsbDeviceManager *manager, const gchar *filter);

gboolean spice_usb_device_manager_is_device_connected(SpiceUsbDeviceManager *manager,
                                                      SpiceUsbDevice *device);
void spice_usb_device_manager_connect_device_async(
                                             SpiceUsbDeviceManager *self,
                                             SpiceUsbDevice *device,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

void spice_usb_device_manager_disconnect_device_async(
                                             SpiceUsbDeviceManager *self,
                                             SpiceUsbDevice *device,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

gboolean spice_usb_device_manager_connect_device_finish(
    SpiceUsbDeviceManager *self, GAsyncResult *res, GError **err);

gboolean spice_usb_device_manager_disconnect_device_finish(
    SpiceUsbDeviceManager *self, GAsyncResult *res, GError **err);

#ifndef SPICE_DISABLE_DEPRECATED
G_DEPRECATED_FOR(spice_usb_device_manager_disconnect_device_async)
void spice_usb_device_manager_disconnect_device(SpiceUsbDeviceManager *manager,
                                                SpiceUsbDevice *device);
#endif

gboolean
spice_usb_device_manager_can_redirect_device(SpiceUsbDeviceManager  *self,
                                             SpiceUsbDevice         *device,
                                             GError                **err);

gboolean spice_usb_device_manager_is_redirecting(SpiceUsbDeviceManager *self);

gboolean spice_usb_device_manager_is_device_cd(SpiceUsbDeviceManager *self,
                                               SpiceUsbDevice *device);

/* returns new array of guint LUN indices */
GArray *spice_usb_device_manager_get_device_luns(SpiceUsbDeviceManager *self,
                                                SpiceUsbDevice *device);

typedef struct _spice_usb_device_lun_info
{
    const gchar *file_path;

    const gchar *vendor;
    const gchar *product;
    const gchar *revision;

    const gchar *alias;

    gboolean started;
    gboolean loaded;
    gboolean locked;
} spice_usb_device_lun_info;

/* CD LUN will be attached to a (possibly new) USB device automatically */
gboolean spice_usb_device_manager_add_cd_lun(SpiceUsbDeviceManager *self,
                                             spice_usb_device_lun_info *lun_info);

/* Get CD LUN info, intended primarily for enumerating LUNs */
gboolean
spice_usb_device_manager_device_lun_get_info(SpiceUsbDeviceManager *self,
                                             SpiceUsbDevice *device,
                                             guint lun,
                                             spice_usb_device_lun_info *lun_info);
/* lock or unlock device */
gboolean
spice_usb_device_manager_device_lun_lock(SpiceUsbDeviceManager *self,
                                         SpiceUsbDevice *device,
                                         guint lun,
                                         gboolean lock);

/* load or eject device */
gboolean
spice_usb_device_manager_device_lun_load(SpiceUsbDeviceManager *self,
                                         SpiceUsbDevice *device,
                                         guint lun,
                                         gboolean load);

/* change the media - device must be not currently loaded */
gboolean
spice_usb_device_manager_device_lun_change_media(SpiceUsbDeviceManager *self,
                                                 SpiceUsbDevice *device,
                                                 guint lun,
                                                 gchar *filename);
/* remove lun from the usb device */
gboolean
spice_usb_device_manager_device_lun_remove(SpiceUsbDeviceManager *self,
                                           SpiceUsbDevice *device,
                                           guint lun);

G_END_DECLS

#endif /* __SPICE_USB_DEVICE_MANAGER_H__ */
