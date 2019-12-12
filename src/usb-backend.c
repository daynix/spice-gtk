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
#include "usb-emulation.h"
#include "channel-usbredir-priv.h"
#include "spice-channel-priv.h"
#include "usbutil.h"

#define LOUD_DEBUG(x, ...)
#define USBREDIR_CALLBACK_NOT_IMPLEMENTED() spice_debug("%s not implemented - FIXME", __func__)

struct _SpiceUsbDevice
{
    /* Pointer to device. Either real device (libusb_device)
     * or emulated one (edev) */
    libusb_device *libusb_device;
    SpiceUsbEmulatedDevice *edev;
    gint ref_count;
    SpiceUsbBackendChannel *attached_to;
    UsbDeviceInformation device_info;
    bool cached_isochronous_valid;
    bool cached_isochronous;
    gboolean edev_configured;
};

struct _SpiceUsbBackend
{
    libusb_context *libusb_context;
    usb_hot_plug_callback hotplug_callback;
    void *hotplug_user_data;
    libusb_hotplug_callback_handle hotplug_handle;
    GThread *event_thread;
    gint event_thread_run;

#ifdef G_OS_WIN32
    HANDLE hWnd;
    libusb_device **libusb_device_list;
    gint redirecting;
#endif

    /* Mask of allocated device, a specific bit set to 1 to indicate that the device at
     * that address is allocated */
    uint32_t own_devices_mask;
};

typedef enum {
    USB_CHANNEL_STATE_INITIALIZING,
    USB_CHANNEL_STATE_HOST,
    USB_CHANNEL_STATE_PARSER,
} SpiceUsbBackendChannelState;

struct _SpiceUsbBackendChannel
{
    struct usbredirhost *usbredirhost;
    struct usbredirparser *parser;
    SpiceUsbBackendChannelState state;
    uint8_t *read_buf;
    int read_buf_size;
    struct usbredirfilter_rule *rules;
    int rules_count;
    uint32_t rejected          : 1;
    uint32_t wait_disconnect_ack : 1;
    SpiceUsbDevice *attached;
    SpiceUsbredirChannel *usbredir_channel;
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

static gboolean fill_usb_info(SpiceUsbDevice *dev)
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

static SpiceUsbDevice *allocate_backend_device(libusb_device *libdev)
{
    SpiceUsbDevice *dev = g_new0(SpiceUsbDevice, 1);
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
    SpiceUsbDevice *dev;
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

static inline void set_redirecting(SpiceUsbBackend *be, gboolean on)
{
    /* nothing on Linux */
}

#endif

/* lock functions for usbredirhost and usbredirparser */
static void *usbredir_alloc_lock(void)
{
    GMutex *mutex;

    mutex = g_new0(GMutex, 1);
    g_mutex_init(mutex);

    return mutex;
}

static void usbredir_free_lock(void *user_data)
{
    GMutex *mutex = user_data;

    g_mutex_clear(mutex);
    g_free(mutex);
}

static void usbredir_lock_lock(void *user_data)
{
    GMutex *mutex = user_data;

    g_mutex_lock(mutex);
}

static void usbredir_unlock_lock(void *user_data)
{
    GMutex *mutex = user_data;

    g_mutex_unlock(mutex);
}

gboolean spice_usb_backend_device_isoch(SpiceUsbDevice *dev)
{
    libusb_device *libdev = dev->libusb_device;
    struct libusb_config_descriptor *conf_desc;
    gboolean isoc_found = FALSE;
    gint i, j, k;
    int rc;

    g_return_val_if_fail(libdev != NULL || dev->edev != NULL, FALSE);

    if (dev->edev != NULL) {
        /* currently we do not emulate isoch devices */
        return FALSE;
    }

    if (dev->cached_isochronous_valid) {
        return dev->cached_isochronous;
    }


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

    dev->cached_isochronous_valid = true;
    dev->cached_isochronous = isoc_found;

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
    if (ch->parser == NULL) {
        return;
    }
    if (is_channel_ready(ch->usbredir_channel)) {
        if (ch->state == USB_CHANNEL_STATE_HOST) {
            SPICE_DEBUG("%s ch %p -> usbredirhost", __FUNCTION__, ch);
            usbredirhost_write_guest_data(ch->usbredirhost);
        } else {
            SPICE_DEBUG("%s ch %p -> parser", __FUNCTION__, ch);
            usbredirparser_do_write(ch->parser);
        }
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
        /* exclude addresses 0 (reserved) and 1 (root hub) */
        be->own_devices_mask = 3;
    }
    SPICE_DEBUG("%s <<", __FUNCTION__);
    return be;
}

static gpointer handle_libusb_events(gpointer user_data)
{
    SpiceUsbBackend *be = user_data;
    SPICE_DEBUG("%s >>", __FUNCTION__);
    int res = 0;
    const char *desc = "";
    while (g_atomic_int_get(&be->event_thread_run)) {
        res = libusb_handle_events(be->libusb_context);
        if (res && res != LIBUSB_ERROR_INTERRUPTED) {
            desc = libusb_strerror(res);
            g_warning("Error handling USB events: %s [%i]", desc, res);
            break;
        }
    }
    if (be->event_thread_run) {
        SPICE_DEBUG("%s: the thread aborted, %s(%d)", __FUNCTION__, desc, res);
    }
    SPICE_DEBUG("%s <<", __FUNCTION__);
    return NULL;
}

void spice_usb_backend_deregister_hotplug(SpiceUsbBackend *be)
{
    g_return_if_fail(be != NULL);
    if (be->hotplug_handle) {
        disable_hotplug_support(be);
        be->hotplug_handle = 0;
    }
    be->hotplug_callback = NULL;
    g_atomic_int_set(&be->event_thread_run, FALSE);
    if (be->event_thread) {
        libusb_interrupt_event_handler(be->libusb_context);
        g_thread_join(be->event_thread);
        be->event_thread = NULL;
    }
}

gboolean spice_usb_backend_register_hotplug(SpiceUsbBackend *be,
                                            void *user_data,
                                            usb_hot_plug_callback proc,
                                            GError **error)
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
        g_set_error(error, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    _("Error on USB hotplug detection: %s [%i]"), desc, rc);
        return FALSE;
    }

    g_atomic_int_set(&be->event_thread_run, TRUE);
    be->event_thread = g_thread_try_new("usb_ev_thread",
                                        handle_libusb_events,
                                        be, error);
    if (!be->event_thread) {
        g_warning("Error starting event thread");
        spice_usb_backend_deregister_hotplug(be);
        return FALSE;
    }
    return TRUE;
}

void spice_usb_backend_delete(SpiceUsbBackend *be)
{
    g_return_if_fail(be != NULL);
    SPICE_DEBUG("%s >>", __FUNCTION__);
    /*
        we expect hotplug callbacks are deregistered
        and the event thread is terminated. If not,
        we do that anyway before delete the backend
    */
    g_warn_if_fail(be->hotplug_handle == 0);
    g_warn_if_fail(be->event_thread == NULL);
    spice_usb_backend_deregister_hotplug(be);
    if (be->libusb_context) {
        libusb_exit(be->libusb_context);
    }
    g_free(be);
    SPICE_DEBUG("%s <<", __FUNCTION__);
}

const UsbDeviceInformation* spice_usb_backend_device_get_info(const SpiceUsbDevice *dev)
{
    return &dev->device_info;
}

gconstpointer spice_usb_backend_device_get_libdev(const SpiceUsbDevice *dev)
{
    return dev->libusb_device;
}

SpiceUsbDevice *spice_usb_backend_device_ref(SpiceUsbDevice *dev)
{
    LOUD_DEBUG("%s >> %p", __FUNCTION__, dev);
    g_atomic_int_inc(&dev->ref_count);
    return dev;
}

void spice_usb_backend_device_unref(SpiceUsbDevice *dev)
{
    LOUD_DEBUG("%s >> %p(%d)", __FUNCTION__, dev, dev->ref_count);
    if (g_atomic_int_dec_and_test(&dev->ref_count)) {
        if (dev->libusb_device) {
            libusb_unref_device(dev->libusb_device);
            LOUD_DEBUG("%s freeing %p (libusb %p)", __FUNCTION__, dev, dev->libusb_device);
        }
        if (dev->edev) {
            device_ops(dev->edev)->unrealize(dev->edev);
        }
        g_free(dev);
    }
}

static int check_edev_device_filter(SpiceUsbDevice *dev,
                                    const struct usbredirfilter_rule *rules,
                                    int count)
{
    SpiceUsbEmulatedDevice *edev = dev->edev;
    uint8_t cls[32], subcls[32], proto[32], *cfg, ifnum = 0;
    uint16_t size, offset = 0;

    if (!device_ops(edev)->get_descriptor(edev, LIBUSB_DT_CONFIG, 0, (void **)&cfg, &size)) {
        return -EINVAL;
    }

    while ((offset + 1) < size) {
        uint8_t len  = cfg[offset];
        uint8_t type = cfg[offset + 1];
        if ((offset + len) > size) {
            break;
        }
        if (type == LIBUSB_DT_INTERFACE) {
            cls[ifnum] = cfg[offset + 5];
            subcls[ifnum] = cfg[offset + 6];
            proto[ifnum] = cfg[offset + 7];
            ifnum++;
        }
        offset += len;
    }

    return usbredirfilter_check(rules, count,
                                dev->device_info.class,
                                dev->device_info.subclass,
                                dev->device_info.protocol,
                                cls, subcls, proto, ifnum,
                                dev->device_info.vid,
                                dev->device_info.pid,
                                dev->device_info.bcdUSB, 0);
}

int spice_usb_backend_device_check_filter(SpiceUsbDevice *dev,
                                          const struct usbredirfilter_rule *rules, int count)
{
    if (dev->libusb_device != NULL) {
        return usbredirhost_check_device_filter(rules, count, dev->libusb_device, 0);
    } else if (dev->edev != NULL) {
        return check_edev_device_filter(dev, rules, count);
    }
    g_warn_if_reached();
    return -EINVAL;
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

static struct usbredirparser *create_parser(SpiceUsbBackendChannel *ch);

static int usbredir_write_callback(void *user_data, uint8_t *data, int count)
{
    SpiceUsbBackendChannel *ch = user_data;
    int res;
    SPICE_DEBUG("%s ch %p, %d bytes", __FUNCTION__, ch, count);

    // handle first packet (HELLO) creating parser from capabilities
    if (SPICE_UNLIKELY(ch->parser == NULL)) {
        // Here we return 0 from this function to keep the packet in
        // the queue. The packet will then be sent to the guest.
        // We are initializing SpiceUsbBackendChannel, the
        // SpiceUsbredirChannel is not ready to receive packets.

        // we are still initializing the host
        if (ch->usbredirhost == NULL) {
            return 0;
        }

        ch->parser = create_parser(ch);
        if (!ch->parser) {
            return 0;
        }

        /* hello is short header (12) + hello struct (64) */
        const int hello_size = 12 + sizeof(struct usb_redir_hello_header);
        g_assert(count >= hello_size + 4);
        g_assert(SPICE_ALIGNED_CAST(struct usb_redir_header *, data)->type == usb_redir_hello);

        const uint32_t flags =
            usbredirparser_fl_write_cb_owns_buffer | usbredirparser_fl_usb_host |
            usbredirparser_fl_no_hello;

        usbredirparser_init(ch->parser,
                            PACKAGE_STRING,
                            SPICE_ALIGNED_CAST(uint32_t *, data + hello_size),
                            (count - hello_size) / sizeof(uint32_t),
                            flags);

        return 0;
    }
    res = spice_usbredir_write(ch->usbredir_channel, data, count);
    return res;
}

static uint64_t usbredir_buffered_output_size_callback(void *user_data)
{
    SpiceUsbBackendChannel *ch = user_data;
    return spice_channel_get_queue_size(SPICE_CHANNEL(ch->usbredir_channel));
}

int spice_usb_backend_read_guest_data(SpiceUsbBackendChannel *ch, uint8_t *data, int count)
{
    int res = 0;

    g_return_val_if_fail(ch->read_buf == NULL, USB_REDIR_ERROR_READ_PARSE);

    ch->read_buf = data;
    ch->read_buf_size = count;
    if (SPICE_UNLIKELY(ch->state == USB_CHANNEL_STATE_INITIALIZING)) {
        if (ch->usbredirhost != NULL) {
            res = usbredirhost_read_guest_data(ch->usbredirhost);
            if (res != 0) {
                return res;
            }
            ch->state = USB_CHANNEL_STATE_HOST;

            /* usbredirhost should consume hello response */
            g_return_val_if_fail(ch->read_buf == NULL, USB_REDIR_ERROR_READ_PARSE);
        } else {
            ch->state = USB_CHANNEL_STATE_PARSER;
        }

        ch->read_buf = data;
        ch->read_buf_size = count;
        if (ch->attached && ch->attached->edev) {
            /* case of CD sharing on connect */
            ch->state = USB_CHANNEL_STATE_PARSER;
            SPICE_DEBUG("%s: switch %p to parser", __FUNCTION__, ch);
        }
        return usbredirparser_do_read(ch->parser);
    }
    if (ch->state == USB_CHANNEL_STATE_HOST) {
        res = usbredirhost_read_guest_data(ch->usbredirhost);
    } else {
        res = usbredirparser_do_read(ch->parser);
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

    if (ch->rejected) {
        ch->rejected = 0;
        res = USB_REDIR_ERROR_DEV_REJECTED;
    }

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
    if (ch->state == USB_CHANNEL_STATE_HOST) {
        SPICE_DEBUG("%s ch %p -> usbredirhost", __FUNCTION__, ch);
        usbredirhost_free_write_buffer(ch->usbredirhost, data);
    } else {
        SPICE_DEBUG("%s ch %p -> parser", __FUNCTION__, ch);
        usbredirparser_free_write_buffer(ch->parser, data);
    }
}

static void
usbredir_control_packet(void *priv, uint64_t id, struct usb_redir_control_packet_header *h,
                        uint8_t *data, int data_len)
{
    SpiceUsbBackendChannel *ch = priv;
    SpiceUsbDevice *d = ch->attached;
    SpiceUsbEmulatedDevice *edev = d ? d->edev : NULL;
    struct usb_redir_control_packet_header response = *h;
    uint8_t reqtype = h->requesttype & 0x7f;
    gboolean done = FALSE;
    void *out_buffer = NULL;

    response.status = usb_redir_stall;
    SPICE_DEBUG("%s %p: TRVIL %02X %02X %04X %04X %04X",
                __FUNCTION__,
                ch, h->requesttype, h->request,
                h->value, h->index, h->length);

    if (!edev) {
        SPICE_DEBUG("%s: device not attached", __FUNCTION__);
        response.status = usb_redir_ioerror;
        done = TRUE;
    } else if (reqtype == (LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE) &&
               h->request == LIBUSB_REQUEST_GET_DESCRIPTOR) {
        uint16_t size;
        done = device_ops(edev)->get_descriptor(edev, h->value >> 8, h->value & 0xff,
                                                &out_buffer, &size);
        response.length = size;
        if (done) {
            response.status = 0;
        }
        done = TRUE;
    }

    if (!done) {
        device_ops(edev)->control_request(edev, data, data_len, &response, &out_buffer);
        done = TRUE;
    }

    if (response.status) {
        response.length = 0;
    } else if (response.length > h->length) {
        response.length = h->length;
    }

    SPICE_DEBUG("%s responding with payload of %02X, status %X",
                __FUNCTION__, response.length, response.status);
    usbredirparser_send_control_packet(ch->parser, id, &response,
                                       response.length ? out_buffer : NULL,
                                       response.length);

    usbredir_write_flush_callback(ch);
    usbredirparser_free_packet_data(ch->parser, data);
}

static void
usbredir_bulk_packet(void *priv, uint64_t id, struct usb_redir_bulk_packet_header *h,
                     uint8_t *data, int data_len)
{
    SpiceUsbBackendChannel *ch = priv;
    SpiceUsbDevice *d = ch->attached;
    SpiceUsbEmulatedDevice *edev = d ? d->edev : NULL;
    struct usb_redir_bulk_packet_header hout = *h;
    uint32_t len = (h->length_high << 16) | h->length;
    SPICE_DEBUG("%s %p: ep %X, len %u, id %" G_GUINT64_FORMAT, __FUNCTION__,
                ch, h->endpoint, len, id);

    if (!edev) {
        SPICE_DEBUG("%s: device not attached", __FUNCTION__);
        hout.status = usb_redir_ioerror;
        hout.length = hout.length_high = 0;
        SPICE_DEBUG("%s: responding with ZLP status %d", __FUNCTION__, hout.status);
    } else if (h->endpoint & LIBUSB_ENDPOINT_IN) {
        if (device_ops(edev)->bulk_in_request(edev, id, &hout)) {
            usbredirparser_free_packet_data(ch->parser, data);
            /* completion is asynchronous */
            return;
        }
    } else {
        hout.status = usb_redir_stall;
        device_ops(edev)->bulk_out_request(edev, h->endpoint, data, data_len, &hout.status);
        SPICE_DEBUG("%s: responding status %d", __FUNCTION__, hout.status);
    }

    usbredirparser_send_bulk_packet(ch->parser, id, &hout, NULL, 0);
    usbredirparser_free_packet_data(ch->parser, data);
    usbredir_write_flush_callback(ch);
}

static void usbredir_device_reset(void *priv)
{
    SpiceUsbBackendChannel *ch = priv;
    SpiceUsbDevice *d = ch->attached;
    SpiceUsbEmulatedDevice *edev = d ? d->edev : NULL;
    SPICE_DEBUG("%s ch %p", __FUNCTION__, ch);
    if (edev) {
        device_ops(edev)->reset(edev);
    }
}

static void
usbredir_interface_info(void *priv, struct usb_redir_interface_info_header *interface_info)
{
    USBREDIR_CALLBACK_NOT_IMPLEMENTED();
}

static void
usbredir_interface_ep_info(void *priv, struct usb_redir_ep_info_header *ep_info)
{
    USBREDIR_CALLBACK_NOT_IMPLEMENTED();
}

static void
usbredir_set_configuration(void *priv, uint64_t id,
                           struct usb_redir_set_configuration_header *set_configuration)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_configuration_status_header h;
    h.status = 0;
    h.configuration = set_configuration->configuration;
    SPICE_DEBUG("%s ch %p, cfg %d", __FUNCTION__, ch, h.configuration);
    if (ch->attached) {
        ch->attached->edev_configured = h.configuration != 0;
    }
    usbredirparser_send_configuration_status(ch->parser, id, &h);
    usbredir_write_flush_callback(ch);
}

static void usbredir_get_configuration(void *priv, uint64_t id)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_configuration_status_header h;
    h.status = 0;
    h.configuration = ch->attached && ch->attached->edev_configured;
    SPICE_DEBUG("%s ch %p, cfg %d", __FUNCTION__, ch, h.configuration);
    usbredirparser_send_configuration_status(ch->parser, id, &h);
    usbredir_write_flush_callback(ch);
}

static void
usbredir_set_alt_setting(void *priv, uint64_t id, struct usb_redir_set_alt_setting_header *s)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_alt_setting_status_header sh;
    sh.status = (!s->interface && !s->alt) ? 0 : usb_redir_stall;
    sh.interface = s->interface;
    sh.alt = s->alt;
    SPICE_DEBUG("%s ch %p, %d:%d", __FUNCTION__, ch, s->interface, s->alt);
    usbredirparser_send_alt_setting_status(ch->parser, id, &sh);
    usbredir_write_flush_callback(ch);
}

static void
usbredir_get_alt_setting(void *priv, uint64_t id, struct usb_redir_get_alt_setting_header *s)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_alt_setting_status_header sh;
    sh.status = (s->interface == 0) ? 0 : usb_redir_stall;
    sh.interface = s->interface;
    sh.alt = 0;
    SPICE_DEBUG("%s ch %p, if %d", __FUNCTION__, ch, s->interface);
    usbredirparser_send_alt_setting_status(ch->parser, id, &sh);
    usbredir_write_flush_callback(ch);
}

static void usbredir_cancel_data(void *priv, uint64_t id)
{
    SpiceUsbBackendChannel *ch = priv;
    SpiceUsbDevice *d = ch->attached;
    SpiceUsbEmulatedDevice *edev = d ? d->edev : NULL;
    if (!edev) {
        SPICE_DEBUG("%s: device not attached", __FUNCTION__);
        return;
    }
    device_ops(edev)->cancel_request(edev, id);
}

static void usbredir_filter_reject(void *priv)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s %p", __FUNCTION__, ch);
    ch->rejected = 1;
}

/* Note that the ownership of the rules array is passed on to the callback. */
static void
usbredir_filter_filter(void *priv, struct usbredirfilter_rule *r, int count)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s ch %p %d filters", __FUNCTION__, ch, count);

    free(ch->rules);
    ch->rules = r;
    ch->rules_count = count;
    if (count) {
        int i;
        for (i = 0; i < count; i++) {
            SPICE_DEBUG("%s class %d, %X:%X",
                r[i].allow ? "allowed" : "denied", r[i].device_class,
                (uint32_t)r[i].vendor_id, (uint32_t)r[i].product_id);
        }
    }
}

static void usbredir_device_disconnect_ack(void *priv)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s ch %p", __FUNCTION__, ch);
    if (ch->state == USB_CHANNEL_STATE_PARSER && ch->usbredirhost != NULL &&
        ch->wait_disconnect_ack) {
        ch->state = USB_CHANNEL_STATE_HOST;
        SPICE_DEBUG("%s switch to usbredirhost", __FUNCTION__);
    }
    ch->wait_disconnect_ack = 0;
}

static void
usbredir_hello(void *priv, struct usb_redir_hello_header *hello)
{
    SpiceUsbBackendChannel *ch = priv;
    SpiceUsbDevice *d = ch->attached;
    SpiceUsbEmulatedDevice *edev = d ? d->edev : NULL;
    struct usb_redir_device_connect_header device_connect;
    struct usb_redir_ep_info_header ep_info = { 0 };
    struct usb_redir_interface_info_header interface_info = { 0 };
    uint8_t *cfg;
    uint16_t size, offset = 0;
    SPICE_DEBUG("%s %p %sattached %s", __FUNCTION__, ch,
        edev ? "" : "not ",  hello ? "" : "(internal)");

    if (!edev) {
        return;
    }
    if (!device_ops(edev)->get_descriptor(edev, LIBUSB_DT_CONFIG, 0, (void **)&cfg, &size)) {
        return;
    }
    while ((offset + 1) < size) {
        uint8_t len  = cfg[offset];
        uint8_t type = cfg[offset + 1];
        if ((offset + len) > size) {
            break;
        }
        if (type == LIBUSB_DT_INTERFACE) {
            uint32_t i = interface_info.interface_count;
            uint8_t class, subclass, protocol;
            class = cfg[offset + 5];
            subclass = cfg[offset + 6];
            protocol = cfg[offset + 7];
            interface_info.interface_class[i] = class;
            interface_info.interface_subclass[i] = subclass;
            interface_info.interface_protocol[i] = protocol;
            interface_info.interface_count++;
            SPICE_DEBUG("%s IF%d: %d/%d/%d", __FUNCTION__, i, class, subclass, protocol);
        } else if (type == LIBUSB_DT_ENDPOINT) {
            uint8_t address = cfg[offset + 2];
            uint16_t max_packet_size = 0x100 * cfg[offset + 5] + cfg[offset + 4];
            uint8_t index = address & 0xf;
            if (address & 0x80) index += 0x10;
            ep_info.type[index] = cfg[offset + 3] & 0x3;
            ep_info.max_packet_size[index] = max_packet_size;
            SPICE_DEBUG("%s EP[%02X]: %d/%d", __FUNCTION__, index, ep_info.type[index], max_packet_size);
        }
        offset += len;
    }

    usbredirparser_send_interface_info(ch->parser, &interface_info);
    usbredirparser_send_ep_info(ch->parser, &ep_info);

    device_connect.device_class = 0; //d->device_info.class;
    device_connect.device_subclass = 0; //d->device_info.subclass;
    device_connect.device_protocol = 0; //d->device_info.protocol;;
    device_connect.vendor_id = d->device_info.vid;
    device_connect.product_id = d->device_info.pid;
    device_connect.device_version_bcd = d->device_info.bcdUSB;
    device_connect.speed = usb_redir_speed_high;
    usbredirparser_send_device_connect(ch->parser, &device_connect);
    usbredir_write_flush_callback(ch);
}

static void initialize_parser(SpiceUsbBackendChannel *ch)
{
    uint32_t flags, caps[USB_REDIR_CAPS_SIZE] = { 0 };

    g_assert(ch->usbredirhost == NULL);

    flags = usbredirparser_fl_write_cb_owns_buffer | usbredirparser_fl_usb_host;

    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_filter);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_device_disconnect_ack);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_bulk_receiving);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_bulk_streams);

    usbredirparser_init(ch->parser, PACKAGE_STRING, caps, USB_REDIR_CAPS_SIZE, flags);
}

/*
    We can initialize the usbredirparser with HELLO enabled only in case
    the libusb is not active and the usbredirhost does not function.
    Then the parser sends session HELLO and receives server's response.
    Otherwise (usbredirparser initialized with HELLO disabled):
    - the usbredirhost sends session HELLO
    - we look into it to know set of capabilities we shall initialize
      the parser with
    - when we receive server's response to HELLO we provide it also to
      parser to let it synchronize with server's capabilities
*/
static struct usbredirparser *create_parser(SpiceUsbBackendChannel *ch)
{
    struct usbredirparser *parser = usbredirparser_create();

    g_return_val_if_fail(parser != NULL, NULL);

    parser->priv = ch;
    parser->log_func = usbredir_log;
    parser->read_func = usbredir_read_callback;
    parser->write_func = usbredir_write_callback;
    parser->reset_func = usbredir_device_reset;
    parser->set_configuration_func = usbredir_set_configuration;
    parser->get_configuration_func = usbredir_get_configuration;
    parser->set_alt_setting_func = usbredir_set_alt_setting;
    parser->get_alt_setting_func = usbredir_get_alt_setting;
    parser->cancel_data_packet_func = usbredir_cancel_data;
    parser->control_packet_func = usbredir_control_packet;
    parser->bulk_packet_func = usbredir_bulk_packet;
    parser->alloc_lock_func = usbredir_alloc_lock;
    parser->lock_func = usbredir_lock_lock;
    parser->unlock_func = usbredir_unlock_lock;
    parser->free_lock_func = usbredir_free_lock;
    parser->hello_func = usbredir_hello;
    parser->filter_reject_func = usbredir_filter_reject;
    parser->device_disconnect_ack_func = usbredir_device_disconnect_ack;
    parser->interface_info_func = usbredir_interface_info;
    parser->ep_info_func = usbredir_interface_ep_info;
    parser->filter_filter_func = usbredir_filter_filter;

    return parser;
}

static gboolean attach_edev(SpiceUsbBackendChannel *ch,
                            SpiceUsbDevice *dev,
                            GError **error)
{
    if (!dev->edev) {
        g_set_error(error, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
           _("Failed to redirect device %d"), 1);
        return FALSE;
    }
    if (ch->state == USB_CHANNEL_STATE_INITIALIZING) {
        /*
            we can't initialize parser until we see hello from usbredir
            and the parser can't work until it sees the hello response.
            this is case of autoconnect when emulated device is attached
            before the channel is up
        */
        SPICE_DEBUG("%s waiting until the channel is ready", __FUNCTION__);

    } else {
        ch->state = USB_CHANNEL_STATE_PARSER;
    }
    ch->wait_disconnect_ack = 0;
    ch->attached = dev;
    dev->attached_to = ch;
    device_ops(dev->edev)->attach(dev->edev, ch->parser);
    if (ch->state == USB_CHANNEL_STATE_PARSER) {
        /* send device info */
        usbredir_hello(ch, NULL);
    }
    return TRUE;
}

gboolean spice_usb_backend_channel_attach(SpiceUsbBackendChannel *ch,
                                          SpiceUsbDevice *dev,
                                          GError **error)
{
    int rc;
    SPICE_DEBUG("%s >> ch %p, dev %p (was attached %p)", __FUNCTION__, ch, dev, ch->attached);

    g_return_val_if_fail(dev != NULL, FALSE);

    if (!dev->libusb_device) {
        return attach_edev(ch, dev, error);
    }

    // no physical device enabled
    if (ch->usbredirhost == NULL) {
        return FALSE;
    }

    libusb_device_handle *handle = NULL;
    if (ch->state != USB_CHANNEL_STATE_INITIALIZING) {
        ch->state = USB_CHANNEL_STATE_HOST;
    }

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
    SpiceUsbDevice *d = ch->attached;
    SpiceUsbEmulatedDevice *edev = d ? d->edev : NULL;
    SPICE_DEBUG("%s >> ch %p, was attached %p", __FUNCTION__, ch, ch->attached);
    if (!d) {
        SPICE_DEBUG("%s: nothing to detach", __FUNCTION__);
        return;
    }
    if (ch->state == USB_CHANNEL_STATE_HOST) {
        /* it will call libusb_close internally */
        usbredirhost_set_device(ch->usbredirhost, NULL);
    } else {
        if (edev) {
            device_ops(edev)->detach(edev);
        }
        usbredirparser_send_device_disconnect(ch->parser);
        usbredir_write_flush_callback(ch);
        ch->wait_disconnect_ack =
            usbredirparser_peer_has_cap(ch->parser, usb_redir_cap_device_disconnect_ack);
        if (!ch->wait_disconnect_ack && ch->usbredirhost != NULL) {
            ch->state = USB_CHANNEL_STATE_HOST;
        }
    }
    SPICE_DEBUG("%s ch %p, detach done", __FUNCTION__, ch);
    ch->attached->attached_to = NULL;
    ch->attached = NULL;
    ch->rejected = 0;
}

SpiceUsbBackendChannel *
spice_usb_backend_channel_new(SpiceUsbBackend *be,
                              SpiceUsbredirChannel *usbredir_channel)
{
    SpiceUsbBackendChannel *ch;

    ch = g_new0(SpiceUsbBackendChannel, 1);
    SPICE_DEBUG("%s >>", __FUNCTION__);
    ch->usbredir_channel = usbredir_channel;
    if (be->libusb_context) {
        ch->backend = be;
        ch->usbredirhost =
            usbredirhost_open_full(be->libusb_context,
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
                                   spice_util_get_debug() ? usbredirparser_debug :
                                        usbredirparser_warning,
                                   usbredirhost_fl_write_cb_owns_buffer);
        g_warn_if_fail(ch->usbredirhost != NULL);
        if (ch->usbredirhost != NULL) {
            usbredirhost_set_buffered_output_size_cb(ch->usbredirhost,
                                                     usbredir_buffered_output_size_callback);
            // force flush of HELLO packet and creation of parser
            usbredirhost_write_guest_data(ch->usbredirhost);
        }
    } else {
        // no physical device support, only emulated, create the
        // parser
        ch->parser = create_parser(ch);
        if (ch->parser != NULL) {
            initialize_parser(ch);
        }
    }

    if (!ch->parser) {
        spice_usb_backend_channel_delete(ch);
        ch = NULL;
    }

    SPICE_DEBUG("%s << %p", __FUNCTION__, ch);
    return ch;
}

void spice_usb_backend_channel_flush_writes(SpiceUsbBackendChannel *ch)
{
    SPICE_DEBUG("%s %p is up", __FUNCTION__, ch);
    if (ch->state != USB_CHANNEL_STATE_PARSER && ch->usbredirhost != NULL) {
        usbredirhost_write_guest_data(ch->usbredirhost);
    } else {
        usbredirparser_do_write(ch->parser);
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
    if (ch->parser) {
        usbredirparser_destroy(ch->parser);
    }

    if (ch->rules) {
        /* rules were allocated by usbredirparser */
        free(ch->rules);
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

gchar *spice_usb_backend_device_get_description(SpiceUsbDevice *dev,
                                                const gchar *format)
{
    guint16 bus, address, vid, pid;
    gchar *description, *descriptor, *manufacturer = NULL, *product = NULL;

    g_return_val_if_fail(dev != NULL, NULL);

    bus     = dev->device_info.bus;
    address = dev->device_info.address;
    vid     = dev->device_info.vid;
    pid     = dev->device_info.pid;

    if ((vid > 0) && (pid > 0)) {
        descriptor = g_strdup_printf("[%04x:%04x]", vid, pid);
    } else {
        descriptor = g_strdup("");
    }

    if (dev->libusb_device) {
        spice_usb_util_get_device_strings(bus, address, vid, pid,
                                          &manufacturer, &product);
    } else {
        product = device_ops(dev->edev)->get_product_description(dev->edev);
    }

    if (!format) {
        format = _("%s %s %s at %d-%d");
    }

    description = g_strdup_printf(format, manufacturer ? manufacturer : "",
                                  product, descriptor, bus, address);

    g_free(manufacturer);
    g_free(descriptor);
    g_free(product);

    return description;
}

void spice_usb_backend_device_report_change(SpiceUsbBackend *be,
                                            SpiceUsbDevice *dev)
{
    gchar *desc;
    g_return_if_fail(dev && dev->edev);

    desc = device_ops(dev->edev)->get_product_description(dev->edev);
    SPICE_DEBUG("%s: %s", __FUNCTION__, desc);
    g_free(desc);
}

void spice_usb_backend_device_eject(SpiceUsbBackend *be, SpiceUsbDevice *dev)
{
    g_return_if_fail(dev);

    if (dev->edev) {
        be->own_devices_mask &= ~(1 << dev->device_info.address);
    }
    if (be->hotplug_callback) {
        be->hotplug_callback(be->hotplug_user_data, dev, FALSE);
    }
}

gboolean
spice_usb_backend_create_emulated_device(SpiceUsbBackend *be,
                                         SpiceUsbEmulatedDeviceCreate create_proc,
                                         void *create_params,
                                         GError **err)
{
    SpiceUsbEmulatedDevice *edev;
    SpiceUsbDevice *dev;
    struct libusb_device_descriptor *desc;
    uint16_t device_desc_size;
    uint8_t address = 0;

    if (be->own_devices_mask == 0xffffffff) {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    _("can't create device - limit reached"));
        return FALSE;
    }
    for (address = 0; address < 32; ++address) {
        if (~be->own_devices_mask & (1 << address)) {
            break;
        }
    }

    dev = g_new0(SpiceUsbDevice, 1);
    dev->device_info.bus = BUS_NUMBER_FOR_EMULATED_USB;
    dev->device_info.address = address;
    dev->ref_count = 1;

    dev->edev = edev = create_proc(be, dev, create_params, err);
    if (edev == NULL) {
        spice_usb_backend_device_unref(dev);
        return FALSE;
    }

    if (!device_ops(edev)->get_descriptor(edev, LIBUSB_DT_DEVICE, 0,
                                          (void **)&desc, &device_desc_size)
        || device_desc_size != sizeof(*desc)) {

        spice_usb_backend_device_unref(dev);
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    _("can't create device - internal error"));
        return FALSE;
    }

    be->own_devices_mask |= 1 << address;

    dev->device_info.vid = desc->idVendor;
    dev->device_info.pid = desc->idProduct;
    dev->device_info.bcdUSB = desc->bcdUSB;
    dev->device_info.class = desc->bDeviceClass;
    dev->device_info.subclass = desc->bDeviceSubClass;
    dev->device_info.protocol = desc->bDeviceProtocol;

    if (be->hotplug_callback) {
        be->hotplug_callback(be->hotplug_user_data, dev, TRUE);
    }
    spice_usb_backend_device_unref(dev);

    return TRUE;
}
