/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
Copyright (C) 2018 Red Hat, Inc.

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
#include <errno.h>
#include <libusb.h>
#include <string.h>
#include <fcntl.h>
#include "usbredirhost.h"
#include "usbredirparser.h"
#include "spice-util.h"
#include "usb-backend.h"
#include "cd-usb-bulk-msd.h"
#if defined(G_OS_WIN32)
#include <windows.h>
#include "win-usb-dev.h"
#else
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#define MAX_LUN_PER_DEVICE      1
#if MAX_LUN_PER_DEVICE > 1
#define MAX_OWN_DEVICES         3
#else
#define MAX_OWN_DEVICES         32
#endif
#define OWN_BUS_NUM             0xff
#define USB2_BCD                0x200
#define CD_DEV_VID              0x1c6b
#define CD_DEV_PID              0xa223
#define CD_DEV_CLASS            8
#define CD_DEV_SUBCLASS         6
#define CD_DEV_PROTOCOL         0x50
#define CD_DEV_BLOCK_SIZE       0x200
#define CD_DEV_MAX_SIZE         737280000
#define DVD_DEV_BLOCK_SIZE      0x800

static void *g_mutex;

typedef struct _SpiceUsbLU
{
    char *filename;
    GFile *file_object;
    GFileInputStream *stream;
    uint64_t size;
    uint32_t blockSize;
    uint32_t padding;
} SpiceUsbLU;

struct _SpiceUsbBackendDevice
{
    union
    {
        void *libusb_device;
        void *msc;
    } d;
    uint32_t isLibUsb   : 1;
    uint32_t configured : 1;
    int refCount;
    void *mutex;
    SpiceUsbBackendChannel *attached_to;
    UsbDeviceInformation device_info;
    SpiceUsbLU units[MAX_LUN_PER_DEVICE];
};

static struct OwnUsbDevices
{
    unsigned long active_devices;
    SpiceUsbBackendDevice devices[MAX_OWN_DEVICES];
} own_devices;

struct _SpiceUsbBackend
{
    libusb_context *libusbContext;
    usb_hot_plug_callback hp_callback;
    void *hp_user_data;
    libusb_hotplug_callback_handle hp_handle;
};

struct _read_bulk 
{
    struct usb_redir_bulk_packet_header hout;
    uint64_t id;
};

struct _SpiceUsbBackendChannel
{
    struct usbredirhost *usbredirhost;
    struct usbredirhost *hiddenhost;
    struct usbredirparser *parser;
    struct usbredirparser *hiddenparser;
    uint8_t *read_buf;
    uint8_t *hello;
    int read_buf_size;
    int hello_size;
    struct usbredirfilter_rule *rules;
    int rules_count;
    uint32_t host_caps;
    uint32_t hello_done_host   : 1;
    uint32_t hello_done_parser : 1;
    uint32_t hello_sent        : 1;
    uint32_t rejected          : 1;
    SpiceUsbBackendDevice *attached;
    SpiceUsbBackendChannelInitData data;
    uint32_t num_reads;
    struct _read_bulk read_bulk[64];
};

static const char *spice_usbutil_libusb_strerror(enum libusb_error error_code)
{
    switch (error_code) {
    case LIBUSB_SUCCESS:
        return "Success";
    case LIBUSB_ERROR_IO:
        return "Input/output error";
    case LIBUSB_ERROR_INVALID_PARAM:
        return "Invalid parameter";
    case LIBUSB_ERROR_ACCESS:
        return "Access denied (insufficient permissions)";
    case LIBUSB_ERROR_NO_DEVICE:
        return "No such device (it may have been disconnected)";
    case LIBUSB_ERROR_NOT_FOUND:
        return "Entity not found";
    case LIBUSB_ERROR_BUSY:
        return "Resource busy";
    case LIBUSB_ERROR_TIMEOUT:
        return "Operation timed out";
    case LIBUSB_ERROR_OVERFLOW:
        return "Overflow";
    case LIBUSB_ERROR_PIPE:
        return "Pipe error";
    case LIBUSB_ERROR_INTERRUPTED:
        return "System call interrupted (perhaps due to signal)";
    case LIBUSB_ERROR_NO_MEM:
        return "Insufficient memory";
    case LIBUSB_ERROR_NOT_SUPPORTED:
        return "Operation not supported or unimplemented on this platform";
    case LIBUSB_ERROR_OTHER:
        return "Other error";
    }
    return "Unknown error";
}

// lock functions for usbredirhost and usbredirparser
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

static gboolean fill_usb_info(SpiceUsbBackendDevice *bdev)
{
    UsbDeviceInformation *pi = &bdev->device_info;

    if (bdev->isLibUsb)
    {
        struct libusb_device_descriptor desc;
        libusb_device *libdev = bdev->d.libusb_device;
        int res = libusb_get_device_descriptor(libdev, &desc);
        pi->bus = libusb_get_bus_number(libdev);
        pi->address = libusb_get_device_address(libdev);
        if (res < 0) {
            g_warning("cannot get device descriptor for (%p) %d.%d",
                libdev, pi->bus, pi->address);
            return FALSE;
        }
        pi->vid = desc.idVendor;
        pi->pid = desc.idProduct;
        pi->class = desc.bDeviceClass;
        pi->subclass = desc.bDeviceSubClass;
        pi->protocol = desc.bDeviceProtocol;
        pi->isochronous = 0;
    }
    return TRUE;
}

#if defined(G_OS_WIN32)
static gboolean open_stream(SpiceUsbLU *unit, const char *filename)
{
    gboolean b = FALSE;
    HANDLE h = CreateFileA(
        filename,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = {0};
        if (!GetFileSizeEx(h, &size)) {
            uint64_t buffer[256];
            unsigned long ret;
            if (DeviceIoControl(h,
                IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                NULL,
                0,
                buffer,
                sizeof(buffer),
                &ret,
                NULL))
            {
                DISK_GEOMETRY_EX *pg = (DISK_GEOMETRY_EX *)buffer;
                unit->blockSize = pg->Geometry.BytesPerSector;
                size = pg->DiskSize;
            }
        }
        unit->size = size.QuadPart;
        if (unit->filename) {
           g_free(unit->filename);
        }
        CloseHandle(h);
        unit->filename = g_strdup(filename);
        unit->file_object = g_file_new_for_path(filename);
        unit->stream = g_file_read(unit->file_object, NULL, NULL);
        b = unit->stream != NULL;
        if (!b) {
            SPICE_DEBUG("%s: can't open stream on %s", __FUNCTION__, filename);
            g_object_unref(unit->file_object);
            unit->file_object = NULL;
        }
    } else {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, filename);
    }
    return b;
}
#else
static gboolean open_stream(SpiceUsbLU *unit, const char *filename)
{
    gboolean b = FALSE;
    int fd = open(
        filename,
        O_RDONLY | O_NONBLOCK);
    if (fd > 0) {
        struct stat file_stat;
        if (fstat(fd, &file_stat) || file_stat.st_size == 0) {
            file_stat.st_size = 0;
            ioctl(fd, BLKGETSIZE64, &file_stat.st_size);
            ioctl(fd, BLKSSZGET, &unit->blockSize);
        }
        unit->size = file_stat.st_size;
        if (unit->filename) {
            g_free(unit->filename);
        }
        close(fd);
        unit->filename = g_strdup(filename);
        unit->file_object = g_file_new_for_path(filename);
        unit->stream = g_file_read(unit->file_object, NULL, NULL);
        b = unit->stream != NULL;
        if (!b) {
            SPICE_DEBUG("%s: can't open stream on %s", __FUNCTION__, filename);
            g_object_unref(unit->file_object);
            unit->file_object = NULL;
        }
    }
    else {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, filename);
    }

    return b;
}
#endif

static void close_stream(SpiceUsbLU *unit)
{
    if (unit->stream) {
        g_object_unref(unit->stream);
        unit->stream = NULL;
    }
    if (unit->file_object) {
        g_object_unref(unit->file_object);
        unit->file_object = NULL;
    }
}

/* Note that this function must be re-entrant safe, as it can get called
from both the main thread as well as from the usb event handling thread */
static void usbredir_write_flush_callback(void *user_data)
{
    SpiceUsbBackendChannel *ch = user_data;
    gboolean b = ch->data.is_channel_ready(ch->data.user_data);
    if (b) {
        if (ch->usbredirhost) {
            SPICE_DEBUG("%s ch %p -> usbredirhost", __FUNCTION__, ch);
            usbredirhost_write_guest_data(ch->usbredirhost);
        }
        else if (ch->parser) {
            SPICE_DEBUG("%s ch %p -> usbredirparser", __FUNCTION__, ch);
            usbredirparser_do_write(ch->parser);
        }
        else {
            b = FALSE;
        }
    }

    if (!b) {
        SPICE_DEBUG("%s ch %p (not ready)", __FUNCTION__, ch);
    }
}

void cd_usb_bulk_msd_read_complete(void *user_data,
    uint8_t *data, uint32_t length, cd_usb_bulk_status status)
{
    SpiceUsbBackendDevice *d = (SpiceUsbBackendDevice *)user_data;
    SpiceUsbBackendChannel *ch = d->attached_to;
    if (ch && ch->attached == d && ch->parser) {
        int nread;
        uint32_t h_length, offset = 0;

        for (nread = 0; nread <ch->num_reads; nread++) {
            h_length = (ch->read_bulk[nread].hout.length_high << 16) | 
                        ch->read_bulk[nread].hout.length; 
            if (h_length > length) {
                h_length = length;
                ch->read_bulk[nread].hout.length = length;
                ch->read_bulk[nread].hout.length_high = length >> 16;
            }

            switch (status) {
            case BULK_STATUS_GOOD:
                ch->read_bulk[nread].hout.status = 0;
                break;
            case BULK_STATUS_CANCELED:
                ch->read_bulk[nread].hout.status = usb_redir_cancelled;
                break;
            case BULK_STATUS_ERROR:
                ch->read_bulk[nread].hout.status = usb_redir_ioerror;
                break;
            case BULK_STATUS_STALL:
            default:
                ch->read_bulk[nread].hout.status = usb_redir_stall;
                break;
            }

            SPICE_DEBUG("%s: responding with hlen %u out of len %u, status %d",
                __FUNCTION__, h_length, length, ch->read_bulk[nread].hout.status);
            usbredirparser_send_bulk_packet(ch->parser, ch->read_bulk[nread].id,
                &ch->read_bulk[nread].hout, h_length ? (data + offset) : NULL, h_length);

            offset += h_length;
            length -= h_length;
        }
        ch->num_reads = 0;
        usbredir_write_flush_callback(ch);
    } else {
        SPICE_DEBUG("broken device<->channel relationship!");
    }
}

/* device reset completion callback */
void cd_usb_bulk_msd_reset_complete(void *user_data, int status)
{
    // SpiceUsbBackendDevice *d = (SpiceUsbBackendDevice *)user_data;
}

static gboolean activate_device(SpiceUsbBackendDevice *d, const char *filename, int unit)
{
    gboolean b = FALSE;
    if (!d->d.msc) {
        d->d.msc = cd_usb_bulk_msd_alloc(d, MAX_LUN_PER_DEVICE);
        if (!d->d.msc) {
            return FALSE;
        }
    }
    d->units[unit].blockSize = CD_DEV_BLOCK_SIZE;
    b = open_stream(&d->units[unit], filename);
    if (b) {
        cd_scsi_device_parameters params = { 0 };
        params.size = d->units[unit].size;
        params.block_size = d->units[unit].blockSize;
        if (params.block_size == CD_DEV_BLOCK_SIZE &&
            params.size % DVD_DEV_BLOCK_SIZE == 0) {
            params.block_size = DVD_DEV_BLOCK_SIZE;
        }
        params.stream = d->units[unit].stream;
        SPICE_DEBUG("%s: ready stream on %s, size %" PRIu64 ", block %u",
            __FUNCTION__, filename, params.size, params.block_size);

        b = !cd_usb_bulk_msd_realize(d->d.msc, unit, &params);
        if (!b) {
            close_stream(&d->units[unit]);
        }
    }
    return b;
}

void spice_usb_backend_add_cd(const char *filename, SpiceUsbBackend *be)
{
    int i;
    gboolean b = FALSE;
    for (i = 0; !b && i < MAX_OWN_DEVICES; i++) {
        if ((1 << i) & ~own_devices.active_devices) {
            b = activate_device(&own_devices.devices[i], filename, 0);
            if (b) {
                own_devices.active_devices |= 1 << i;
            }
#ifdef G_OS_WIN32
            spice_usb_backend_indicate_dev_change();
#else
            if (be->hp_callback) {
                SpiceUsbBackendDevice *d = &own_devices.devices[i];
                be->hp_callback(be->hp_user_data, d, TRUE);
            }
#endif
        }
    }
    for (i = 2; !b && i < MAX_OWN_DEVICES; i++) {
        if ((1 << i) & own_devices.active_devices) {
            int j;
            for (j = 0; !b && j < MAX_LUN_PER_DEVICE; j++) {
                if (!own_devices.devices[i].units[j].stream) {
                    b = activate_device(&own_devices.devices[i], filename, j);
                    if (!b) {
                        break;
                    }
                }
            }
        }
    }
    if (!b) {
        SPICE_DEBUG("can not create device %s", filename);
    }
}

static void initialize_own_devices(void)
{
    int i;
    // addresses 0 and 1 excluded as they are treated as
    // not suitable for redirection
    own_devices.active_devices = 3;
    for (i = 0; i < MAX_OWN_DEVICES; i++) {
        own_devices.devices[i].mutex = g_mutex;
        own_devices.devices[i].device_info.bus = OWN_BUS_NUM;
        own_devices.devices[i].device_info.address = i;
        own_devices.devices[i].device_info.vid = CD_DEV_VID;
        own_devices.devices[i].device_info.pid = CD_DEV_PID;
        own_devices.devices[i].device_info.class = 0;
        own_devices.devices[i].device_info.subclass = 0;
        own_devices.devices[i].device_info.protocol = 0;
    }
}

static void log_handler(
    const gchar *log_domain,
    GLogLevelFlags log_level,
    const gchar *message,
    gpointer user_data)
{
    GString *log_msg;
    log_msg = g_string_new(NULL);
    if (log_msg)
    {
        gchar *timestamp;
        GThread *th = g_thread_self();
        GDateTime *current_time = g_date_time_new_now_local();
        gint micros = g_date_time_get_microsecond(current_time);
        timestamp = g_date_time_format(current_time, "%H:%M:%S");
        g_string_append_printf(log_msg, "[%p][%s.%03d]", th, timestamp, micros / 1000);
        g_date_time_unref(current_time);
        g_free(timestamp);
        g_string_append(log_msg, message);
        g_log_default_handler(log_domain, log_level, log_msg->str, NULL);
        g_string_free(log_msg, TRUE);
    }
}

static void configure_log(void)
{
    g_log_set_default_handler(log_handler, NULL);
}

SpiceUsbBackend *spice_usb_backend_initialize(void)
{
    SpiceUsbBackend *be;
    SPICE_DEBUG("%s >>", __FUNCTION__);
    if (!g_mutex) {
        g_mutex = usbredir_alloc_lock();
        initialize_own_devices();
        configure_log();
    }
    be = (SpiceUsbBackend *)g_new0(SpiceUsbBackend, 1);
    if (be) {
        int rc;
        rc = libusb_init(&be->libusbContext);
        if (rc < 0) {
            const char *desc = spice_usbutil_libusb_strerror(rc);
            g_warning("Error initializing LIBUSB support: %s [%i]", desc, rc);
        }
    }
    SPICE_DEBUG("%s <<", __FUNCTION__);
    return be;
}

gboolean spice_usb_backend_handle_events(SpiceUsbBackend *be)
{
    SPICE_DEBUG("%s >>", __FUNCTION__);
    gboolean b = TRUE;
    if (be->libusbContext) {
        SPICE_DEBUG("%s >> libusb", __FUNCTION__);
        int res = libusb_handle_events(be->libusbContext);
        if (res && res != LIBUSB_ERROR_INTERRUPTED) {
            const char *desc = spice_usbutil_libusb_strerror(res);
            g_warning("Error handling USB events: %s [%i]", desc, res);
            b = FALSE;
        }
        SPICE_DEBUG("%s << libusb %d", __FUNCTION__, res);
    }
    else {
        b = TRUE;
        g_usleep(1000000);
    }
    SPICE_DEBUG("%s <<", __FUNCTION__);
    return b;
}

static int LIBUSB_CALL hotplug_callback(libusb_context *ctx,
    libusb_device *device,
    libusb_hotplug_event event,
    void *user_data)
{
    SpiceUsbBackend *be = (SpiceUsbBackend *)user_data;
    if (be->hp_callback) {
        SpiceUsbBackendDevice *d;
        gboolean val = event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED;
        d = g_new0(SpiceUsbBackendDevice, 1);
        if (d) {
            d->isLibUsb = 1;
            d->refCount = 1;
            d->mutex = g_mutex;
            d->d.libusb_device = device;
            if (fill_usb_info(d)) {
                SPICE_DEBUG("created dev %p, usblib dev %p", d, device);
                be->hp_callback(be->hp_user_data, d, val);
            } else {
                g_free(d);
            }
        }
    }
    return 0;
}

gboolean spice_usb_backend_handle_hotplug(
    SpiceUsbBackend *be,
    void *user_data,
    usb_hot_plug_callback proc)
{
    int rc;
    if (!proc) {
        if (be->hp_handle) {
            libusb_hotplug_deregister_callback(be->libusbContext, be->hp_handle);
            be->hp_handle = 0;
        }
        be->hp_callback = proc;
        return TRUE;
    }

    be->hp_callback = proc;
    be->hp_user_data = user_data;
    rc = libusb_hotplug_register_callback(be->libusbContext,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        hotplug_callback, be, &be->hp_handle);
    if (rc != LIBUSB_SUCCESS) {
        const char *desc = spice_usbutil_libusb_strerror(rc);
        g_warning("Error initializing USB hotplug support: %s [%i]", desc, rc);
        be->hp_callback = NULL;
        return FALSE;
    }
    return TRUE;
}

void spice_usb_backend_finalize(SpiceUsbBackend *be)
{
    SPICE_DEBUG("%s >>", __FUNCTION__);
    if (be->libusbContext) {
        libusb_exit(be->libusbContext);
    }
    g_free(be);
    SPICE_DEBUG("%s <<", __FUNCTION__);
}

SpiceUsbBackendDevice **spice_usb_backend_get_device_list(SpiceUsbBackend *be)
{
    SPICE_DEBUG("%s >>", __FUNCTION__);
    libusb_device **devlist = NULL, **dev;
    SpiceUsbBackendDevice *d, **list;

    int n = 0, i, index;

    if (be->libusbContext) {
        libusb_get_device_list(be->libusbContext, &devlist);
    }

    // add all the libusb device that not present in our list
    for (dev = devlist; dev && *dev; dev++) {
        n++;
    }

    list = g_new0(SpiceUsbBackendDevice*, n + MAX_OWN_DEVICES);
    if (!list) {
        libusb_free_device_list(devlist, 1);
        return NULL;
    }

    for (i = 2; i < MAX_OWN_DEVICES; ++i) {
        if (own_devices.active_devices & (1 << i)) {
            n++;
        }
    }

    index = 0;

    for (dev = devlist; dev && *dev; dev++) {
        d = g_new0(SpiceUsbBackendDevice, 1);
        if (d) {
            d->isLibUsb = 1;
            d->refCount = 1;
            d->mutex = g_mutex;
            d->d.libusb_device = *dev;
            if (index >= n || !fill_usb_info(d)) {
                g_free(d);
                libusb_unref_device(*dev);
            }
            else {
                SPICE_DEBUG("created dev %p, usblib dev %p", d, *dev);
                list[index++] = d;
            }
        }
    }

    usbredir_lock_lock(g_mutex);

    for (i = 2; i < MAX_OWN_DEVICES; ++i) {
        d = &own_devices.devices[i];
        if ((own_devices.active_devices & (1 << i)) && index < n) {
            list[index++] = d;
            d->refCount++;
            SPICE_DEBUG("found own %p, address %d", d, d->device_info.address);
        }
    }
    usbredir_unlock_lock(g_mutex);

    if (devlist) {
        libusb_free_device_list(devlist, 0);
    }

    SPICE_DEBUG("%s <<", __FUNCTION__);
    return list;
}

gboolean spice_usb_backend_device_is_hub(SpiceUsbBackendDevice *dev)
{
    return dev->device_info.class == LIBUSB_CLASS_HUB;
}

static unsigned char is_libusb_isochronous(libusb_device *libdev)
{
    struct libusb_config_descriptor *conf_desc;
    unsigned char isoc_found = FALSE;
    gint i, j, k;

    g_return_val_if_fail(libdev != NULL, FALSE);

    if (libusb_get_active_config_descriptor(libdev, &conf_desc) != 0) {
        g_return_val_if_reached(FALSE);
    }

    for (i = 0; !isoc_found && i < conf_desc->bNumInterfaces; i++) {
        for (j = 0; !isoc_found && j < conf_desc->interface[i].num_altsetting; j++) {
            for (k = 0; !isoc_found && k < conf_desc->interface[i].altsetting[j].bNumEndpoints;k++) {
                gint attributes = conf_desc->interface[i].altsetting[j].endpoint[k].bmAttributes;
                gint type = attributes & LIBUSB_TRANSFER_TYPE_MASK;
                if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
                    isoc_found = TRUE;
            }
        }
    }

    libusb_free_config_descriptor(conf_desc);
    return isoc_found;
}

const UsbDeviceInformation*  spice_usb_backend_device_get_info(SpiceUsbBackendDevice *dev)
{
    dev->device_info.isochronous = dev->isLibUsb ? is_libusb_isochronous(dev->d.libusb_device) : 0;
    return &dev->device_info;
}

gboolean spice_usb_backend_devices_same(
    SpiceUsbBackendDevice *dev1,
    SpiceUsbBackendDevice *dev2)
{
    if (dev1->isLibUsb != dev2->isLibUsb) {
        return FALSE;
    }
    if (dev1->isLibUsb) {
        return dev1->d.libusb_device == dev2->d.libusb_device;
    }
    // assuming CD redir devices are static
    return dev1 == dev2;
}

gconstpointer spice_usb_backend_device_get_libdev(SpiceUsbBackendDevice *dev)
{
    if (dev->isLibUsb) {
        return dev->d.libusb_device;
    }
    return NULL;
}

void spice_usb_backend_free_device_list(SpiceUsbBackendDevice **devlist)
{
    SPICE_DEBUG("%s >>", __FUNCTION__);
    SpiceUsbBackendDevice **dev;
    for (dev = devlist; *dev; dev++) {
        SpiceUsbBackendDevice *d = *dev;
        spice_usb_backend_device_release(d);
    }
    g_free(devlist);
    SPICE_DEBUG("%s <<", __FUNCTION__);
}

void spice_usb_backend_device_acquire(SpiceUsbBackendDevice *dev)
{
    void *mutex = dev->mutex;
    SPICE_DEBUG("%s >> %p", __FUNCTION__, dev);
    usbredir_lock_lock(mutex);
    if (dev->isLibUsb) {
        libusb_ref_device(dev->d.libusb_device);
    }
    dev->refCount++;
    usbredir_unlock_lock(mutex);
}

void spice_usb_backend_device_release(SpiceUsbBackendDevice *dev)
{
    void *mutex = dev->mutex;
    SPICE_DEBUG("%s >> %p(%d)", __FUNCTION__, dev, dev->refCount);
    usbredir_lock_lock(mutex);
    if (dev->isLibUsb) {
        libusb_unref_device(dev->d.libusb_device);
        dev->refCount--;
        if (dev->refCount == 0) {
            SPICE_DEBUG("%s freeing %p (libusb %p)", __FUNCTION__, dev, dev->d.libusb_device);
            g_free(dev);
        }
    }
    else {
        dev->refCount--;
    }
    usbredir_unlock_lock(mutex);
    SPICE_DEBUG("%s <<", __FUNCTION__);
}

gboolean spice_usb_backend_device_need_thread(SpiceUsbBackendDevice *dev)
{
    gboolean b = dev->isLibUsb != 0;
    SPICE_DEBUG("%s << %d", __FUNCTION__, b);
    return b;
}

int spice_usb_backend_device_check_filter(
    SpiceUsbBackendDevice *dev,
    const struct usbredirfilter_rule *rules,
    int count)
{
    if (dev->isLibUsb) {
        return usbredirhost_check_device_filter(
            rules, count, dev->d.libusb_device, 0);
    } else {
        uint8_t cls, subcls, proto;
        cls = CD_DEV_CLASS;
        subcls = CD_DEV_SUBCLASS;
        proto = CD_DEV_PROTOCOL;
        return usbredirfilter_check(rules, count,
            dev->device_info.class,
            dev->device_info.subclass,
            dev->device_info.protocol,
            &cls, &subcls, &proto, 1, dev->device_info.vid,
            dev->device_info.pid, USB2_BCD, 0);
    }
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

static void usbredir_log(void *user_data, int level, const char *msg)
{
    SpiceUsbBackendChannel *ch = (SpiceUsbBackendChannel *)user_data;

    switch (level) {
    case usbredirparser_error:
        g_critical("%s", msg);
        ch->data.log(ch->data.user_data, msg, TRUE);
        break;
    case usbredirparser_warning:
        g_warning("%s", msg);
        ch->data.log(ch->data.user_data, msg, TRUE);
        break;
    default:
        ch->data.log(ch->data.user_data, msg, FALSE);
        break;
    }
}

static int usbredir_write_callback(void *user_data, uint8_t *data, int count)
{
    SpiceUsbBackendChannel *ch = user_data;
    int res;
    SPICE_DEBUG("%s ch %p, %d bytes", __FUNCTION__, ch, count);
    if (!ch->hello_sent) {
        ch->hello_sent = 1;
        if (count == 80) {
            memcpy(&ch->host_caps, data + 76, 4);
            SPICE_DEBUG("%s ch %p, sending first hello, caps %08X",
                __FUNCTION__, ch, ch->host_caps);
        }
    }
    res = ch->data.write_callback(ch->data.user_data, data, count);
    return res;
}

#if USBREDIR_VERSION >= 0x000701
static uint64_t usbredir_buffered_output_size_callback(void *user_data)
{
    SpiceUsbBackendChannel *ch = user_data;
    return ch->data.get_queue_size(ch->data.user_data);
}
#endif

int spice_usb_backend_provide_read_data(SpiceUsbBackendChannel *ch, uint8_t *data, int count)
{
    int res = 0;
    if (!ch->read_buf) {
        typedef int(*readproc_t)(void *);
        readproc_t fn = NULL;
        void *param;
        ch->read_buf = data;
        ch->read_buf_size = count;
        if (!ch->hello) {
            ch->hello = g_malloc(count);
            memcpy(ch->hello, data, count);
            ch->hello_size = count;
            if (ch->usbredirhost) {
                ch->hello_done_host = 1;
            }
            if (ch->parser) {
                ch->hello_done_parser = 1;
            }
        }
        if (ch->usbredirhost) {
            fn = (readproc_t)usbredirhost_read_guest_data;
            param = ch->usbredirhost;
        }
        if (ch->parser) {
            fn = (readproc_t)usbredirparser_do_read;
            param = ch->parser;
        }
        res = fn ? fn(param) : USB_REDIR_ERROR_IO;
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

    } else {
        res = USB_REDIR_ERROR_READ_PARSE;
        SPICE_DEBUG("%s ch %p, %d bytes, already has data", __FUNCTION__, ch, count);
    }
    if (ch->rejected) {
        ch->rejected = 0;
        res = USB_REDIR_ERROR_DEV_REJECTED;
    }
    return res;
}

void spice_usb_backend_return_write_data(SpiceUsbBackendChannel *ch, void *data)
{
    typedef void(*retdata)(void *, void *);
    retdata fn = NULL;
    void *param;
    if (ch->usbredirhost) {
        fn = (retdata)usbredirhost_free_write_buffer;
        param = ch->usbredirhost;
    }
    if (ch->parser) {
        fn = (retdata)usbredirhost_free_write_buffer;
        param = ch->parser;
    }
    if (fn) {
        SPICE_DEBUG("%s ch %p", __FUNCTION__, ch);
        fn(param, data);
    } else {
        SPICE_DEBUG("%s ch %p - NOBODY TO CALL", __FUNCTION__, ch);
    }
}

#if 0
struct usbredirparser {
    /* app private data passed into all callbacks as the priv argument */
    void *priv;
    /* non packet callbacks */
    usbredirparser_log log_func;
    usbredirparser_read read_func;
    usbredirparser_write write_func;
    /* usb-redir-protocol v0.3 control packet complete callbacks */
    usbredirparser_device_connect device_connect_func;
    usbredirparser_device_disconnect device_disconnect_func;
    usbredirparser_reset reset_func;
    usbredirparser_interface_info interface_info_func;
    usbredirparser_ep_info ep_info_func;
    usbredirparser_set_configuration set_configuration_func;
    usbredirparser_get_configuration get_configuration_func;
    usbredirparser_configuration_status configuration_status_func;
    usbredirparser_set_alt_setting set_alt_setting_func;
    usbredirparser_get_alt_setting get_alt_setting_func;
    usbredirparser_alt_setting_status alt_setting_status_func;
    usbredirparser_start_iso_stream start_iso_stream_func;
    usbredirparser_stop_iso_stream stop_iso_stream_func;
    usbredirparser_iso_stream_status iso_stream_status_func;
    usbredirparser_start_interrupt_receiving start_interrupt_receiving_func;
    usbredirparser_stop_interrupt_receiving stop_interrupt_receiving_func;
    usbredirparser_interrupt_receiving_status interrupt_receiving_status_func;
    usbredirparser_alloc_bulk_streams alloc_bulk_streams_func;
    usbredirparser_free_bulk_streams free_bulk_streams_func;
    usbredirparser_bulk_streams_status bulk_streams_status_func;
    usbredirparser_cancel_data_packet cancel_data_packet_func;
    /* usb-redir-protocol v0.3 data packet complete callbacks */
    usbredirparser_control_packet control_packet_func;
    usbredirparser_bulk_packet bulk_packet_func;
    usbredirparser_iso_packet iso_packet_func;
    usbredirparser_interrupt_packet interrupt_packet_func;
    /* usbredir 0.3.2 new non packet callbacks (for multi-thread locking) */
    usbredirparser_alloc_lock alloc_lock_func;
    usbredirparser_lock lock_func;
    usbredirparser_unlock unlock_func;
    usbredirparser_free_lock free_lock_func;
    /* usbredir 0.3.2 new control packet complete callbacks */
    usbredirparser_hello hello_func;
    /* usbredir 0.4 new control packet complete callbacks */
    usbredirparser_filter_reject filter_reject_func;
    usbredirparser_filter_filter filter_filter_func;
    usbredirparser_device_disconnect_ack device_disconnect_ack_func;
    /* usbredir 0.6 new control packet complete callbacks */
    usbredirparser_start_bulk_receiving start_bulk_receiving_func;
    usbredirparser_stop_bulk_receiving stop_bulk_receiving_func;
    usbredirparser_bulk_receiving_status bulk_receiving_status_func;
    /* usbredir 0.6 new data packet complete callbacks */
    usbredirparser_buffered_bulk_packet buffered_bulk_packet_func;
};
#endif

static void get_device_descriptor(SpiceUsbBackendDevice *d,
    struct usb_redir_control_packet_header *h)
{
    uint8_t *buffer = (uint8_t *)(h + 1);
    const void *p = NULL;
    uint8_t len = 0;
    static const struct libusb_device_descriptor desc =
    {
        .bLength = 18,
        .bDescriptorType = LIBUSB_DT_DEVICE,
        .bcdUSB = USB2_BCD,
        .bDeviceClass = 0,
        .bDeviceSubClass = 0,
        .bDeviceProtocol = 0,
        .bMaxPacketSize0 = 64,
        .idVendor = CD_DEV_VID,
        .idProduct = CD_DEV_PID,
        .bcdDevice = 0x100,
        .iManufacturer = 0,
        .iProduct = 0,
        .iSerialNumber = 0,
        .bNumConfigurations = 1
    };
    static const uint8_t cfg[] =
    {
        9, //len of cfg desc
        LIBUSB_DT_CONFIG, // desc type
        0x20, // wlen
        0,
        1, // num if
        1, // cfg val
        0, // cfg name
        0x80, // bus powered
        0x32, // 100 ma
        9, // len of IF desc
        LIBUSB_DT_INTERFACE,
        0, // num if
        0, // alt setting
        2, // num of endpoints
        CD_DEV_CLASS,
        CD_DEV_SUBCLASS,
        CD_DEV_PROTOCOL,
        0, // if name
        7,
        LIBUSB_DT_ENDPOINT,
        0x81, //->Direction : IN - EndpointID : 1
        0x02, //->Bulk Transfer Type
        0,    //wMaxPacketSize : 0x0200 = 0x200 max bytes
        2,
        0,    //bInterval
        7,
        LIBUSB_DT_ENDPOINT,
        0x02, //->Direction : OUT - EndpointID : 2
        0x02, //->Bulk Transfer Type
        0,    //wMaxPacketSize : 0x0200 = 0x200 max bytes
        2,
        0,    //bInterval
    };
    switch (h->value >> 8) {
        case LIBUSB_DT_DEVICE:
            p = &desc;
            len = sizeof(desc);
            break;
        case LIBUSB_DT_CONFIG:
            p = cfg;
            len = sizeof(cfg);
            break;
    }
    if (p && len) {
        len = MIN(len, h->length);
        memcpy(buffer, p, len);
        h->length = len;
    } else {
        h->length = 0;
        h->status = usb_redir_stall;
    }
}

static void usbredir_control_packet(void *priv,
    uint64_t id, struct usb_redir_control_packet_header *h,
    uint8_t *data, int data_len)
{
    SpiceUsbBackendChannel *ch = priv;
    struct {
        struct usb_redir_control_packet_header h;
        uint8_t buffer[255];
    } response = { 0 };
    uint8_t reqtype = h->requesttype & 0x7f;
    response.h = *h;
    response.h.status = 0;
    SPICE_DEBUG("%s %p: TRVIL %02X %02X %04X %04X %04X",
        __FUNCTION__,
        ch, h->requesttype, h->request,
        h->value, h->index, h->length);
    if (!ch->attached) {
        // device already detached
        response.h.status = usb_redir_ioerror;
        response.h.length = 0;
    } else if (reqtype == (LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE)) {
        switch (h->request) {
            case LIBUSB_REQUEST_GET_DESCRIPTOR:
                get_device_descriptor(ch->attached, &response.h);
                break;
            default:
                response.h.length = 0;
                break;
        }
    } else if (reqtype == (LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_ENDPOINT)) {
        // should be clear stall request
        response.h.length = 0;
        response.h.status = 0;
    } else if (reqtype == (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE)) {
        response.h.length = 0;
        response.h.status = 0;
        switch (h->request) {
            case 0xFF:
                // mass-storage class request 'reset'
                break;
            case 0xFE:
                // mass-storage class request 'get max lun'
                // returning one byte
                if (!h->length) {
                    response.h.status = usb_redir_stall;
                } else {
                    response.h.length = 1;
                    response.buffer[0] = MAX_LUN_PER_DEVICE - 1;
                }
                break;
            default:
                break;
        }
    }
    else {
     response.h.length = 0;
     response.h.status = usb_redir_stall;
 }
    SPICE_DEBUG("%s responding with payload of %02X, status %X",
        __FUNCTION__,
        response.h.length, response.h.status);
    usbredirparser_send_control_packet(ch->parser, id, &response.h,
        response.h.length ? response.buffer : NULL, response.h.length);
    usbredir_write_flush_callback(ch);
    usbredirparser_free_packet_data(ch->parser, data);
}

static void usbredir_bulk_packet(void *priv,
    uint64_t id, struct usb_redir_bulk_packet_header *h,
    uint8_t *data, int data_len)
{
    SpiceUsbBackendChannel *ch = priv;
    SpiceUsbBackendDevice *d = ch->attached;
    struct usb_redir_bulk_packet_header hout = *h;
    uint32_t h_length = (h->length_high << 16) | h->length;
    SPICE_DEBUG("%s %p: ep %X, sid %uX, rid %lX, hlen %d, data %p, len %d", __FUNCTION__, 
                ch, h->endpoint, h->stream_id, (long unsigned)id, (int)h_length, data, data_len);
    if (!d || !d->d.msc) {
        SPICE_DEBUG("%s: device not attached or not realized", __FUNCTION__);
        hout.status = usb_redir_ioerror;
        hout.length = hout.length_high = 0;
        SPICE_DEBUG("%s: responding (a) with ZLP status %d", __FUNCTION__, hout.status);
        usbredirparser_send_bulk_packet(ch->parser, id,
            &hout, NULL, 0);
    } else if (h->endpoint & LIBUSB_ENDPOINT_IN) {
        if (ch->num_reads > 0) {
            SPICE_DEBUG("%s: already %u pending reads", __FUNCTION__, ch->num_reads);
            //cd_usb_bulk_msd_read_complete(d, NULL, 0, BULK_STATUS_ERROR);
        }
        ch->read_bulk[ch->num_reads].hout = *h;
        ch->read_bulk[ch->num_reads].id = id;
        ch->num_reads++;

        int res = cd_usb_bulk_msd_read(d->d.msc, h_length);
        if (res) {
            SPICE_DEBUG("%s: error on bulk read", __FUNCTION__);
            ch->num_reads = 0;
            hout.length = hout.length_high = 0;
            hout.status = usb_redir_stall;
            SPICE_DEBUG("%s: responding (b) with ZLP status %d", __FUNCTION__, hout.status);
            usbredirparser_send_bulk_packet(ch->parser, id,
                &hout, NULL, 0);
        }
    } else {
        cd_usb_bulk_msd_write(d->d.msc, data, data_len);
        hout.status = usb_redir_success;
        SPICE_DEBUG("%s: responding status %d", __FUNCTION__, hout.status);
        usbredirparser_send_bulk_packet(ch->parser, id, &hout, NULL, 0);
    }

    usbredirparser_free_packet_data(ch->parser, data);
    usbredir_write_flush_callback(ch);
}

static void usbredir_device_reset(void *priv)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s ch %p", __FUNCTION__, ch);
    if (ch->attached) {
        cd_usb_bulk_msd_reset(ch->attached->d.msc);
    }
}

static void usbredir_interface_info(void *priv,
    struct usb_redir_interface_info_header *interface_info)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s not implemented %p", __FUNCTION__, ch);
}

static void usbredir_interface_ep_info(void *priv,
    struct usb_redir_ep_info_header *ep_info)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s not implemented %p", __FUNCTION__, ch);
}

static void usbredir_set_configuration(void *priv,
    uint64_t id, struct usb_redir_set_configuration_header *set_configuration)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_configuration_status_header h;
    h.status = 0;
    h.configuration = set_configuration->configuration;
    SPICE_DEBUG("%s ch %p, cfg %d", __FUNCTION__, ch, h.configuration);
    if (ch->attached) {
        ch->attached->configured = h.configuration != 0;
    }
    usbredirparser_send_configuration_status(ch->parser, id, &h);
    usbredir_write_flush_callback(ch);
}

static void usbredir_get_configuration(void *priv, uint64_t id)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_configuration_status_header h;
    h.status = 0;
    h.configuration = ch->attached && ch->attached->configured;
    SPICE_DEBUG("%s ch %p, cfg %d", __FUNCTION__, ch, h.configuration);
    usbredirparser_send_configuration_status(ch->parser, id, &h);
    usbredir_write_flush_callback(ch);
}

static void usbredir_set_alt_setting(void *priv,
    uint64_t id, struct usb_redir_set_alt_setting_header *s)
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

static void usbredir_get_alt_setting(void *priv,
    uint64_t id, struct usb_redir_get_alt_setting_header *s)
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
    if (ch->num_reads > 0) {
        SPICE_DEBUG("%s ch %p id %" PRIu64 "num_reads %" PRIu32,
            __FUNCTION__, ch, id, ch->num_reads);
        if (cd_usb_bulk_msd_cancel_read(ch->attached->d.msc)) {
            int nread;
            for (nread = 0; nread < ch->num_reads; nread++) {
                ch->read_bulk[nread].hout.length = 0;
                ch->read_bulk[nread].hout.length_high = 0;
                ch->read_bulk[nread].hout.status = usb_redir_cancelled;
                usbredirparser_send_bulk_packet(ch->parser, ch->read_bulk[nread].id,
                    &ch->read_bulk[nread].hout, NULL, 0);
            }
            ch->num_reads = 0;
            usbredir_write_flush_callback(ch);
        }
    }
}

static void usbredir_filter_reject(void *priv)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s %p", __FUNCTION__, ch);
    ch->rejected = 1;
}

/* Note that the ownership of the rules array is passed on to the callback. */
static void usbredir_filter_filter(void *priv,
    struct usbredirfilter_rule *r, int count)
{
    SpiceUsbBackendChannel *ch = priv;
    SPICE_DEBUG("%s ch %p %d filters", __FUNCTION__, ch, count);
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
    if (ch->parser) {
        ch->parser = NULL;
        SPICE_DEBUG("%s switch to usbredirhost", __FUNCTION__);
        ch->usbredirhost = ch->hiddenhost;
    }
}

static void usbredir_hello(void *priv,
    struct usb_redir_hello_header *hello)
{
    SpiceUsbBackendChannel *ch = priv;
    struct usb_redir_device_connect_header device_connect;
    struct usb_redir_ep_info_header ep_info = { 0 };
    struct usb_redir_interface_info_header interface_info = { 0 };
    SPICE_DEBUG("%s %p %s", __FUNCTION__, ch, hello ? "" : "(internal)");
    if (ch->attached) {
        interface_info.interface_count = 1;
        interface_info.interface_class[0] = CD_DEV_CLASS;
        interface_info.interface_subclass[0] = CD_DEV_SUBCLASS;
        interface_info.interface_protocol[0] = CD_DEV_PROTOCOL;
        usbredirparser_send_interface_info(ch->parser, &interface_info);

        ep_info.type[0x11] = 2;
        ep_info.max_packet_size[0x11] = 512;
        ep_info.type[0x02] = 2;
        ep_info.max_packet_size[0x02] = 512;
        usbredirparser_send_ep_info(ch->parser, &ep_info);

        device_connect.device_class = 0;
        device_connect.device_subclass = 0;
        device_connect.device_protocol = 0;
        device_connect.vendor_id = ch->attached->device_info.vid;
        device_connect.product_id = ch->attached->device_info.pid;
        device_connect.device_version_bcd = USB2_BCD;
        device_connect.speed = usb_redir_speed_high;
        usbredirparser_send_device_connect(ch->parser, &device_connect);
        usbredir_write_flush_callback(ch);
    }
}

static struct usbredirparser *create_parser(SpiceUsbBackendChannel *ch)
{
    struct usbredirparser *parser = usbredirparser_create();
    if (parser) {
        uint32_t caps[USB_REDIR_CAPS_SIZE] = { 0 };
        uint32_t flags =
            usbredirparser_fl_no_hello |
            usbredirparser_fl_write_cb_owns_buffer |
            usbredirparser_fl_usb_host;
        parser->priv = ch;
        parser->log_func = usbredir_log;
        parser->read_func = usbredir_read_callback;
        parser->write_func = usbredir_write_callback;
        parser->reset_func = usbredir_device_reset;
        parser->interface_info_func = usbredir_interface_info;
        parser->ep_info_func = usbredir_interface_ep_info;
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
        parser->filter_filter_func = usbredir_filter_filter;
        parser->device_disconnect_ack_func = usbredir_device_disconnect_ack;
        if (ch->host_caps & (1 << usb_redir_cap_connect_device_version)) {
            usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
        }
        usbredirparser_caps_set_cap(caps, usb_redir_cap_filter);
        if (ch->host_caps & (1 << usb_redir_cap_device_disconnect_ack)) {
            usbredirparser_caps_set_cap(caps, usb_redir_cap_device_disconnect_ack);
        }
        if (ch->host_caps & (1 << usb_redir_cap_ep_info_max_packet_size)) {
            usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
        }
        if (ch->host_caps & (1 << usb_redir_cap_64bits_ids)) {
            usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
        }
        if (ch->host_caps & (1 << usb_redir_cap_32bits_bulk_length)) {
            usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
        }
        if (ch->host_caps & (1 << usb_redir_cap_bulk_streams)) {
            usbredirparser_caps_set_cap(caps, usb_redir_cap_bulk_streams);
        }
        usbredirparser_init(parser, PACKAGE_STRING, caps, USB_REDIR_CAPS_SIZE, flags);
    }

    return parser;
}

gboolean spice_usb_backend_channel_attach(SpiceUsbBackendChannel *ch, SpiceUsbBackendDevice *dev, const char **msg)
{
    const char *dummy;
    if (!msg) {
        msg = &dummy;
    }
    SPICE_DEBUG("%s >> ch %p, dev %p (was attached %p)", __FUNCTION__, ch, dev, ch->attached);
    gboolean b = FALSE;
    if (dev && dev->isLibUsb) {
        libusb_device_handle *handle = NULL;
        int rc = libusb_open(dev->d.libusb_device, &handle);
        b = rc == 0 && handle;
        if (b) {
            if (!ch->usbredirhost) {
                ch->usbredirhost = ch->hiddenhost;
                ch->parser = NULL;
            }
            rc = usbredirhost_set_device(ch->usbredirhost, handle);
            if (rc) {
                SPICE_DEBUG("%s ch %p, dev %p usbredirhost error %d", __FUNCTION__, ch, dev, rc);
                b = FALSE;
            } else {
                ch->attached = dev;
                dev->attached_to = ch;
                if (ch->hello && !ch->hello_done_host) {
                    SPICE_DEBUG("%s sending cached hello to host", __FUNCTION__);
                    ch->hello_done_host = 1;
                    spice_usb_backend_provide_read_data(ch, ch->hello, ch->hello_size);
                }
            }
        } else {
            const char *desc = spice_usbutil_libusb_strerror(rc);
            g_warning("Error libusb_open: %s [%i]", desc, rc);
            *msg = desc;
        }
    } else if (!dev) {
        SPICE_DEBUG("%s intentional sleep", __FUNCTION__);
        g_usleep(100000);
        if (ch->usbredirhost) {
            // it will call libusb_close internally
            usbredirhost_set_device(ch->usbredirhost, NULL);
        } else {
            // CD redir detach
            usbredirparser_send_device_disconnect(ch->parser);
            usbredir_write_flush_callback(ch);
        }
        SPICE_DEBUG("%s ch %p, detach done", __FUNCTION__, ch);
        ch->attached->attached_to = NULL;
        ch->attached = NULL;
        b = TRUE;
    } else {
        // CD redir attach
        b = TRUE;
        ch->usbredirhost = NULL;
        ch->parser = ch->hiddenparser;
        ch->attached = dev;
        dev->attached_to = ch;
        if (ch->hello_done_parser) {
            // send device info
            usbredir_hello(ch, NULL);
        } else if (ch->hello) {
            SPICE_DEBUG("%s sending cached hello to parser", __FUNCTION__);
            ch->hello_done_parser = 1;
            spice_usb_backend_provide_read_data(ch, ch->hello, ch->hello_size);
            usbredir_write_flush_callback(ch);
        }
    }
    return b;
}

SpiceUsbBackendChannel *spice_usb_backend_channel_initialize(
    SpiceUsbBackend *be,
    const SpiceUsbBackendChannelInitData *init_data)
{
    SpiceUsbBackendChannel *ch = g_new0(SpiceUsbBackendChannel, 1);
    SPICE_DEBUG("%s >>", __FUNCTION__);
    gboolean ok = FALSE;
    if (ch) {
        ch->data = *init_data;
        ch->hiddenhost =
            usbredirhost_open_full(
                be->libusbContext,
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
                init_data->debug ? usbredirparser_debug : usbredirparser_warning,
                usbredirhost_fl_write_cb_owns_buffer);
        ok = ch->hiddenhost != NULL;
        if (ok) {
#if USBREDIR_VERSION >= 0x000701
            usbredirhost_set_buffered_output_size_cb(ch->hiddenhost, usbredir_buffered_output_size_callback);
#endif
        }
    }

    if (ok) {
        ch->usbredirhost = ch->hiddenhost;
    }

    if (ch && !ok) {
        g_error("Out of memory allocating usbredir or parser");
        if (ch->hiddenhost) {
            usbredirhost_close(ch->hiddenhost);
        }
        g_free(ch);
        ch = NULL;
    }
    SPICE_DEBUG("%s << %p", __FUNCTION__, ch);
    return ch;
}

void spice_usb_backend_channel_up(SpiceUsbBackendChannel *ch)
{
    SPICE_DEBUG("%s %p, host %p, parser %p", __FUNCTION__, ch, ch->usbredirhost, ch->parser);
    usbredirhost_write_guest_data(ch->usbredirhost);
    ch->hiddenparser = create_parser(ch);
}

void spice_usb_backend_channel_finalize(SpiceUsbBackendChannel *ch)
{
    SPICE_DEBUG("%s >> %p", __FUNCTION__, ch);
    if (ch->usbredirhost) {
        usbredirhost_close(ch->usbredirhost);
    }
    if (ch->parser) {
        usbredirparser_destroy(ch->parser);
    }
    if (ch->hello) {
        g_free(ch->hello);
    }

    if (ch->rules) {
        // is it ok to g_free the memory that was allocated by parser?
        g_free(ch->rules);
    }

    g_free(ch);
    SPICE_DEBUG("%s << %p", __FUNCTION__, ch);
}

void spice_usb_backend_channel_get_guest_filter(
    SpiceUsbBackendChannel *ch,
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

    if (*count) {
        SPICE_DEBUG("%s ch %p: %d filters", __FUNCTION__, ch, *count);
    }
    for (i = 0; i < *count; i++) {
        SPICE_DEBUG("%s class %d, %X:%X",
            r[i]->allow ? "allowed" : "denied", r[i]->device_class,
            (uint32_t)r[i]->vendor_id, (uint32_t)r[i]->product_id);
    }
}

#endif // USB_REDIR
