/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
USB CD Device emulation - Data Bulk transfers - Mass Storage Device

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

#ifdef IN_QEMU
    #include "qemu/osdep.h"
    #include "qapi/error.h"
    #include "qemu-common.h"
    #include "hw/usb.h"
    #include "hw/usb/desc.h"

    #define SPICE_DEBUG(fmt, ...) \
        do { printf("dev-scsi: " fmt , ## __VA_ARGS__); } while (0)

#else
    #include "spice/types.h"
    #include "spice-common.h"
    #include "spice-util.h"
#endif

#define SPICE_ERROR(fmt, ...) \
    do { SPICE_DEBUG("usb-msd error: " fmt , ## __VA_ARGS__); } while (0)

#include "cd-usb-bulk-msd.h"
#include "cd-scsi.h"

enum usb_cd_state {
    USB_CD_STATE_INIT, /* Not ready */
    USB_CD_STATE_CBW, /* Waiting for Command Block */
    USB_CD_STATE_DATAOUT, /* Transfer data to device */
    USB_CD_STATE_DATAIN, /* Transfer data from device */
    USB_CD_STATE_ZERO_DATAIN, /* Need to send zero bulk-in before status */
    USB_CD_STATE_CSW, /* Send Command Status */
    USB_CD_STATE_DEVICE_RESET, /* reset of a single device */
    USB_CD_STATE_TARGET_RESET /* reset of entire target */
};

struct __attribute__((__packed__)) usb_cd_cbw {
    uint32_t sig;
    uint32_t tag;
    uint32_t exp_data_len; /* expected data xfer length for the request */
    uint8_t flags;
    uint8_t lun;
    uint8_t cmd_len; /* actual length of the scsi command that follows */
    uint8_t cmd[16]; /* scsi command to perform */
};

struct __attribute__((__packed__)) usb_cd_csw {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
};

typedef enum _usb_msd_status {
    USB_MSD_STATUS_GOOD = 0,
    USB_MSD_STATUS_FAILED = 1,
    USB_MSD_STATUS_PHASE_ERR = 2,
} usb_msd_status;

typedef struct _usb_cd_bulk_msd_request
{
    cd_scsi_request scsi_req;

    uint32_t lun;
    uint32_t usb_tag;
    uint32_t usb_req_len; /* length of data requested by usb */
    uint32_t scsi_in_len; /* length of data returned by scsi limited by usb request */

    uint32_t xfer_len; /* length of data transfered until now */
    uint32_t bulk_in_len; /* length of the last postponed bulk-in request */

    struct usb_cd_csw csw; /* usb status header */
} usb_cd_bulk_msd_request;

typedef struct _usb_cd_bulk_msd_device
{
    enum usb_cd_state state;
    void *scsi_target; /* scsi handle */
    void *usb_user_data; /* used in callbacks to usb */
    usb_cd_bulk_msd_request usb_req; /* now supporting a single cmd */
    uint8_t *data_buf;
    uint32_t data_buf_len;
} usb_cd_bulk_msd_device;

static inline const char *usb_cd_state_str(enum usb_cd_state state)
{
    switch(state)
    {
    case USB_CD_STATE_INIT:
        return "INIT";
    case USB_CD_STATE_CBW:
        return "CBW";
    case USB_CD_STATE_DATAOUT:
        return "DATAOUT";
    case USB_CD_STATE_DATAIN:
        return "DATAIN";
    case USB_CD_STATE_ZERO_DATAIN:
        return "ZERO_DATAIN";
    case USB_CD_STATE_CSW:
        return "CSW";
    case USB_CD_STATE_DEVICE_RESET:
        return "DEV_RESET";
    case USB_CD_STATE_TARGET_RESET:
        return "TGT_RESET";
    default:
        return "ILLEGAL";
    }
}

static void cd_usb_bulk_msd_set_state(usb_cd_bulk_msd_device *cd, enum usb_cd_state state)
{
    SPICE_DEBUG("State %s -> %s", usb_cd_state_str(cd->state), usb_cd_state_str(state));
    cd->state = state;
}

void *cd_usb_bulk_msd_alloc(void *usb_user_data, uint32_t max_luns)
{
    usb_cd_bulk_msd_device *cd;

    cd = g_malloc0(sizeof(*cd));

    cd->data_buf_len = 256 * 1024;
    cd->data_buf = g_malloc(cd->data_buf_len);

    cd->scsi_target = cd_scsi_target_alloc(cd, max_luns);
    if (cd->scsi_target == NULL) {
        g_free(cd);
        return NULL;
    }
    cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_INIT);
    cd->usb_user_data = usb_user_data;

    SPICE_DEBUG("Alloc, max_luns:%" G_GUINT32_FORMAT, max_luns);
    return cd;
}

int cd_usb_bulk_msd_realize(void *device, uint32_t lun,
                            const cd_scsi_device_parameters *params)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;
    cd_scsi_device_parameters dev_params;
    int rc;

    dev_params.size = params->size;
    dev_params.block_size = params->block_size;

    dev_params.vendor = params->vendor ? : "SPICE";
    dev_params.product = params->product ? : "USB-CD";
    dev_params.version = params->version ? : "0.1";
    dev_params.serial = params->serial ? : "123456";

    dev_params.stream = params->stream;

    rc = cd_scsi_dev_realize(cd->scsi_target, lun, &dev_params);
    if (rc != 0) {
        SPICE_ERROR("Failed to realize lun:%" G_GUINT32_FORMAT, lun);
        return rc;
    }

    if (cd->state == USB_CD_STATE_INIT) {
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CBW);
        cd_scsi_dev_request_release(cd->scsi_target, &cd->usb_req.scsi_req);
    }

    SPICE_DEBUG("Realize OK lun:%" G_GUINT32_FORMAT, lun);
    return 0;
}

int cd_usb_bulk_msd_unrealize(void *device, uint32_t lun)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;
    int rc;

    rc = cd_scsi_dev_unrealize(cd->scsi_target, lun);
    if (rc != 0) {
        SPICE_ERROR("Unrealize lun:%" G_GUINT32_FORMAT, lun);
        return 0;
    }

    SPICE_DEBUG("Unrealize lun:%" G_GUINT32_FORMAT, lun);
    return 0;
}

void cd_usb_bulk_msd_free(void *device)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;

    cd_scsi_target_free(cd->scsi_target);
    g_free(cd);

    SPICE_DEBUG("Free");
}

int cd_usb_bulk_msd_reset(void *device)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;

    cd_scsi_target_reset(cd->scsi_target);
    cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CBW);

    SPICE_DEBUG("Reset");
    return 0;
}

static int parse_usb_msd_cmd(usb_cd_bulk_msd_device *cd, uint8_t *buf, uint32_t cbw_len)
{
    struct usb_cd_cbw *cbw = (struct usb_cd_cbw *)buf;
    usb_cd_bulk_msd_request *usb_req = &cd->usb_req;
    cd_scsi_request *scsi_req = &usb_req->scsi_req;

    if (cbw_len != 31) {
        SPICE_ERROR("CMD: Bad CBW size:%" G_GUINT32_FORMAT, cbw_len);
        return -1;
    }
    if (le32toh(cbw->sig) != 0x43425355) { /* MSD command signature */
        SPICE_ERROR("CMD: Bad CBW signature:%08x", le32toh(cbw->sig));
        return -1;
    }

    usb_req->lun = cbw->lun;
    usb_req->usb_tag = le32toh(cbw->tag);
    usb_req->usb_req_len = le32toh(cbw->exp_data_len);

    usb_req->scsi_in_len = 0; /* no data from scsi yet */
    usb_req->xfer_len = 0; /* no bulks transfered yet */
    usb_req->bulk_in_len = 0; /* no bulk-in requests yet */    

    if (usb_req->usb_req_len == 0) {
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CSW); /* no data - return status */
        scsi_req->buf = NULL;
        scsi_req->buf_len = 0;
    } else if (cbw->flags & 0x80) {
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_DATAIN); /* read command */
        scsi_req->buf = cd->data_buf;
        scsi_req->buf_len = cd->data_buf_len;
    } else {
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_DATAOUT); /* write command */
        scsi_req->buf = NULL;
        scsi_req->buf_len = 0;
    }

    scsi_req->cdb_len = ((uint32_t)cbw->cmd_len) & 0x1F;
    g_assert(scsi_req->cdb_len <= sizeof(scsi_req->cdb));
    memcpy(scsi_req->cdb, cbw->cmd, scsi_req->cdb_len);

    scsi_req->tag = usb_req->usb_tag;
    scsi_req->lun = usb_req->lun;

    SPICE_DEBUG("CMD lun:%" G_GUINT32_FORMAT " tag:0x%x flags:%08x "
        "cdb_len:%" G_GUINT32_FORMAT " req_len:%" G_GUINT32_FORMAT,
        usb_req->lun, usb_req->usb_tag, cbw->flags,
        scsi_req->cdb_len, usb_req->usb_req_len);

    /* prepare status - CSW */
    usb_req->csw.sig = htole32(0x53425355);
    usb_req->csw.tag = cbw->tag;
    usb_req->csw.residue = 0;
    usb_req->csw.status = (uint8_t)USB_MSD_STATUS_GOOD;

    return 0;
}

static void usb_cd_send_status(usb_cd_bulk_msd_device *cd)
{
    usb_cd_bulk_msd_request *usb_req = &cd->usb_req;
    cd_scsi_request *scsi_req = &usb_req->scsi_req;

    SPICE_DEBUG("Command CSW tag:0x%x msd_status:%d len:%" G_GUINT64_FORMAT,
                le32toh(usb_req->csw.tag), (int)usb_req->csw.status, sizeof(usb_req->csw));

    g_assert(usb_req->csw.sig == htole32(0x53425355));

    cd_usb_bulk_msd_read_complete(cd->usb_user_data,
                                  (uint8_t *)&usb_req->csw, sizeof(usb_req->csw),
                                  BULK_STATUS_GOOD);

    cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CBW); /* Command next */
    cd_scsi_dev_request_release(cd->scsi_target, scsi_req);
}

static void usb_cd_send_data_in(usb_cd_bulk_msd_device *cd, uint32_t max_len)
{
    usb_cd_bulk_msd_request *usb_req = &cd->usb_req;
    cd_scsi_request *scsi_req = &usb_req->scsi_req;
    uint8_t *buf = ((uint8_t *)scsi_req->buf) + usb_req->xfer_len;
    uint32_t avail_len = usb_req->scsi_in_len - usb_req->xfer_len;
    uint32_t send_len = (avail_len <= max_len) ? avail_len : max_len;

    SPICE_DEBUG("Data-in cmd tag 0x%x, remains %" G_GUINT32_FORMAT
                ", requested %" G_GUINT32_FORMAT ", send %" G_GUINT32_FORMAT,
                usb_req->csw.tag, avail_len, max_len, send_len);

    g_assert(max_len <= usb_req->usb_req_len);

    cd_usb_bulk_msd_read_complete(cd->usb_user_data,
                                  buf, send_len,
                                  BULK_STATUS_GOOD);

    if (scsi_req->status == GOOD) {
        usb_req->xfer_len += send_len;
        if (usb_req->xfer_len == usb_req->scsi_in_len) {
            /* all data for this bulk has been transferred */
            if (usb_req->scsi_in_len == usb_req->usb_req_len || /* req fully satisfiled */
                send_len < max_len) { /* partial bulk - no more data */
                cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CSW);
            } else {
                /* partial cmd data fullfilled entire vulk-in request */
                cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_ZERO_DATAIN);
            }
        }
    } else { /* cmd failed - no more data */
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CSW);
    }
}

int cd_usb_bulk_msd_read(void *device, uint32_t max_len)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;
    usb_cd_bulk_msd_request *usb_req = &cd->usb_req;
    cd_scsi_request *scsi_req = &usb_req->scsi_req;

    SPICE_DEBUG("msd_read, state: %s, len %" G_GUINT32_FORMAT,
                usb_cd_state_str(cd->state), max_len);

    switch (cd->state) {
    case USB_CD_STATE_CSW: /* Command Status */
        if (max_len < 13) {
            goto fail;
        }
        if (cd_scsi_get_req_state(scsi_req) == SCSI_REQ_COMPLETE) {
            usb_cd_send_status(cd);
        } else {
            usb_req->bulk_in_len += max_len;
            SPICE_DEBUG("msd_read CSW, req incomplete, added len %" G_GUINT32_FORMAT
                        " saved len %" G_GUINT32_FORMAT, 
                        max_len, usb_req->bulk_in_len);
        }
        break;

    case USB_CD_STATE_DATAIN: /* Bulk Data-IN */
        if (cd_scsi_get_req_state(scsi_req) == SCSI_REQ_COMPLETE) {
            usb_cd_send_data_in(cd, max_len);
        } else {
            usb_req->bulk_in_len += max_len;
            SPICE_DEBUG("msd_read DATAIN, req incomplete, added len %" G_GUINT32_FORMAT
                        " saved len %" G_GUINT32_FORMAT, 
                        max_len, usb_req->bulk_in_len);
        }
        break;

    case USB_CD_STATE_ZERO_DATAIN:
        cd_usb_bulk_msd_read_complete(cd->usb_user_data, 
                                      NULL, 0,
                                      BULK_STATUS_GOOD);
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CSW); /* Status next */
        break;

    default:
        SPICE_ERROR("Unexpected read state: %s, len %" G_GUINT32_FORMAT,
                    usb_cd_state_str(cd->state), max_len);
        goto fail;
    }
    return 0;

fail:
    return -1;
}

void cd_scsi_dev_request_complete(void *target_user_data, cd_scsi_request *scsi_req)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)target_user_data;
    usb_cd_bulk_msd_request *usb_req = &cd->usb_req;

    g_assert(scsi_req == &usb_req->scsi_req);

    usb_req->scsi_in_len = (scsi_req->in_len <= usb_req->usb_req_len) ?
                            scsi_req->in_len : usb_req->usb_req_len;

    /* prepare CSW */
    if (usb_req->usb_req_len > usb_req->scsi_in_len) {
        usb_req->csw.residue = htole32(usb_req->usb_req_len - usb_req->scsi_in_len);
    }
    if (scsi_req->status != GOOD) {
        usb_req->csw.status = (uint8_t)USB_MSD_STATUS_FAILED;
    }

    if (usb_req->bulk_in_len) {        
        /* bulk-in request arrived while scsi was still running */
        if (cd->state == USB_CD_STATE_DATAIN) {
            usb_cd_send_data_in(cd, usb_req->bulk_in_len);            
        } else if (cd->state == USB_CD_STATE_CSW) {
            usb_cd_send_status(cd);
        }
        usb_req->bulk_in_len = 0;
    }
}

int cd_usb_bulk_msd_cancel_read(void *device)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;
    usb_cd_bulk_msd_request *usb_req = &cd->usb_req;
    cd_scsi_request *scsi_req = &usb_req->scsi_req;

    /* meanwhile cancel unconditionally - no async IO yet */
    cd_usb_bulk_msd_read_complete(cd->usb_user_data,
                                  NULL, 0,
                                  BULK_STATUS_CANCELED);
    cd_scsi_dev_request_release(cd->scsi_target, scsi_req);
    return 0;
}

int cd_usb_bulk_msd_write(void *device, uint8_t *buf_out, uint32_t buf_out_len)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)device;

    switch (cd->state) {
    case USB_CD_STATE_CBW: /* Command Block */
        parse_usb_msd_cmd(cd, buf_out, buf_out_len);
        if (cd->state == USB_CD_STATE_DATAIN || cd->state == USB_CD_STATE_CSW) {
            cd_scsi_dev_request_submit(cd->scsi_target, &cd->usb_req.scsi_req);
        }
        break;
    case USB_CD_STATE_DATAOUT: /* Data-Out for a Write cmd */
        cd->usb_req.scsi_req.buf = buf_out;
        cd->usb_req.scsi_req.buf_len = buf_out_len;
        cd_scsi_dev_request_submit(cd->scsi_target, &cd->usb_req.scsi_req);
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CSW); /* Status next */
        break;
    default:
        SPICE_DEBUG("Unexpected write state: %s, len %" G_GUINT32_FORMAT,
                    usb_cd_state_str(cd->state), buf_out_len);
        goto fail;
    }
    return 0;

fail:
    return -1;
}

void cd_scsi_dev_reset_complete(void *target_user_data, uint32_t lun)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)target_user_data;

    if (cd->state == USB_CD_STATE_DEVICE_RESET) {
        cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_CBW);
        cd_usb_bulk_msd_reset_complete(cd->usb_user_data, 0);
    }
}

void cd_scsi_target_reset_complete(void *target_user_data)
{
    usb_cd_bulk_msd_device *cd = (usb_cd_bulk_msd_device *)target_user_data;
    cd_usb_bulk_msd_set_state(cd, USB_CD_STATE_INIT);
}
