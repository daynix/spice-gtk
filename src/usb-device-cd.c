/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
    Copyright (C) 2019 Red Hat, Inc.

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

#include "config.h"

#ifdef USE_USBREDIR

#include <glib-object.h>
#include <inttypes.h>
#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <errno.h>
#include <libusb.h>
#include <fcntl.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <ntddcdrm.h>
#include <ntddmmc.h>
#else
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/cdrom.h>
#endif

#include "usb-emulation.h"
#include "usb-device-cd.h"
#include "cd-usb-bulk-msd.h"

typedef struct SpiceCdLU {
    char *filename;
    GFileInputStream *stream;
    uint64_t size;
    uint32_t blockSize;
    uint32_t loaded : 1;
    uint32_t device : 1;
} SpiceCdLU;

#define MAX_LUN_PER_DEVICE              1
#define USB2_BCD                        0x200
/* Red Hat USB VID */
#define CD_DEV_VID                      0x2b23
#define CD_DEV_PID                      0xCDCD
#define CD_DEV_CLASS                    8
#define CD_DEV_SUBCLASS                 6
#define CD_DEV_PROTOCOL                 0x50
#define CD_DEV_BLOCK_SIZE               0x200
#define DVD_DEV_BLOCK_SIZE              0x800
#define MAX_BULK_IN_REQUESTS            64

struct BufferedBulkRead {
    struct usb_redir_bulk_packet_header hout;
    uint64_t id;
};

struct SpiceUsbEmulatedDevice {
    UsbDeviceOps dev_ops;
    SpiceUsbBackend *backend;
    SpiceUsbDevice *parent;
    struct usbredirparser *parser;
    UsbCdBulkMsdDevice* msc;
    SpiceCdLU units[MAX_LUN_PER_DEVICE];
    gboolean locked;
    gboolean delete_on_eject;
    gboolean deleting;
    uint32_t num_reads;
    struct BufferedBulkRead read_bulk[MAX_BULK_IN_REQUESTS];
    /* according to USB MSC spec */
    uint16_t serial[12];
    uint8_t max_lun_index;
};

typedef struct SpiceUsbEmulatedDevice UsbCd;

#ifndef G_OS_WIN32

static int cd_device_open_stream(SpiceCdLU *unit, const char *filename)
{
    unit->device = 0;

    if (!unit->filename && !filename) {
        SPICE_DEBUG("%s: file name not provided", __FUNCTION__);
        return -1;
    }
    if (unit->filename && filename) {
        g_free(unit->filename);
        unit->filename = NULL;
    }
    if (filename) {
        unit->filename = g_strdup(filename);
    }

    int fd = open(unit->filename, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, unit->filename);
        return -1;
    }

    struct stat file_stat = { 0 };
    if (fstat(fd, &file_stat) || file_stat.st_size == 0) {
        file_stat.st_size = 0;
        unit->device = 1;
        if (!ioctl(fd, BLKGETSIZE64, &file_stat.st_size) &&
            !ioctl(fd, BLKSSZGET, &unit->blockSize)) {
        }
    }
    unit->size = file_stat.st_size;
    close(fd);
    if (unit->size) {
        GFile *file_object = g_file_new_for_path(unit->filename);
        unit->stream = g_file_read(file_object, NULL, NULL);
        g_clear_object(&file_object);
    }
    if (!unit->stream) {
        SPICE_DEBUG("%s: can't open stream on %s", __FUNCTION__, unit->filename);
        return -1;
    }

    return 0;
}

static int cd_device_load(SpiceCdLU *unit, gboolean load)
{
    int error;
    if (!unit->device || !unit->filename) {
        return -1;
    }
    int fd = open(unit->filename, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (load) {
        error = ioctl(fd, CDROMCLOSETRAY, 0);
    } else {
        error = ioctl(fd, CDROM_LOCKDOOR, 0);
        error = ioctl(fd, CDROMEJECT, 0);
    }
    if (error) {
        // note that ejecting might be available only for root
        // loading might be available also for regular user
        SPICE_DEBUG("%s: can't %sload %s, res %d, errno %d",
            __FUNCTION__, load ? "" : "un", unit->filename, error, errno);
    }
    close(fd);
    return error;
}

static int cd_device_check(SpiceCdLU *unit)
{
    int error;
    if (!unit->device || !unit->filename) {
        return -1;
    }
    int fd = open(unit->filename, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    error = ioctl(fd, CDROM_DRIVE_STATUS, 0);
    error = (error == CDS_DISC_OK) ? 0 : -1;
    if (!error) {
        error = ioctl(fd, CDROM_DISC_STATUS, 0);
        error = (error == CDS_DATA_1) ? 0 : -1;
    }
    close(fd);
    return error;
}

#else

static gboolean is_device_name(const char *filename)
{
    return g_ascii_isalpha(filename[0]) && filename[1] == ':' &&
        filename[2] == 0;
}

static HANDLE open_file(const char *filename)
{
    HANDLE h = CreateFileA(filename,
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           NULL, OPEN_EXISTING,
                           0,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = NULL;
    }
    return h;
}

static uint32_t ioctl_out(HANDLE h, uint32_t code, void *out_buffer, uint32_t out_size)
{
    uint32_t error;
    DWORD ret;
    BOOL b = DeviceIoControl(h, code, NULL, 0, out_buffer, out_size, &ret, NULL);
    error = b ? 0 : GetLastError();
    return error;
}

static uint32_t ioctl_none(HANDLE h, uint32_t code)
{
    return ioctl_out(h, code, NULL, 0);
}

static gboolean check_device(HANDLE h)
{
    GET_CONFIGURATION_IOCTL_INPUT cfgIn =
        { FeatureCdRead, SCSI_GET_CONFIGURATION_REQUEST_TYPE_ALL, {} };
    DWORD ret;
    GET_CONFIGURATION_HEADER cfgOut;
    return DeviceIoControl(h, IOCTL_CDROM_GET_CONFIGURATION,
                           &cfgIn, sizeof(cfgIn), &cfgOut, sizeof(cfgOut),
                           &ret, NULL);
}

static int cd_device_open_stream(SpiceCdLU *unit, const char *filename)
{
    HANDLE h;
    unit->device = 0;
    if (!unit->filename && !filename) {
        SPICE_DEBUG("%s: unnamed file", __FUNCTION__);
        return -1;
    }
    if (unit->filename && filename) {
        g_free(unit->filename);
        unit->filename = NULL;
    }
    if (!filename) {
        // reopening the stream on existing file name
    } else if (is_device_name(filename)) {
        unit->filename = g_strdup_printf("\\\\.\\%s", filename);
    } else {
        unit->filename = g_strdup(filename);
    }
    h = open_file(unit->filename);
    if (!h) {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, unit->filename);
        return -1;
    }

    LARGE_INTEGER size = { 0 };
    if (!GetFileSizeEx(h, &size)) {
        uint64_t buffer[256];
        uint32_t res;
        unit->device = check_device(h);
        SPICE_DEBUG("%s: CD device %srecognized on %s",
            __FUNCTION__, unit->device ? "" : "NOT ", unit->filename);
        res = ioctl_out(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        buffer, sizeof(buffer));
        if (!res) {
            DISK_GEOMETRY_EX *pg = (DISK_GEOMETRY_EX *)buffer;
            unit->blockSize = pg->Geometry.BytesPerSector;
            size = pg->DiskSize;
        } else {
            SPICE_DEBUG("%s: can't obtain size of %s (error %u)",
                __FUNCTION__, unit->filename, res);
        }
    }
    unit->size = size.QuadPart;
    CloseHandle(h);
    if (unit->size) {
        GFile *file_object = g_file_new_for_path(unit->filename);
        unit->stream = g_file_read(file_object, NULL, NULL);
        g_clear_object(&file_object);
    }
    if (!unit->stream) {
        SPICE_DEBUG("%s: can't open stream on %s", __FUNCTION__, unit->filename);
        return -1;
    }
    return 0;
}

static int cd_device_load(SpiceCdLU *unit, gboolean load)
{
    int error = 0;
    HANDLE h;
    if (!unit->device || !unit->filename) {
        return -1;
    }
    h = open_file(unit->filename);
    if (h) {
        uint32_t res = ioctl_none(h, load ? IOCTL_STORAGE_LOAD_MEDIA : IOCTL_STORAGE_EJECT_MEDIA);
        if (res) {
            SPICE_DEBUG("%s: can't %sload %s, win error %u",
                        __FUNCTION__, load ? "" : "un", unit->filename, res);
            error = -1;
        } else {
            SPICE_DEBUG("%s: device %s [%s]",
                        __FUNCTION__, load ? "loaded" : "ejected", unit->filename);
        }
        CloseHandle(h);
    }
    return error;
}

static int cd_device_check(SpiceCdLU *unit)
{
    int error = 0;
    CDROM_DISK_DATA data;
    HANDLE h;
    if (!unit->device || !unit->filename) {
        return -1;
    }
    h = open_file(unit->filename);
    if (h) {
        uint32_t res = ioctl_none(h, IOCTL_STORAGE_CHECK_VERIFY);
        if (!res) {
            res = ioctl_out(h, IOCTL_CDROM_DISK_TYPE, &data, sizeof(data));
        }
        if (res != 0 || data.DiskData != CDROM_DISK_DATA_TRACK) {
            error = -1;
        }
        CloseHandle(h);
    }
    return error;
}

#endif

static gboolean open_stream(SpiceCdLU *unit, const char *filename)
{
    gboolean b;
    b = cd_device_open_stream(unit, filename) == 0;
    return b;
}

static void close_stream(SpiceCdLU *unit)
{
    g_clear_object(&unit->stream);
}

static gboolean load_lun(UsbCd *d, int unit, gboolean load)
{
    gboolean b = TRUE;
    if (load && d->units[unit].device) {
        // there is one possible problem in case our backend is the
        // local CD device and it is ejected
        cd_device_load(&d->units[unit], TRUE);
        close_stream(&d->units[unit]);
        if (cd_device_check(&d->units[unit]) || !open_stream(&d->units[unit], NULL)) {
            return FALSE;
        }
    }

    if (load) {
        CdScsiMediaParameters media_params = { 0 };

        media_params.stream = d->units[unit].stream;
        media_params.size = d->units[unit].size;
        media_params.block_size = d->units[unit].blockSize;
        if (media_params.block_size == CD_DEV_BLOCK_SIZE &&
            media_params.size % DVD_DEV_BLOCK_SIZE == 0) {
            media_params.block_size = DVD_DEV_BLOCK_SIZE;
        }
        SPICE_DEBUG("%s: loading %s, size %" G_GUINT64_FORMAT ", block %u",
                    __FUNCTION__, d->units[unit].filename, media_params.size, media_params.block_size);

        b = !cd_usb_bulk_msd_load(d->msc, unit, &media_params);

        d->units[unit].loaded = !!b;

    } else {
        SPICE_DEBUG("%s: unloading %s", __FUNCTION__, d->units[unit].filename);
        cd_usb_bulk_msd_unload(d->msc, unit);
        d->units[unit].loaded = FALSE;
    }
    return b;
}

static void usb_cd_unrealize(UsbCd *d)
{
    uint32_t unit = 0;
    cd_usb_bulk_msd_unrealize(d->msc, unit);
    g_clear_pointer(&d->units[unit].filename, g_free);
    close_stream(&d->units[unit]);
    g_clear_pointer(&d->msc, cd_usb_bulk_msd_free);
    g_free(d);
}

static gboolean usb_cd_get_descriptor(UsbCd *d, uint8_t type, uint8_t index,
                                      void **buffer, uint16_t *size)
{
    static struct libusb_device_descriptor desc = {
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
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
        .bNumConfigurations = 1
    };
    static uint8_t cfg[] =
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
    static uint16_t s0[2] = { 0x304, 0x409 };
    static uint16_t s1[8] = { 0x310, 'R', 'e', 'd', ' ', 'H', 'a', 't' };
    static uint16_t s2[9] = { 0x312, 'S', 'p', 'i', 'c', 'e', ' ', 'C', 'D' };

    void *p = NULL;
    uint16_t len = 0;

    switch (type) {
    case LIBUSB_DT_DEVICE:
        p = &desc;
        len = sizeof(desc);
        break;
    case LIBUSB_DT_CONFIG:
        p = cfg;
        len = sizeof(cfg);
        break;
    case LIBUSB_DT_STRING:
        if (index == 0) {
            p = s0; len = sizeof(s0);
        } else if (index == 1) {
            p = s1; len = sizeof(s1);
        } else if (index == 2) {
            p = s2; len = sizeof(s2);
        } else if (index == 3) {
            p = d->serial; len = sizeof(d->serial);
        }
        break;
    }

    if (p) {
        *buffer = p;
        *size = len;
    }

    return p != NULL;
}

static void usb_cd_attach(UsbCd *device, struct usbredirparser *parser)
{
    device->parser = parser;
}

static void usb_cd_detach(UsbCd *device)
{
    device->parser = NULL;
}

static void usb_cd_reset(UsbCd *device)
{
    cd_usb_bulk_msd_reset(device->msc);
}

static void usb_cd_control_request(UsbCd *device,
                                   uint8_t *data, int data_len,
                                   struct usb_redir_control_packet_header *h,
                                   void **buffer)
{
    uint8_t reqtype = h->requesttype & 0x7f;
    if (!device->msc) {
        return;
    }
    if (reqtype == (LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_ENDPOINT)) {
        // might be clear stall request
        h->length = 0;
        h->status = usb_redir_success;;
        return;
    }

    if (reqtype == (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE)) {
        switch (h->request) {
        case 0xFF:
            // mass-storage class request 'reset'
            usb_cd_reset(device);
            h->length = 0;
            h->status = usb_redir_success;
            break;
        case 0xFE:
            // mass-storage class request 'get max lun'
            // returning one byte
            if (h->length) {
                h->length = 1;
                h->status = usb_redir_success;
                *buffer = &device->max_lun_index;
            }
            break;
        }
    }
}

static void usb_cd_bulk_out_request(UsbCd *device,
                                    uint8_t ep, uint8_t *data, int data_len,
                                    uint8_t *status)
{
    if (!cd_usb_bulk_msd_write(device->msc, data, data_len)) {
        *status = usb_redir_success;
    }
}

void cd_usb_bulk_msd_read_complete(void *user_data,
                                   uint8_t *data, uint32_t length, CdUsbBulkStatus status)
{
    UsbCd *d = (UsbCd *)user_data;

    if (d->deleting) {
        d->deleting = FALSE;
        spice_usb_backend_device_eject(d->backend, d->parent);
    }

    if (d->parser) {
        int nread;
        uint32_t offset = 0;

        for (nread = 0; nread < d->num_reads; nread++) {
            uint32_t max_len;
            max_len = (d->read_bulk[nread].hout.length_high << 16) |
                        d->read_bulk[nread].hout.length;
            if (max_len > length) {
                max_len = length;
                d->read_bulk[nread].hout.length = length;
                d->read_bulk[nread].hout.length_high = length >> 16;
            }

            switch (status) {
            case BULK_STATUS_GOOD:
                d->read_bulk[nread].hout.status = usb_redir_success;
                break;
            case BULK_STATUS_CANCELED:
                d->read_bulk[nread].hout.status = usb_redir_cancelled;
                break;
            case BULK_STATUS_ERROR:
                d->read_bulk[nread].hout.status = usb_redir_ioerror;
                break;
            case BULK_STATUS_STALL:
            default:
                d->read_bulk[nread].hout.status = usb_redir_stall;
                break;
            }

            SPICE_DEBUG("%s: responding %" G_GUINT64_FORMAT " with len %u out of %u, status %d",
                        __FUNCTION__, d->read_bulk[nread].id, max_len,
                        length, d->read_bulk[nread].hout.status);
            usbredirparser_send_bulk_packet(d->parser, d->read_bulk[nread].id,
                                            &d->read_bulk[nread].hout,
                                            max_len ? (data + offset) : NULL,
                                            max_len);
            offset += max_len;
            length -= max_len;
        }
        d->num_reads = 0;
        usbredirparser_do_write(d->parser);

        if (length) {
            SPICE_DEBUG("%s: ERROR: %u bytes were not reported!", __FUNCTION__, length);
        }

    } else {
        SPICE_DEBUG("%s: broken device<->channel relationship!", __FUNCTION__);
    }
}

/* device reset completion callback */
void cd_usb_bulk_msd_reset_complete(void *user_data, int status)
{
    // UsbCd *d = (UsbCd *)user_data;
}

static gboolean usb_cd_bulk_in_request(UsbCd *d, uint64_t id,
                                       struct usb_redir_bulk_packet_header *h)
{
    int res;
    uint32_t len = (h->length_high << 16) | h->length;
    if (d->num_reads >= MAX_BULK_IN_REQUESTS) {
        h->length = h->length_high = 0;
        SPICE_DEBUG("%s: too many pending reads", __FUNCTION__);
        h->status = usb_redir_babble;
        return FALSE;
    }

    if (d->num_reads) {
        SPICE_DEBUG("%s: already has %u pending reads", __FUNCTION__, d->num_reads);
    }

    d->read_bulk[d->num_reads].hout = *h;
    d->read_bulk[d->num_reads].id = id;
    d->num_reads++;
    res = cd_usb_bulk_msd_read(d->msc, len);
    if (!res) {
        return TRUE;
    }

    SPICE_DEBUG("%s: error on bulk read", __FUNCTION__);
    d->num_reads--;
    h->length = h->length_high = 0;
    h->status = usb_redir_ioerror;

    return FALSE;
}

static void usb_cd_cancel_request(UsbCd *d, uint64_t id)
{
    uint32_t nread;

    for (nread = 0; nread < d->num_reads; nread++) {
        if (d->read_bulk[nread].id == id) {
            if (cd_usb_bulk_msd_cancel_read(d->msc)) {
                cd_usb_bulk_msd_read_complete(d, NULL, 0, BULK_STATUS_CANCELED);
            }
            return;
        }
    }
    SPICE_DEBUG("%s: ERROR: no such id to cancel!", __FUNCTION__);
}

// called when a change happens on SCSI layer
void cd_usb_bulk_msd_lun_changed(void *user_data, uint32_t lun)
{
    UsbCd *d = (UsbCd *)user_data;
    CdScsiDeviceInfo cd_info;

    if (!cd_usb_bulk_msd_get_info(d->msc, lun, &cd_info)) {
        // load or unload command received from SCSI
        if (d->units[lun].loaded != cd_info.loaded) {
            if (!load_lun(d, lun, cd_info.loaded)) {
                SPICE_DEBUG("%s: load failed, unloading unit", __FUNCTION__);
                cd_usb_bulk_msd_unload(d->msc, lun);
            }
        }
    }

    if (d->delete_on_eject) {
        d->delete_on_eject = FALSE;
        d->deleting = TRUE;
    } else {
        spice_usb_backend_device_report_change(d->backend, d->parent);
    }
}

static gchar *usb_cd_get_product_description(UsbCd *device)
{
    gchar *base_name = g_path_get_basename(device->units[0].filename);
    gchar *res = g_strdup_printf("SPICE CD (%s)", base_name);
    g_free(base_name);
    return res;
}

static const UsbDeviceOps devops =
{
    .get_descriptor = usb_cd_get_descriptor,
    .get_product_description = usb_cd_get_product_description,
    .attach = usb_cd_attach,
    .reset = usb_cd_reset,
    .detach = usb_cd_detach,
    .control_request = usb_cd_control_request,
    .bulk_out_request = usb_cd_bulk_out_request,
    .bulk_in_request = usb_cd_bulk_in_request,
    .cancel_request = usb_cd_cancel_request,
    .unrealize = usb_cd_unrealize,
};

static UsbCd* usb_cd_create(SpiceUsbBackend *be,
                            SpiceUsbDevice *parent,
                            void *opaque_param,
                            GError **err)
{
    CdEmulationParams *param = opaque_param;
    int error = 0, i;
    uint32_t unit = 0;
    UsbCd *d = g_new0(UsbCd, 1);
    CdScsiDeviceParameters dev_params = { 0 };
    uint16_t address = spice_usb_backend_device_get_info(parent)->address;

    d->dev_ops = devops;
    d->backend = be;
    d->parent  = parent;
    d->delete_on_eject = !!param->delete_on_eject;
    d->locked = !d->delete_on_eject;
    d->serial[0] = 0x0300 + sizeof(d->serial);
    d->serial[1] = '0' + address / 10;
    d->serial[2] = '0' + address % 10;
    for (i = 3; i < G_N_ELEMENTS(d->serial); ++i) {
        d->serial[i] = '0';
    }
    d->max_lun_index = MAX_LUN_PER_DEVICE - 1;

    dev_params.vendor = "Red Hat";
    dev_params.product = "SPICE CD";
    dev_params.version = "0";

    d->msc = cd_usb_bulk_msd_alloc(d, MAX_LUN_PER_DEVICE);
    if (!d->msc) {
        g_clear_pointer(&d, g_free);
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    _("can't allocate device"));
        return NULL;
    }
    d->units[unit].blockSize = CD_DEV_BLOCK_SIZE;
    if (!cd_usb_bulk_msd_realize(d->msc, unit, &dev_params)) {
        if (open_stream(&d->units[unit], param->filename) &&
            load_lun(d, unit, TRUE)) {
            if (d->locked) {
                cd_usb_bulk_msd_lock(d->msc, unit, TRUE);
            }
        } else {
            close_stream(&d->units[unit]);
            cd_usb_bulk_msd_unrealize(d->msc, unit);
            g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                        _("can't create device with %s"),
                        param->filename);
            error = 1;
        }
    } else {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    _("can't allocate device"));
        error = 1;
    }
    if (error) {
        g_clear_pointer(&d->msc, cd_usb_bulk_msd_free);
        g_clear_pointer(&d, g_free);
        return NULL;
    }

    return d;
}

gboolean
create_emulated_cd(SpiceUsbBackend *be,
                   CdEmulationParams *param,
                   GError **err)
{
    return spice_usb_backend_create_emulated_device(be, usb_cd_create, param, err);
}

#endif /* USE_USBREDIR */
