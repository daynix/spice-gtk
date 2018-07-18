/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
    Copyright (C) 2018 Red Hat, Inc.

    Red Hat Authors:
    Yuri Benditovich<ybendito@redhat.com>

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

#ifndef __SPICE_USB_BACKEND_H__
#define __SPICE_USB_BACKEND_H__

#include <usbredirfilter.h>
#include "usb-device-manager.h"

G_BEGIN_DECLS

typedef struct _SpiceUsbBackend SpiceUsbBackend;
typedef struct _SpiceUsbBackendDevice SpiceUsbBackendDevice;
typedef struct _SpiceUsbBackendChannel SpiceUsbBackendChannel;

typedef struct _UsbDeviceInformation
{
    uint16_t bus;
    uint16_t address;
    uint16_t vid;
    uint16_t pid;
    uint8_t class;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t isochronous;
    uint8_t is_cd;
} UsbDeviceInformation;

typedef struct _SpiceUsbBackendChannelInitData
{
    void *user_data;
    void (*log)(void *user_data, const char *msg, gboolean error);
    int (*write_callback)(void *user_data, uint8_t *data, int count);
    int (*is_channel_ready)(void *user_data);
    uint64_t (*get_queue_size)(void *user_data);
    gboolean debug;
} SpiceUsbBackendChannelInitData;

typedef void(*usb_hot_plug_callback)(
    void *user_data, SpiceUsbBackendDevice *dev, gboolean added);

typedef void(*backend_device_change_callback)(
    void *user_data, SpiceUsbBackendDevice *dev);

enum {
    USB_REDIR_ERROR_IO = -1,
    USB_REDIR_ERROR_READ_PARSE = -2,
    USB_REDIR_ERROR_DEV_REJECTED = -3,
    USB_REDIR_ERROR_DEV_LOST = -4,
};

SpiceUsbBackend *spice_usb_backend_initialize(void);
gboolean spice_usb_backend_handle_events(SpiceUsbBackend *);
gboolean spice_usb_backend_handle_hotplug(SpiceUsbBackend *, void *user_data, usb_hot_plug_callback proc);
void spice_usb_backend_set_device_change_callback(SpiceUsbBackend *, void *user_data, backend_device_change_callback proc);
void spice_usb_backend_finalize(SpiceUsbBackend *context);
// returns NULL-terminated array of SpiceUsbBackendDevice *
SpiceUsbBackendDevice **spice_usb_backend_get_device_list(SpiceUsbBackend *backend);
gboolean spice_usb_backend_device_is_hub(SpiceUsbBackendDevice *dev);
gboolean spice_usb_backend_device_need_thread(SpiceUsbBackendDevice *dev);
void spice_usb_backend_free_device_list(SpiceUsbBackendDevice **devlist);
void spice_usb_backend_device_acquire(SpiceUsbBackendDevice *dev);
void spice_usb_backend_device_release(SpiceUsbBackendDevice *dev);
gboolean spice_usb_backend_devices_same(SpiceUsbBackendDevice *dev1, SpiceUsbBackendDevice *dev2);
gconstpointer spice_usb_backend_device_get_libdev(SpiceUsbBackendDevice *dev);
const UsbDeviceInformation*  spice_usb_backend_device_get_info(SpiceUsbBackendDevice *dev);
// returns 0 if the device passes the filter
int spice_usb_backend_device_check_filter(SpiceUsbBackendDevice *dev, const struct usbredirfilter_rule *rules, int count);

SpiceUsbBackendChannel *spice_usb_backend_channel_initialize(SpiceUsbBackend *context, const SpiceUsbBackendChannelInitData *init_data);
// returns 0 for success or error code
int spice_usb_backend_provide_read_data(SpiceUsbBackendChannel *ch, uint8_t *data, int count);
gboolean spice_usb_backend_channel_attach(SpiceUsbBackendChannel *ch, SpiceUsbBackendDevice *dev, const char **msg);
void spice_usb_backend_channel_up(SpiceUsbBackendChannel *ch);
void spice_usb_backend_channel_get_guest_filter(SpiceUsbBackendChannel *ch, const struct usbredirfilter_rule  **rules, int *count);
void spice_usb_backend_return_write_data(SpiceUsbBackendChannel *ch, void *data);
void spice_usb_backend_channel_finalize(SpiceUsbBackendChannel *ch);

gboolean spice_usb_backend_add_cd_lun(SpiceUsbBackend *be, const SpiceUsbDeviceLunInfo *info);
gboolean spice_usb_backend_remove_cd_lun(SpiceUsbBackend *be, SpiceUsbBackendDevice *bdev, guint lun);
uint32_t spice_usb_backend_get_cd_luns_bitmask(SpiceUsbBackendDevice *bdev);
gboolean spice_usb_backend_get_cd_lun_info(SpiceUsbBackendDevice *bdev, guint lun, SpiceUsbDeviceLunInfo *info);
gboolean spice_usb_backend_load_cd_lun(SpiceUsbBackend *be,
    SpiceUsbBackendDevice *bdev, guint lun, gboolean load);
gboolean spice_usb_backend_lock_cd_lun(SpiceUsbBackend *be,
    SpiceUsbBackendDevice *bdev, guint lun, gboolean lock);
gboolean spice_usb_backend_change_cd_lun(
    SpiceUsbBackend *be, SpiceUsbBackendDevice *bdev,
    guint lun, const SpiceUsbDeviceLunInfo *info);

G_END_DECLS

#endif
