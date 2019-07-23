/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
    Copyright (C) 2012-2018 Red Hat, Inc.

    Red Hat Authors:
    Yuri Benditovich<ybendito@redhat.com>
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

#ifdef USE_USBREDIR

#include <glib-object.h>
#include <inttypes.h>
#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <errno.h>
#include <libusb.h>
#include <string.h>
#include <fcntl.h>
#ifdef G_OS_WIN32
#include <commctrl.h>
#endif
#include "usbredirhost.h"
#include "usbredirparser.h"
#include "spice-util.h"
#include "usb-backend.h"
#include "channel-usbredir-priv.h"
#include "spice-channel-priv.h"

#define LOUD_DEBUG(x, ...)

struct _SpiceUsbBackendDevice
{
    libusb_device *libusb_device;
    gint ref_count;
    SpiceUsbBackendChannel *attached_to;
    UsbDeviceInformation device_info;
};

struct _SpiceUsbBackend
{
    libusb_context *libusb_context;
    usb_hot_plug_callback hotplug_callback;
    void *hotplug_user_data;
    libusb_hotplug_callback_handle hotplug_handle;
#ifdef G_OS_WIN32
    HANDLE hWnd;
    libusb_device **libusb_device_list;
    gint redirecting;
#endif
};

struct _SpiceUsbBackendChannel
{
    struct usbredirhost *usbredirhost;
    uint8_t *read_buf;
    int read_buf_size;
    struct usbredirfilter_rule *rules;
    int rules_count;
    SpiceUsbBackendDevice *attached;
    SpiceUsbredirChannel  *user_data;
    SpiceUsbBackend *backend;
    GError **error;
};

static void get_usb_device_info_from_libusb_device(UsbDeviceInformation *info,
                                                   libusb_device *libdev)
{
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(libdev, &desc);
    info->bus = libusb_get_bus_number(libdev);
    info->address = libusb_get_device_address(libdev);
    info->vid = desc.idVendor;
    info->pid = desc.idProduct;
    info->class = desc.bDeviceClass;
    info->subclass = desc.bDeviceSubClass;
    info->protocol = desc.bDeviceProtocol;
}

static gboolean fill_usb_info(SpiceUsbBackendDevice *dev)
{
    UsbDeviceInformation *info = &dev->device_info;
    get_usb_device_info_from_libusb_device(info, dev->libusb_device);

    if (info->address == 0xff || /* root hub (HCD) */
        info->address <= 1 || /* root hub or bad address */
        (info->class == LIBUSB_CLASS_HUB) /*hub*/) {
        return FALSE;
    }
    return TRUE;
}

static SpiceUsbBackendDevice *allocate_backend_device(libusb_device *libdev)
{
    SpiceUsbBackendDevice *dev = g_new0(SpiceUsbBackendDevice, 1);
    dev->ref_count = 1;
    dev->libusb_device = libdev;
    if (!fill_usb_info(dev)) {
        g_clear_pointer(&dev, g_free);
    }
    return dev;
}

static int LIBUSB_CALL hotplug_callback(libusb_context *ctx,
                                        libusb_device *libdev,
                                        libusb_hotplug_event event,
                                        void *user_data)
{
    SpiceUsbBackend *be = user_data;
    SpiceUsbBackendDevice *dev;
    gboolean arrived = event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED;

    g_return_val_if_fail(be->hotplug_callback != NULL, 0);

    dev = allocate_backend_device(libdev);
    if (dev) {
        SPICE_DEBUG("created dev %p, usblib dev %p", dev, libdev);
        libusb_ref_device(libdev);
        be->hotplug_callback(be->hotplug_user_data, dev, arrived);
        spice_usb_backend_device_unref(dev);
    }
    return 0;
}

#ifdef G_OS_WIN32
/* Windows-specific: get notification on device change */

static gboolean is_same_libusb_dev(libusb_device *libdev1,
                                   libusb_device *libdev2)
{
    UsbDeviceInformation info1, info2;
    g_return_val_if_fail(libdev1 != NULL && libdev2 != NULL, FALSE);

    get_usb_device_info_from_libusb_device(&info1, libdev1);
    get_usb_device_info_from_libusb_device(&info2, libdev2);

    return info1.bus == info2.bus &&
           info1.address == info2.address &&
           info1.vid == info2.vid &&
           info1.pid == info2.pid;
}

/*
    Compares context and list1 and list2 and fire callback for each
    device in list1 that not present in list2.
    Returns number of such devices.
*/
static int compare_dev_list_fire_callback(SpiceUsbBackend *be,
                                          libusb_device * const *list1,
                                          libusb_device * const *list2,
                                          gboolean add)
{
    int num_changed = 0;
    libusb_hotplug_event event = add ?
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED : LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
    while (*list1) {
        gboolean found = 0;
        uint32_t n = 0;
        while (!found && list2[n]) {
            found = is_same_libusb_dev(*list1, list2[n]);
            n++;
        }
        if (!found) {
            UsbDeviceInformation info;
            get_usb_device_info_from_libusb_device(&info, *list1);
            SPICE_DEBUG("%s %04X:%04X at %d:%d", add ? "adding" : "removing",
                        info.vid, info.pid, info.bus, info.address);
            hotplug_callback(NULL, *list1, event, be);
            num_changed++;
        }
        list1++;
    }
    return num_changed;
}

static LRESULT subclass_proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    SpiceUsbBackend *be = (SpiceUsbBackend *)dwRefData;
    if (uMsg == WM_DEVICECHANGE && !be->redirecting) {
        libusb_device **new_list = NULL;
        libusb_get_device_list(be->libusb_context, &new_list);
        if (new_list) {
            int num_changed = compare_dev_list_fire_callback(be, be->libusb_device_list, new_list, FALSE);
            num_changed += compare_dev_list_fire_callback(be, new_list, be->libusb_device_list, TRUE);
            if (num_changed > 0) {
                libusb_free_device_list(be->libusb_device_list, TRUE);
                be->libusb_device_list = new_list;
            } else {
                libusb_free_device_list(new_list, TRUE);
            }
        } else {
            g_warn_if_reached();
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static void disable_hotplug_support(SpiceUsbBackend *be)
{
    if (be->hWnd) {
        DestroyWindow(be->hWnd);
        be->hWnd = NULL;
    }
    if (be->libusb_device_list) {
        libusb_free_device_list(be->libusb_device_list, TRUE);
        be->libusb_device_list = NULL;
    }
}

static int enable_hotplug_support(SpiceUsbBackend *be, const char **error_on_enable)
{
    long win_err;
    libusb_device **libdev_list = NULL;
    libusb_device *empty_list = NULL;

    libusb_get_device_list(be->libusb_context, &libdev_list);
    if (!libdev_list) {
        *error_on_enable = "Getting device list";
        goto error;
    }
    /* using standard class for window to receive messages */
    be->hWnd = CreateWindow("Static", NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (!be->hWnd) {
        *error_on_enable = "CreateWindow";
        goto error;
    }
    if (!SetWindowSubclass(be->hWnd, subclass_proc, 0, (DWORD_PTR)be)) {
        *error_on_enable = "SetWindowSubclass";
        goto error;
    }
    be->hotplug_handle = 1;
    be->libusb_device_list = libdev_list;

    compare_dev_list_fire_callback(be, be->libusb_device_list, &empty_list, TRUE);

    return LIBUSB_SUCCESS;
error:
    win_err = GetLastError();
    if (!win_err) {
        win_err = -1;
    }
    g_warning("%s failed: %ld", *error_on_enable, win_err);
    if (libdev_list) {
        libusb_free_device_list(libdev_list, TRUE);
    }
    return win_err;
}

static void set_redirecting(SpiceUsbBackend *be, gboolean on)
{
    if (on) {
        g_atomic_int_inc(&be->redirecting);
    } else {
        gboolean no_redir;
        no_redir = g_atomic_int_dec_and_test(&be->redirecting);
        if (no_redir && be->hWnd) {
            PostMessage(be->hWnd, WM_DEVICECHANGE, 0, 0);
        }
    }
}

#else
/* Linux-specific: use hot callback from libusb */

static void disable_hotplug_support(SpiceUsbBackend *be)
{
    libusb_hotplug_deregister_callback(be->libusb_context, be->hotplug_handle);
}

static int enable_hotplug_support(SpiceUsbBackend *be, const char **error_on_enable)
{
    int rc = 0;
    rc = libusb_hotplug_register_callback(be->libusb_context,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        hotplug_callback, be, &be->hotplug_handle);
    *error_on_enable = libusb_strerror(rc);
    return rc;
}

static inline void set_redirecting(SpiceUsbBackend *be, gboolean on) {
    /* nothing on Linux */
}

#endif

/* lock functions for usbredirhost and usbredirparser */
static void *usbredir_alloc_lock(void) {
    GMutex *mutex;

    mutex = g_new0(GMutex, 1);
    g_mutex_init(mutex);

    return mutex;
}

static void usbredir_free_lock(void *user_data) {
    GMutex *mutex = user_data;

    g_mutex_clear(mutex);
    g_free(mutex);
}

static void usbredir_lock_lock(void *user_data) {
    GMutex *mutex = user_data;

    g_mutex_lock(mutex);
}

static void usbredir_unlock_lock(void *user_data) {
    GMutex *mutex = user_data;

    g_mutex_unlock(mutex);
}

gboolean spice_usb_backend_device_isoch(SpiceUsbBackendDevice *dev)
{
    libusb_device *libdev = dev->libusb_device;
    struct libusb_config_descriptor *conf_desc;
    gboolean isoc_found = FALSE;
    gint i, j, k;
    int rc;

    g_return_val_if_fail(libdev != NULL, 0);

    rc = libusb_get_active_config_descriptor(libdev, &conf_desc);
    if (rc) {
        const char *desc = libusb_strerror(rc);
        g_warning("can't get configuration descriptor, %s [%i]", desc, rc);
        return FALSE;
    }

    for (i = 0; !isoc_found && i < conf_desc->bNumInterfaces; i++) {
        for (j = 0; !isoc_found && j < conf_desc->interface[i].num_altsetting; j++) {
            for (k = 0; !isoc_found && k < conf_desc->interface[i].altsetting[j].bNumEndpoints;k++) {
                gint attributes = conf_desc->interface[i].altsetting[j].endpoint[k].bmAttributes;
                gint type = attributes & LIBUSB_TRANSFER_TYPE_MASK;
                if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                    isoc_found = TRUE;
                }
            }
        }
    }

    libusb_free_config_descriptor(conf_desc);
    return isoc_found;
}

static gboolean is_channel_ready(SpiceUsbredirChannel *channel)
{
    return spice_channel_get_state(SPICE_CHANNEL(channel)) == SPICE_CHANNEL_STATE_READY;
}

/* Note that this function must be re-entrant safe, as it can get called
from both the main thread as well as from the usb event handling thread */
static void usbredir_write_flush_callback(void *user_data)
{
    SpiceUsbBackendChannel *ch = user_data;
    if (!ch->usbredirhost) {
        /* just to be on the safe side */
        return;
    }
    if (is_channel_ready(ch->user_data)) {
        SPICE_DEBUG("%s ch %p -> usbredirhost", __FUNCTION__, ch);
        usbredirhost_write_guest_data(ch->usbredirhost);
    } else {
        SPICE_DEBUG("%s ch %p (not ready)", __FUNCTION__, ch);
    }
}

SpiceUsbBackend *spice_usb_backend_new(GError **error)
{
    int rc;
    SpiceUsbBackend *be;
    SPICE_DEBUG("%s >>", __FUNCTION__);
    be = g_new0(SpiceUsbBackend, 1);
    rc = libusb_init(&be->libusb_context);
    if (rc < 0) {
        const char *desc = libusb_strerror(rc);
        g_warning("Error initializing LIBUSB support: %s [%i]", desc, rc);
        g_set_error(error, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
            "Error initializing LIBUSB support: %s [%i]", desc, rc);
        g_free(be);
        be = NULL;
    } else {
#ifdef G_OS_WIN32
#if LIBUSB_API_VERSION >= 0x01000106
        libusb_set_option(be->libusb_context, LIBUSB_OPTION_USE_USBDK);
#endif
#endif
    }
    SPICE_DEBUG("%s <<", __FUNCTION__);
    return be;
}

gboolean spice_usb_backend_handle_events(SpiceUsbBackend *be)
{
    SPICE_DEBUG("%s >>", __FUNCTION__);
    gboolean ok = FALSE;
    if (be->libusb_context) {
        int res = libusb_handle_events(be->libusb_context);
        ok = res == 0;
        if (res && res != LIBUSB_ERROR_INTERRUPTED) {
            const char *desc = libusb_strerror(res);
            g_warning("Error handling USB events: %s [%i]", desc, res);
            ok = FALSE;
        }
    }
    SPICE_DEBUG("%s << %d", __FUNCTION__, ok);
    return ok;
}

void spice_usb_backend_interrupt_event_handler(SpiceUsbBackend *be)
{
    if (be->libusb_context) {
        libusb_interrupt_event_handler(be->libusb_context);
    }
}

void spice_usb_backend_deregister_hotplug(SpiceUsbBackend *be)
{
    g_return_if_fail(be != NULL);
    if (be->hotplug_handle) {
        disable_hotplug_support(be);
        be->hotplug_handle = 0;
    }
    be->hotplug_callback = NULL;
}

gboolean spice_usb_backend_register_hotplug(SpiceUsbBackend *be,
                                            void *user_data,
                                            usb_hot_plug_callback proc)
{
    int rc;
    const char *desc;
    g_return_val_if_fail(be != NULL, FALSE);

    be->hotplug_callback = proc;
    be->hotplug_user_data = user_data;

    rc = enable_hotplug_support(be, &desc);

    if (rc != LIBUSB_SUCCESS) {
        g_warning("Error initializing USB hotplug support: %s [%i]", desc, rc);
        be->hotplug_callback = NULL;
        return FALSE;
    }
    return TRUE;
}

void spice_usb_backend_delete(SpiceUsbBackend *be)
{
    g_return_if_fail(be != NULL);
    SPICE_DEBUG("%s >>", __FUNCTION__);
    if (be->libusb_context) {
        libusb_exit(be->libusb_context);
    }
    g_free(be);
    SPICE_DEBUG("%s <<", __FUNCTION__);
}

const UsbDeviceInformation* spice_usb_backend_device_get_info(SpiceUsbBackendDevice *dev)
{
    return &dev->device_info;
}

gconstpointer spice_usb_backend_device_get_libdev(SpiceUsbBackendDevice *dev)
{
    return dev->libusb_device;
}

SpiceUsbBackendDevice *spice_usb_backend_device_ref(SpiceUsbBackendDevice *dev)
{
    LOUD_DEBUG("%s >> %p", __FUNCTION__, dev);
    g_atomic_int_inc(&dev->ref_count);
    return dev;
}

void spice_usb_backend_device_unref(SpiceUsbBackendDevice *dev)
{
    LOUD_DEBUG("%s >> %p(%d)", __FUNCTION__, dev, dev->ref_count);
    if (g_atomic_int_dec_and_test(&dev->ref_count)) {
        libusb_unref_device(dev->libusb_device);
        LOUD_DEBUG("%s freeing %p (libusb %p)", __FUNCTION__, dev, dev->libusb_device);
        g_free(dev);
    }
}

int spice_usb_backend_device_check_filter(
    SpiceUsbBackendDevice *dev,
    const struct usbredirfilter_rule *rules,
    int count)
{
    return usbredirhost_check_device_filter(
        rules, count, dev->libusb_device, 0);
}

static int usbredir_read_callback(void *user_data, uint8_t *data, int count)
{
    SpiceUsbBackendChannel *ch = user_data;

    count = MIN(ch->read_buf_size, count);

    if (count != 0) {
        memcpy(data, ch->read_buf, count);
    }

    ch->read_buf_size -= count;
    if (ch->read_buf_size) {
        ch->read_buf += count;
    }
    else {
        ch->read_buf = NULL;
    }
    SPICE_DEBUG("%s ch %p, %d bytes", __FUNCTION__, ch, count);

    return count;
}

static const char *strip_usbredir_prefix(const char *msg)
{
    if (strncmp(msg, "usbredirhost: ", 14) == 0) {
        msg += 14;
    }
    return msg;
}

static void channel_error(SpiceUsbBackendChannel *ch, const char *msg)
{
    if (!ch->error)
        return;
    g_set_error_literal(ch->error, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED, msg);
    ch->error = NULL;
}

static void usbredir_log(void *user_data, int level, const char *msg)
{
    SpiceUsbBackendChannel *ch = (SpiceUsbBackendChannel *)user_data;
    const char *stripped_msg = strip_usbredir_prefix(msg);
    switch (level) {
    case usbredirparser_error:
        g_critical("%s", msg);
        channel_error(ch, stripped_msg);
        break;
    case usbredirparser_warning:
        g_warning("%s", msg);
        channel_error(ch, stripped_msg);
        break;
    default:
        break;
    }
}

static int usbredir_write_callback(void *user_data, uint8_t *data, int count)
{
    SpiceUsbBackendChannel *ch = user_data;
    int res;
    SPICE_DEBUG("%s ch %p, %d bytes", __FUNCTION__, ch, count);
    res = spice_usbredir_write_callback(ch->user_data, data, count);
    return res;
}

static uint64_t usbredir_buffered_output_size_callback(void *user_data)
{
    SpiceUsbBackendChannel *ch = user_data;
    return spice_channel_get_queue_size(SPICE_CHANNEL(ch->user_data));
}

int spice_usb_backend_read_guest_data(SpiceUsbBackendChannel *ch, uint8_t *data, int count)
{
    int res = 0;

    g_return_val_if_fail(ch->read_buf == NULL, USB_REDIR_ERROR_READ_PARSE);

    ch->read_buf = data;
    ch->read_buf_size = count;
    if (ch->usbredirhost) {
        res = usbredirhost_read_guest_data(ch->usbredirhost);
    } else {
        res = USB_REDIR_ERROR_IO;
    }
    switch (res)
    {
    case usbredirhost_read_io_error:
        res = USB_REDIR_ERROR_IO;
        break;
    case usbredirhost_read_parse_error:
        res = USB_REDIR_ERROR_READ_PARSE;
        break;
    case usbredirhost_read_device_rejected:
        res = USB_REDIR_ERROR_DEV_REJECTED;
        break;
    case usbredirhost_read_device_lost:
        res = USB_REDIR_ERROR_DEV_LOST;
        break;
    }
    SPICE_DEBUG("%s ch %p, %d bytes, res %d", __FUNCTION__, ch, count, res);

    return res;
}

GError *spice_usb_backend_get_error_details(int error_code, gchar *desc)
{
    GError *err;
    switch (error_code) {
        case USB_REDIR_ERROR_READ_PARSE:
            err = g_error_new(SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                              _("usbredir protocol parse error for %s"), desc);
            break;
        case USB_REDIR_ERROR_DEV_REJECTED:
            err = g_error_new(SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_USB_DEVICE_REJECTED,
                              _("%s rejected by host"), desc);
            break;
        case USB_REDIR_ERROR_DEV_LOST:
            err = g_error_new(SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_USB_DEVICE_LOST,
                              _("%s disconnected (fatal IO error)"), desc);
            break;
        default:
            err = g_error_new(SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                              _("Unknown error (%d) for %s"), error_code, desc);
            }
    return err;
}

void spice_usb_backend_return_write_data(SpiceUsbBackendChannel *ch, void *data)
{
    if (ch->usbredirhost) {
        SPICE_DEBUG("%s ch %p", __FUNCTION__, ch);
        usbredirhost_free_write_buffer(ch->usbredirhost, data);
    } else {
        SPICE_DEBUG("%s ch %p - NOBODY TO CALL", __FUNCTION__, ch);
    }
}

gboolean spice_usb_backend_channel_attach(SpiceUsbBackendChannel *ch,
                                          SpiceUsbBackendDevice *dev,
                                          GError **error)
{
    int rc;
    SPICE_DEBUG("%s >> ch %p, dev %p (was attached %p)", __FUNCTION__, ch, dev, ch->attached);

    g_return_val_if_fail(dev != NULL, FALSE);

    libusb_device_handle *handle = NULL;

    /*
       Under Windows we need to avoid updating
       list of devices when we are acquiring the device
    */
    set_redirecting(ch->backend, TRUE);

    rc = libusb_open(dev->libusb_device, &handle);

    set_redirecting(ch->backend, FALSE);

    if (rc) {
        const char *desc = libusb_strerror(rc);
        g_set_error(error, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
           "Error libusb_open: %s [%i]", desc, rc);
        return FALSE;
    }
    ch->error = error;
    rc = usbredirhost_set_device(ch->usbredirhost, handle);
    if (rc) {
        SPICE_DEBUG("%s ch %p, dev %p usbredirhost error %d", __FUNCTION__, ch, dev, rc);
        ch->error = NULL;
        return FALSE;
    } else {
        ch->attached = dev;
        dev->attached_to = ch;
    }
    ch->error = NULL;
    return TRUE;
}

void spice_usb_backend_channel_detach(SpiceUsbBackendChannel *ch)
{
    SPICE_DEBUG("%s >> ch %p, was attached %p", __FUNCTION__, ch, ch->attached);
    if (!ch->attached) {
        SPICE_DEBUG("%s: nothing to detach", __FUNCTION__);
        return;
    }
    if (ch->usbredirhost) {
        /* it will call libusb_close internally */
        usbredirhost_set_device(ch->usbredirhost, NULL);
    }
    SPICE_DEBUG("%s ch %p, detach done", __FUNCTION__, ch);
    ch->attached->attached_to = NULL;
    ch->attached = NULL;
}

SpiceUsbBackendChannel *spice_usb_backend_channel_new(SpiceUsbBackend *be,
                                                      void *user_data)
{
    SpiceUsbBackendChannel *ch;

    g_return_val_if_fail(SPICE_IS_USBREDIR_CHANNEL(user_data), NULL);

    ch = g_new0(SpiceUsbBackendChannel, 1);
    SPICE_DEBUG("%s >>", __FUNCTION__);
    ch->user_data = SPICE_USBREDIR_CHANNEL(user_data);
    if (be->libusb_context) {
        ch->backend = be;
        ch->usbredirhost = usbredirhost_open_full(
            be->libusb_context,
            NULL,
            usbredir_log,
            usbredir_read_callback,
            usbredir_write_callback,
            usbredir_write_flush_callback,
            usbredir_alloc_lock,
            usbredir_lock_lock,
            usbredir_unlock_lock,
            usbredir_free_lock,
            ch, PACKAGE_STRING,
            spice_util_get_debug() ? usbredirparser_debug : usbredirparser_warning,
            usbredirhost_fl_write_cb_owns_buffer);
        g_warn_if_fail(ch->usbredirhost != NULL);
    }
    if (ch->usbredirhost) {
        usbredirhost_set_buffered_output_size_cb(ch->usbredirhost, usbredir_buffered_output_size_callback);
    } else {
        g_free(ch);
        ch = NULL;
    }

    SPICE_DEBUG("%s << %p", __FUNCTION__, ch);
    return ch;
}

void spice_usb_backend_channel_flush_writes(SpiceUsbBackendChannel *ch)
{
    SPICE_DEBUG("%s %p, host %p", __FUNCTION__, ch, ch->usbredirhost);
    if (ch->usbredirhost) {
        usbredirhost_write_guest_data(ch->usbredirhost);
    }
}

void spice_usb_backend_channel_delete(SpiceUsbBackendChannel *ch)
{
    SPICE_DEBUG("%s >> %p", __FUNCTION__, ch);
    if (!ch) {
        return;
    }
    if (ch->usbredirhost) {
        usbredirhost_close(ch->usbredirhost);
    }

    if (ch->rules) {
        g_free(ch->rules);
    }

    g_free(ch);
    SPICE_DEBUG("%s << %p", __FUNCTION__, ch);
}

void
spice_usb_backend_channel_get_guest_filter(SpiceUsbBackendChannel *ch,
                                           const struct usbredirfilter_rule **r,
                                           int *count)
{
    int i;
    *r = NULL;
    *count = 0;
    if (ch->usbredirhost) {
        usbredirhost_get_guest_filter(ch->usbredirhost, r, count);
    }
    if (*r == NULL) {
        *r = ch->rules;
        *count = ch->rules_count;
    }

    SPICE_DEBUG("%s ch %p: %d filters", __FUNCTION__, ch, *count);

    for (i = 0; i < *count; i++) {
        const struct usbredirfilter_rule *ra = *r;
        SPICE_DEBUG("%s class %d, %X:%X",
            ra[i].allow ? "allowed" : "denied", ra[i].device_class,
            (uint32_t)ra[i].vendor_id, (uint32_t)ra[i].product_id);
    }
}

#endif /* USB_REDIR */
