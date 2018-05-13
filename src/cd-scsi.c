/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   CD device emulation - SCSI engine
   by Alexander Nezhinsky (anezhins@redhat.com)

   Copyright (c) 2018 RedHat

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
#include "spice/types.h"
#include "spice-common.h"
#include "spice-util.h"
#include "cd-scsi.h"

#define SPICE_ERROR(fmt, ...) \
    do { SPICE_DEBUG("dev-scsi error: " fmt , ## __VA_ARGS__); } while (0)

#define MAX_LUNS   32

struct _cd_scsi_target; /* forward declaration */

enum cd_scsi_power_condition {
    CD_SCSI_POWER_STOPPED,
    CD_SCSI_POWER_ACTIVE,
    CD_SCSI_POWER_IDLE,
    CD_SCSI_POWER_STANDBY
};

typedef struct _cd_scsi_lu
{
    struct _cd_scsi_target *tgt;
    uint32_t lun;

    gboolean realized;
    gboolean removable;
    gboolean loaded;
    gboolean prevent_media_removal;
    gboolean cd_rom;

    enum cd_scsi_power_condition power_cond;

    uint32_t claim_version;

    uint64_t size;
    uint32_t block_size;
    uint32_t num_blocks;

    char *vendor;
    char *product;
    char *version;
    char *serial;

    GFileInputStream *stream;

    scsi_short_sense short_sense; /* currently held sense of the scsi device */
    uint8_t fixed_sense[FIXED_SENSE_LEN];
} cd_scsi_lu;

typedef enum _cd_scsi_target_state
{
    CD_SCSI_TGT_STATE_RUNNING,
    CD_SCSI_TGT_STATE_RESET,
} cd_scsi_target_state;

typedef struct _cd_scsi_target
{
    void *user_data;

    cd_scsi_target_state state;
    cd_scsi_request *cur_req;
    GCancellable *cancellable;

    uint32_t num_luns;
    uint32_t max_luns;
    cd_scsi_lu units[MAX_LUNS];
} cd_scsi_target;

/* Predefined sense codes */

/* No sense data available */
const scsi_short_sense sense_code_NO_SENSE = {
    .key = NO_SENSE , .asc = 0x00 , .ascq = 0x00
};

/* LUN not ready, Caused not reportable */
const scsi_short_sense sense_code_LUN_NOT_READY = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x00
};

/* LUN not ready, in process of becoming ready */
const scsi_short_sense sense_code_BECOMING_READY = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x01
};

/* LUN not ready, Caused not reportable */
const scsi_short_sense sense_code_INIT_CMD_REQUIRED = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x02
};

/* LUN not ready, Manual intervention required */
const scsi_short_sense sense_code_INTERVENTION_REQUIRED = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x03
};

/* LUN not ready, Medium not present */
const scsi_short_sense sense_code_NO_MEDIUM = {
    .key = NOT_READY, .asc = 0x3a, .ascq = 0x00
};

/* LUN not ready, Medium not present - Tray Closed */
const scsi_short_sense sense_code_NO_MEDIUM_TRAY_CLOSED = {
    .key = NOT_READY, .asc = 0x3a, .ascq = 0x01
};

/* LUN not ready, Medium not present - Tray Open */
const scsi_short_sense sense_code_NO_MEDIUM_TRAY_OPEN = {
    .key = NOT_READY, .asc = 0x3a, .ascq = 0x02
};

/* LUN not ready, medium removal prevented */
const scsi_short_sense sense_code_NOT_READY_REMOVAL_PREVENTED = {
    .key = NOT_READY, .asc = 0x53, .ascq = 0x02
};

/* Hardware error, internal target failure */
const scsi_short_sense sense_code_TARGET_FAILURE = {
    .key = HARDWARE_ERROR, .asc = 0x44, .ascq = 0x00
};

/* Illegal request, invalid command operation code */
const scsi_short_sense sense_code_INVALID_OPCODE = {
    .key = ILLEGAL_REQUEST, .asc = 0x20, .ascq = 0x00
};

/* Illegal request, LBA out of range */
const scsi_short_sense sense_code_LBA_OUT_OF_RANGE = {
    .key = ILLEGAL_REQUEST, .asc = 0x21, .ascq = 0x00
};

/* Illegal request, Invalid field in CDB */
const scsi_short_sense sense_code_INVALID_FIELD = {
    .key = ILLEGAL_REQUEST, .asc = 0x24, .ascq = 0x00
};

/* Illegal request, Invalid field in parameter list */
const scsi_short_sense sense_code_INVALID_PARAM = {
    .key = ILLEGAL_REQUEST, .asc = 0x26, .ascq = 0x00
};

/* Illegal request, Parameter list length error */
const scsi_short_sense sense_code_INVALID_PARAM_LEN = {
    .key = ILLEGAL_REQUEST, .asc = 0x1a, .ascq = 0x00
};

/* Illegal request, LUN not supported */
const scsi_short_sense sense_code_LUN_NOT_SUPPORTED = {
    .key = ILLEGAL_REQUEST, .asc = 0x25, .ascq = 0x00
};

/* Illegal request, Saving parameters not supported */
const scsi_short_sense sense_code_SAVING_PARAMS_NOT_SUPPORTED = {
    .key = ILLEGAL_REQUEST, .asc = 0x39, .ascq = 0x00
};

/* Illegal request, Incompatible medium installed */
const scsi_short_sense sense_code_INCOMPATIBLE_FORMAT = {
    .key = ILLEGAL_REQUEST, .asc = 0x30, .ascq = 0x00
};

/* Illegal request, medium removal prevented */
const scsi_short_sense sense_code_ILLEGAL_REQ_REMOVAL_PREVENTED = {
    .key = ILLEGAL_REQUEST, .asc = 0x53, .ascq = 0x02
};

/* Unit attention, Capacity data has changed */
const scsi_short_sense sense_code_CAPACITY_CHANGED = {
    .key = UNIT_ATTENTION, .asc = 0x2a, .ascq = 0x09
};

/* Unit attention, Power on, reset or bus device reset occurred */
const scsi_short_sense sense_code_RESET = {
    .key = UNIT_ATTENTION, .asc = 0x29, .ascq = 0x00
};

/* Unit attention, SCSI bus reset */
const scsi_short_sense sense_code_SCSI_BUS_RESET = {
    .key = UNIT_ATTENTION, .asc = 0x29, .ascq = 0x02
};

/* Unit attention, No medium */
const scsi_short_sense sense_code_UNIT_ATTENTION_NO_MEDIUM = {
    .key = UNIT_ATTENTION, .asc = 0x3a, .ascq = 0x00
};

/* Unit attention, Medium may have changed */
const scsi_short_sense sense_code_MEDIUM_CHANGED = {
    .key = UNIT_ATTENTION, .asc = 0x28, .ascq = 0x00
};

/* Unit attention, Reported LUNs data has changed */
const scsi_short_sense sense_code_REPORTED_LUNS_CHANGED = {
    .key = UNIT_ATTENTION, .asc = 0x3f, .ascq = 0x0e
};

/* Unit attention, Device internal reset */
const scsi_short_sense sense_code_DEVICE_INTERNAL_RESET = {
    .key = UNIT_ATTENTION, .asc = 0x29, .ascq = 0x04
};

static inline gboolean cd_scsi_opcode_ua_supress(uint32_t opcode)
{
    switch (opcode) {
    case INQUIRY:
    case REPORT_LUNS:
    case GET_CONFIGURATION:
    case GET_EVENT_STATUS_NOTIFICATION:
    case REQUEST_SENSE:
        return TRUE;
    default:
        return FALSE;
    }
}

static inline const char *cd_scsi_req_state_str(cd_scsi_req_state state)
{
    switch(state) {
    case SCSI_REQ_IDLE:
        return "IDLE";
    case SCSI_REQ_RUNNING:
        return "RUNNING";
    case SCSI_REQ_COMPLETE:
        return "COMPLETE";
    case SCSI_REQ_CANCELED:
        return "CANCELED";
    default:
        return "ILLEGAL";
    }
}

static uint32_t cd_scsi_build_fixed_sense(uint8_t *buf, const scsi_short_sense *short_sense)
{
    memset(buf, 0, FIXED_SENSE_LEN);

    buf[0] = FIXED_SENSE_CURRENT;
    buf[2] = short_sense->key;
    buf[7] = 10;
    buf[12] = short_sense->asc;
    buf[13] = short_sense->ascq;

    return FIXED_SENSE_LEN;
}

static inline void cd_scsi_req_init(cd_scsi_request *req)
{
    req->req_state = SCSI_REQ_IDLE;
    req->xfer_dir = SCSI_XFER_NONE;
    req->priv_data = NULL;
    req->in_len = 0;
    req->status = GOOD;
}

static inline void cd_scsi_dev_sense_reset(cd_scsi_lu *dev)
{
    memset(&dev->short_sense, 0, sizeof(dev->short_sense));
}

static inline void cd_scsi_dev_sense_power_on(cd_scsi_lu *dev)
{
    dev->short_sense = sense_code_RESET;
}

static void cd_scsi_pending_sense(cd_scsi_lu *dev, cd_scsi_request *req)
{
    req->req_state = SCSI_REQ_COMPLETE;
    req->status = CHECK_CONDITION;
    req->in_len = 0;
}

static void cd_scsi_sense_check_cond(cd_scsi_lu *dev, cd_scsi_request *req,
                                     const scsi_short_sense *short_sense)
{
    req->req_state = SCSI_REQ_COMPLETE;
    req->status = CHECK_CONDITION;
    req->in_len = 0;

    dev->short_sense = *short_sense;
    cd_scsi_build_fixed_sense(dev->fixed_sense, short_sense);
}

static void cd_scsi_cmd_complete_good(cd_scsi_lu *dev, cd_scsi_request *req)
{
    req->req_state = SCSI_REQ_COMPLETE;
    req->status = GOOD;
}

/* SCSI Target */

void *cd_scsi_target_alloc(void *target_user_data, uint32_t max_luns)
{
    cd_scsi_target *st;

    if (max_luns == 0 || max_luns > MAX_LUNS) {
        SPICE_ERROR("Alloc, illegal max_luns:%" G_GUINT32_FORMAT, max_luns);
        return NULL;
    }

    st = g_malloc0(sizeof(*st));

    st->user_data = target_user_data;
    st->state = CD_SCSI_TGT_STATE_RUNNING;
    st->cur_req = NULL;
    st->cancellable = g_cancellable_new();
    st->max_luns = max_luns;

    return (void *)st;
}

void cd_scsi_target_free(void *scsi_target)
{
    cd_scsi_target_reset(scsi_target);
    g_free(scsi_target);
}

/* SCSI Device */

static inline gboolean cd_scsi_target_lun_legal(cd_scsi_target *st, uint32_t lun)
{
    return (lun < st->max_luns) ? TRUE : FALSE;
}

static inline gboolean cd_scsi_target_lun_realized(cd_scsi_target *st, uint32_t lun)
{
    return (st->num_luns == 0 || !st->units[lun].realized) ? FALSE : TRUE;
}

int cd_scsi_dev_realize(void *scsi_target, uint32_t lun,
                        const cd_scsi_device_parameters *dev_params)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;
    cd_scsi_lu *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Realize, illegal lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    if (cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Realize, already realized lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    dev = &st->units[lun];

    memset(dev, 0, sizeof(*dev));
    dev->tgt = st;
    dev->lun = lun;

    dev->realized = TRUE;
    dev->removable = TRUE;
    dev->loaded = FALSE;
    dev->prevent_media_removal = FALSE;
    dev->cd_rom = FALSE;

    dev->power_cond = CD_SCSI_POWER_ACTIVE;

    dev->claim_version = 0; /* 0 : none; 2,3,5 : SPC/MMC-x */

    dev->vendor = g_strdup(dev_params->vendor);
    dev->product = g_strdup(dev_params->product);
    dev->version = g_strdup(dev_params->version);
    dev->serial = g_strdup(dev_params->serial);

    cd_scsi_dev_sense_power_on(dev);

    st->num_luns ++;

    SPICE_DEBUG("Realize lun:%" G_GUINT32_FORMAT " bs:%" G_GUINT32_FORMAT 
                " VR:[%s] PT:[%s] ver:[%s] SN[%s]",
                lun, dev->block_size, dev->vendor,
                dev->product, dev->version, dev->serial);
    return 0;
}

int cd_scsi_dev_load(void *scsi_target, uint32_t lun,
                     const cd_scsi_media_parameters *media_params)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;
    cd_scsi_lu *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Load, illegal lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Load, unrealized lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    dev = &st->units[lun];
    if (dev->loaded) {
        // ToDo: implement re-loading with media change notification
        SPICE_ERROR("Load, lun:%" G_GUINT32_FORMAT " already loaded", lun);
        return -1;
    }

    dev->stream = media_params->stream;
    dev->size = media_params->size;
    dev->block_size = media_params->block_size;
    dev->num_blocks = media_params->size / media_params->block_size;

    dev->loaded = TRUE;

    SPICE_DEBUG("Load lun:%" G_GUINT32_FORMAT " size:%" G_GUINT64_FORMAT
                " blk_sz:%" G_GUINT32_FORMAT " num_blocks:%" G_GUINT32_FORMAT,
                lun, dev->size, dev->block_size, dev->num_blocks);
    return 0;
}

int cd_scsi_dev_unload(void *scsi_target, uint32_t lun)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;
    cd_scsi_lu *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Unoad, illegal lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Unload, unrealized lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    dev = &st->units[lun];
    if (!dev->loaded) {
        SPICE_ERROR("Unoad, lun:%" G_GUINT32_FORMAT " not loaded yet", lun);
        return -1;
    }
    if (dev->prevent_media_removal) {
        SPICE_ERROR("Unoad, lun:%" G_GUINT32_FORMAT " prevent_media_removal set", lun);
        return -1;
    }

    dev->loaded = FALSE;

    dev->stream = NULL;
    dev->size = 0;
    dev->block_size = 0;
    dev->num_blocks = 0;

    SPICE_DEBUG("Unload lun:%" G_GUINT32_FORMAT, lun);
    return 0;
}

int cd_scsi_dev_unrealize(void *scsi_target, uint32_t lun)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;
    cd_scsi_lu *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Unrealize, illegal lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Unrealize, absent lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    dev = &st->units[lun];

    if (dev->vendor != NULL) {
        free(dev->vendor);
        dev->vendor = NULL;
    }
    if (dev->product != NULL) {
        free(dev->product);
        dev->product = NULL;
    }
    if (dev->version != NULL) {
        free(dev->version);
        dev->version = NULL;
    }
    if (dev->serial != NULL) {
        free(dev->serial);
        dev->serial = NULL;
    }

    dev->loaded = FALSE;
    dev->realized = FALSE;
    dev->power_cond = CD_SCSI_POWER_STOPPED;

    st->num_luns --;

    SPICE_DEBUG("Unrealize lun:%" G_GUINT32_FORMAT, lun);
    return 0;
}

int cd_scsi_dev_reset(void *scsi_target, uint32_t lun)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;
    cd_scsi_lu *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Device reset, illegal lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Device reset, absent lun:%" G_GUINT32_FORMAT, lun);
        return -1;
    }
    dev = &st->units[lun];

    dev->prevent_media_removal = FALSE;
    cd_scsi_dev_sense_power_on(dev);

    SPICE_DEBUG("Device reset lun:%" G_GUINT32_FORMAT, lun);
    return 0;
}

static void cd_scsi_target_do_reset(cd_scsi_target *st)
{
    uint32_t lun;

    for (lun = 0; lun < st->max_luns; lun++) {
        if (st->units[lun].realized) {
            cd_scsi_dev_reset(st, lun);
        }
    }

    SPICE_DEBUG("Target reset complete");
    st->state = CD_SCSI_TGT_STATE_RUNNING;
    cd_scsi_target_reset_complete(st->user_data);
}

int cd_scsi_target_reset(void *scsi_target)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;

    if (st->state == CD_SCSI_TGT_STATE_RESET) {
        SPICE_DEBUG("Target already in reset");
        return -1;
    }

    st->state = CD_SCSI_TGT_STATE_RESET;

    if (st->cur_req != NULL) {
        cd_scsi_dev_request_cancel(scsi_target, st->cur_req);
        if (st->cur_req != NULL) {
            SPICE_DEBUG("Target reset in progress...");
            return 0;
        }
    }

    cd_scsi_target_do_reset(st);
    return 0;
}

cd_scsi_req_state cd_scsi_get_req_state(cd_scsi_request *req)
{
    return req->req_state;
}

static void strpadcpy(char *buf, int buf_size, const char *str, char pad)
{
    int len = strnlen(str, buf_size);
    memcpy(buf, str, len);
    memset(buf + len, pad, buf_size - len);
}

/* SCSI CDB */

static int scsi_cdb_length(uint8_t *cdb)
{
    int cdb_len;

    switch (cdb[0] >> 5) {
    case 0:
        cdb_len = 6;
        break;
    case 1:
    case 2:
        cdb_len = 10;
        break;
    case 4:
        cdb_len = 16;
        break;
    case 5:
        cdb_len = 12;
        break;
    default:
        cdb_len = -1;
    }
    return cdb_len;
}

static uint64_t scsi_cdb_lba(uint8_t *cdb, int cdb_len)
{
    uint64_t lba;

    switch (cdb_len) {
    case 6:
        lba = (((uint64_t)(cdb[1] & 0x1f)) << 16) |
              (((uint64_t)cdb[2]) << 8) |
               ((uint64_t)cdb[3]);
        break;
    case 10:
    case 12:
        lba = (((uint64_t)cdb[2]) << 24) |
              (((uint64_t)cdb[3]) << 16) |
              (((uint64_t)cdb[4]) << 8)  |
               ((uint64_t)cdb[5]);
        break;
    case 16:
        lba = (((uint64_t)cdb[2]) << 56) |
              (((uint64_t)cdb[3]) << 48) |
              (((uint64_t)cdb[4]) << 40) |
              (((uint64_t)cdb[5]) << 32) |
              (((uint64_t)cdb[6]) << 24) |
              (((uint64_t)cdb[7]) << 16) |
              (((uint64_t)cdb[8]) << 8)  |
               ((uint64_t)cdb[9]);
        break;
    default:
        lba = 0;
    }
    return lba;
}

static uint32_t scsi_cdb_xfer_length(uint8_t *cdb, int cdb_len)
{
    uint32_t len;

    switch (cdb_len) {
    case 6:
        len = (uint32_t)cdb[4];
        if (len == 0)
            len = 256;
        break;
    case 10:
        len = (((uint32_t)cdb[7]) << 8) |
               ((uint32_t)cdb[8]);
        break;
    case 12:
        len = (((uint32_t)cdb[6]) << 24) |
              (((uint32_t)cdb[7]) << 16) |
              (((uint32_t)cdb[8]) << 8)  |
               ((uint32_t)cdb[9]);
        break;
    case 16:
        len = (((uint32_t)cdb[10]) << 24) |
              (((uint32_t)cdb[11]) << 16) |
              (((uint32_t)cdb[12]) << 8)  |
               ((uint32_t)cdb[13]);
        break;
    default:
        len = 0;
        break;
    }
    return len;
}

/* SCSI commands */

static void cd_scsi_cmd_test_unit_ready(cd_scsi_lu *dev, cd_scsi_request *req)
{
    req->xfer_dir = SCSI_XFER_NONE;
    req->in_len = 0;

    if (dev->power_cond != CD_SCSI_POWER_STOPPED) {
        if (dev->loaded) {
            cd_scsi_cmd_complete_good(dev, req);
        } else {
            cd_scsi_sense_check_cond(dev, req, &sense_code_NO_MEDIUM);
        }
    } else {
        cd_scsi_sense_check_cond(dev, req, &sense_code_INIT_CMD_REQUIRED);
    }
}

static void cd_scsi_cmd_request_sense(cd_scsi_lu *dev, cd_scsi_request *req)
{
    req->xfer_dir = SCSI_XFER_FROM_DEV;

    req->req_len = req->cdb[4];
    req->in_len = (req->req_len < sizeof(dev->fixed_sense)) ?
                   req->req_len : sizeof(dev->fixed_sense);

    if (dev->short_sense.key == NO_SENSE) {
        cd_scsi_build_fixed_sense(dev->fixed_sense, &dev->short_sense);
    }
    memcpy(req->buf, dev->fixed_sense, sizeof(dev->fixed_sense));
    cd_scsi_dev_sense_reset(dev); /* clear reported sense */

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_report_luns(cd_scsi_target *st, cd_scsi_lu *dev,
                                    cd_scsi_request *req)
{
    uint8_t *out_buf = req->buf;
    uint32_t num_luns = st->num_luns;
    uint32_t buflen = 8; /* header length */
    uint32_t lun;

    req->req_len = scsi_cdb_xfer_length(req->cdb, 12);
    req->xfer_dir = SCSI_XFER_FROM_DEV;

    if (req->cdb[0] == 0x01) {
        /* only well known logical units */
        num_luns = 0;
    }

    out_buf[0] = (uint8_t)(num_luns >> 24);
    out_buf[1] = (uint8_t)(num_luns >> 16);
    out_buf[2] = (uint8_t)(num_luns >> 8);
    out_buf[3] = (uint8_t)(num_luns);
    memset(&out_buf[4], 0, 4);

    if (num_luns > 0) {
        for (lun = 0; lun < num_luns; lun++) {
            if (st->units[lun].realized) {
                out_buf[buflen++] = (uint8_t)(num_luns >> 24);
                out_buf[buflen++] = (uint8_t)(num_luns >> 16);
                out_buf[buflen++] = (uint8_t)(num_luns >> 8);
                out_buf[buflen++] = (uint8_t)(num_luns);
                memset(&out_buf[buflen], 0, 4);
                buflen += 4;
            }
        }
    }

    req->in_len = buflen;
    cd_scsi_cmd_complete_good(dev, req);
}

#define SCSI_MAX_INQUIRY_LEN        256
#define SCSI_MAX_MODE_LEN           256

static void cd_scsi_cmd_inquiry_vpd_no_lun(cd_scsi_lu *dev, cd_scsi_request *req,
                                           uint32_t perif_qual)
{
    uint8_t *outbuf = req->buf;
    uint8_t page_code = req->cdb[2];
    uint32_t resp_len = 4;

    outbuf[0] = (perif_qual << 5) | TYPE_ROM;
    outbuf[1] = page_code ; /* this page */
    outbuf[2] = 0;
    outbuf[3] = 0; /* no more data */

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    SPICE_DEBUG("inquiry_vpd, unsupported lun:%" G_GUINT32_FORMAT
                " perif_qual:0x%x len: %" G_GUINT64_FORMAT,
                req->lun, perif_qual, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_inquiry_vpd(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint8_t page_code = req->cdb[2];
    int buflen = 0;
    int start;

    outbuf[buflen++] = TYPE_ROM;
    outbuf[buflen++] = page_code ; // this page
    outbuf[buflen++] = 0x00;
    outbuf[buflen++] = 0x00;
    start = buflen;

    switch (page_code) {
    case 0x00: /* Supported page codes, mandatory */
    {
        SPICE_DEBUG("Inquiry EVPD[Supported pages] "
                    "buffer size %" G_GUINT64_FORMAT, req->req_len);
        outbuf[buflen++] = 0x00; // list of supported pages (this page)
        if (dev->serial) {
            outbuf[buflen++] = 0x80; // unit serial number
        }
        outbuf[buflen++] = 0x83; // device identification

        break;
    }
    case 0x80: /* Device serial number, optional */
    {
        int serial_len;

        serial_len = strlen(dev->serial);
        if (serial_len > 36) {
            serial_len = 36;
        }

        SPICE_DEBUG("Inquiry EVPD[Serial num] xfer size %" G_GUINT64_FORMAT,
                    req->req_len);
        memcpy(outbuf+buflen, dev->serial, serial_len);
        buflen += serial_len;
        break;
    }
    case 0x83: /* Device identification page, mandatory */
    {
        int serial_len = strlen(dev->serial);
        int max_len = 20;

        if (serial_len > max_len) {
            serial_len = max_len;
        }
        SPICE_DEBUG("Inquiry EVPD[Device id] xfer size %" G_GUINT64_FORMAT,
                    req->req_len);

        outbuf[buflen++] = 0x2; // ASCII
        outbuf[buflen++] = 0;   // not officially assigned
        outbuf[buflen++] = 0;   // reserved
        outbuf[buflen++] = serial_len; // length of data following

        memcpy(outbuf+buflen, dev->serial, serial_len);
        buflen += serial_len;
        break;
    }

    default:
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        SPICE_DEBUG("inquiry_standard, lun:%" G_GUINT32_FORMAT " invalid page_code: %02x", 
                    req->lun, (int)page_code);
        return;
    }

    /* done with EVPD */
    g_assert(buflen - start <= 255);
    outbuf[start - 1] = buflen - start;

    req->in_len = buflen;
    cd_scsi_cmd_complete_good(dev, req);
}

#define INQUIRY_STANDARD_LEN                96
#define INQUIRY_STANDARD_LEN_NO_VER         57

#define PERIF_QUALIFIER_CONNECTED           0x00
#define PERIF_QUALIFIER_NOT_CONNECTED       0x01
#define PERIF_QUALIFIER_UNSUPPORTED         0x03

#define INQUIRY_REMOVABLE_MEDIUM            0x80

#define INQUIRY_VERSION_NONE                0x00
#define INQUIRY_VERSION_SPC3                0x05

#define INQUIRY_RESP_DATA_FORMAT_SPC3       0x02

#define INQUIRY_VERSION_DESC_SAM2           0x040
#define INQUIRY_VERSION_DESC_SPC3           0x300
#define INQUIRY_VERSION_DESC_MMC3           0x2A0
#define INQUIRY_VERSION_DESC_SBC2           0x320

static void cd_scsi_cmd_inquiry_standard_no_lun(cd_scsi_lu *dev, cd_scsi_request *req,
                                                uint32_t perif_qual)
{
    uint8_t *outbuf = req->buf;
    uint32_t resp_len = 5;

    outbuf[0] = (perif_qual << 5) | TYPE_ROM;
    outbuf[1] = 0;
    outbuf[2] = INQUIRY_VERSION_NONE;
    outbuf[3] = INQUIRY_RESP_DATA_FORMAT_SPC3;
    outbuf[4] = 0;

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    SPICE_DEBUG("inquiry_standard, unsupported lun:%" G_GUINT32_FORMAT
                " perif_qual:0x%x len: %" G_GUINT64_FORMAT,
                req->lun, perif_qual, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_inquiry_standard(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t resp_len = (dev->claim_version == 0) ? INQUIRY_STANDARD_LEN_NO_VER : INQUIRY_STANDARD_LEN;

    outbuf[0] = (PERIF_QUALIFIER_CONNECTED << 5) | TYPE_ROM;
    outbuf[1] = (dev->removable) ? INQUIRY_REMOVABLE_MEDIUM : 0;
    outbuf[2] = (dev->claim_version == 0) ? INQUIRY_VERSION_NONE : INQUIRY_VERSION_SPC3;
    outbuf[3] = INQUIRY_RESP_DATA_FORMAT_SPC3; /* no HiSup, no NACA */

    outbuf[4] = resp_len - 4;

    /* (outbuf[6,7] = 0) means also {BQue=0,CmdQue=0} - no queueing at all */

    strpadcpy((char *) &outbuf[8], 8, dev->vendor, ' ');
    strpadcpy((char *) &outbuf[16], 16, dev->product, ' ');
    memcpy(&outbuf[32], dev->version, MIN(4, strlen(dev->version)));

    if (dev->claim_version > 0) {
        outbuf[58] = (INQUIRY_VERSION_DESC_SAM2 >> 8) & 0xff;
        outbuf[59] = INQUIRY_VERSION_DESC_SAM2 & 0xff;

        outbuf[60] = (INQUIRY_VERSION_DESC_SPC3 >> 8) & 0xff;
        outbuf[61] = INQUIRY_VERSION_DESC_SPC3 & 0xff;

        outbuf[62] = (INQUIRY_VERSION_DESC_MMC3 >> 8) & 0xff;
        outbuf[63] = INQUIRY_VERSION_DESC_MMC3 & 0xff;

        outbuf[64] = (INQUIRY_VERSION_DESC_SBC2 >> 8) & 0xff;
        outbuf[65] = INQUIRY_VERSION_DESC_SBC2 & 0xff;
    }

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;
    
    SPICE_DEBUG("inquiry_standard, lun:%" G_GUINT32_FORMAT " len: %" G_GUINT64_FORMAT,
                req->lun, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_INQUIRY_FLAG_EVPD                0x01
#define CD_INQUIRY_FLAG_CMD_DT              0x02

static void cd_scsi_cmd_inquiry(cd_scsi_lu *dev, cd_scsi_request *req)
{
    gboolean evpd, cmd_data;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    evpd = (req->cdb[1] & CD_INQUIRY_FLAG_EVPD) ? TRUE : FALSE;
    cmd_data = (req->cdb[1] & CD_INQUIRY_FLAG_CMD_DT) ? TRUE : FALSE;

    if (cmd_data) {
        SPICE_DEBUG("inquiry, lun:%" G_GUINT32_FORMAT " CmdDT bit set - unsupported, "
                    "cdb[1]:0x%02x cdb[1]:0x%02x",
                    req->lun, (int)req->cdb[1], (int)req->cdb[2]);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }

    req->req_len = req->cdb[4] | (req->cdb[3] << 8);
    memset(req->buf, 0, req->req_len);

    if (evpd) { /* enable vital product data */
        cd_scsi_cmd_inquiry_vpd(dev, req);
    } else { /* standard inquiry data */
        if (req->cdb[2] != 0) {
            SPICE_DEBUG("inquiry_standard, lun:%" G_GUINT32_FORMAT " non-zero page code: %02x",
                        req->lun, (int)req->cdb[2]);
            cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
            return;
        }
        cd_scsi_cmd_inquiry_standard(dev, req);
    }
}

static void cd_scsi_cmd_read_capacity(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint32_t last_blk = dev->num_blocks - 1;
    uint32_t blk_size = dev->block_size;
    uint32_t *last_blk_out = (uint32_t *)req->buf;
    uint32_t *blk_size_out = (uint32_t *)(req->buf + 4);

    req->xfer_dir = SCSI_XFER_FROM_DEV;
    req->req_len = 8;

    *last_blk_out = htobe32(last_blk);
    *blk_size_out = htobe32(blk_size);

    SPICE_DEBUG("Read capacity, lun:%" G_GUINT32_FORMAT
                " last_blk: %" G_GUINT32_FORMAT " blk_sz: %" G_GUINT32_FORMAT,
                req->lun, last_blk, blk_size);

    req->in_len = 8;
    cd_scsi_cmd_complete_good(dev, req);
}

#define RDI_TYPE_STANDARD           0 /* Standard Disc Information */
#define RDI_TYPE_TRACK_RESOURCES    1 /* Track Resources Information */
#define RDI_TYPE_POW_RESOURCES      2 /* POW Resources Information */

#define RDI_STANDARD_LEN            34

#define RDI_ERAZABLE                (0x01 << 4)
#define RDI_NON_ERAZABLE            (0x00 << 4)

#define RDI_SESSION_EMPTY           (0x00 << 2)
#define RDI_SESSION_INCOMPLETE      (0x01 << 2)
#define RDI_SESSION_DAMAGED         (0x02 << 2)
#define RDI_SESSION_COMPLETE        (0x03 << 2)

#define RDI_DISC_EMPTY              0x00
#define RDI_DISC_INCOMPLETE         0x01
#define RDI_DISC_COMPLETE           0x02
#define RDI_DISC_RANDOM_WR          0x03

#define RDI_DISC_PMA_TYPE_CD_ROM        0x00
#define RDI_DISC_PMA_TYPE_CDI           0x10
#define RDI_DISC_PMA_TYPE_DDCD          0x20
#define RDI_DISC_PMA_TYPE_UNDEFINED     0xFF

static void cd_scsi_cmd_get_read_disc_information(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t data_type;
    uint32_t first_track = 1;
    uint32_t last_track = 1;
    uint32_t num_sessions = 1;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    data_type = req->cdb[1] & 0x7;
    if (data_type != RDI_TYPE_STANDARD) {
        SPICE_DEBUG("read_disc_information, lun:%" G_GUINT32_FORMAT " unsupported data type: %02x", 
                    req->lun, data_type);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];
    req->in_len = (req->req_len < RDI_STANDARD_LEN) ? req->req_len : RDI_STANDARD_LEN;

    memset(outbuf, 0, RDI_STANDARD_LEN);
    outbuf[1] = RDI_STANDARD_LEN - 2; /* length excluding the counter itself */
    outbuf[2] = RDI_NON_ERAZABLE | RDI_SESSION_COMPLETE | RDI_DISC_COMPLETE;
    outbuf[3] = first_track; /* on disk */
    outbuf[4] = num_sessions & 0xff; /* lsb */
    outbuf[5] = first_track & 0xff; /* in last sesson, lsb */
    outbuf[6] = last_track & 0xff; /* in last sesson, lsb */
    outbuf[8] = RDI_DISC_PMA_TYPE_CD_ROM;
    outbuf[9] = (num_sessions >> 8) & 0xff; /* msb */
    outbuf[10] = (first_track >> 8) & 0xff; /* in last sesson, lsb */
    outbuf[11] = (last_track >> 8) & 0xff; /* in last sesson, lsb */

    SPICE_DEBUG("read_disc_information, lun:%" G_GUINT32_FORMAT " len: %" G_GUINT64_FORMAT,
                req->lun, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

#define RTI_ADDR_TYPE_LBA           0x00
#define RTI_ADDR_TYPE_TRACK_NUM     0x01
#define RTI_ADDR_TYPE_SESSION_NUM   0x02

#define RTI_TRACK_NUM_LEAD_IN       0x00
#define RTI_TRACK_NUM_INVISIBLE     0xff

#define TIB_LEN                     0x36

#define TIB_TRACK_MODE_CD           0x04
#define TIB_DATA_MODE_ISO_10149     0x01

#define TIB_LRA_VALID               (0x01 << 1)

static void cd_scsi_cmd_get_read_track_information(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t track_size = dev->num_blocks;
    uint32_t last_addr = track_size - 1;
    uint32_t track_num = 1;
    uint32_t session_num = 1;
    uint32_t addr_type;
    uint32_t addr_num;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    addr_type = req->cdb[1] & 0x3;
    addr_num = (req->cdb[2] << 24) | (req->cdb[3] << 16) |
               (req->cdb[4] << 8) | req->cdb[5];

    switch (addr_type) {
    case RTI_ADDR_TYPE_LBA:
        if (addr_num > last_addr) {
            SPICE_DEBUG("read_track_information, lun:%" G_GUINT32_FORMAT
                        " addr_type LBA: %" G_GUINT32_FORMAT " invalid LBA: %" G_GUINT32_FORMAT,
                        req->lun, addr_type, addr_num);
            cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
            return;
        }
        break;
    case RTI_ADDR_TYPE_TRACK_NUM:
        if (addr_num != track_num) {
            SPICE_DEBUG("read_track_information, lun:%" G_GUINT32_FORMAT
                        " addr_type track: %" G_GUINT32_FORMAT " invalid track: %" G_GUINT32_FORMAT,
                        req->lun, addr_type, addr_num);
            cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
            return;
        }
        break;
    case RTI_ADDR_TYPE_SESSION_NUM:
        if (addr_num != session_num) {
            SPICE_DEBUG("read_track_information, lun:%" G_GUINT32_FORMAT
                        " addr_type session: %" G_GUINT32_FORMAT " invalid session: %" G_GUINT32_FORMAT,
                        req->lun, addr_type, addr_num);
            cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
            return;
        }
        break;
    default:
        SPICE_DEBUG("read_track_information, lun:%" G_GUINT32_FORMAT
                    "invalid addr_type: %" G_GUINT32_FORMAT " addr_num: %" G_GUINT32_FORMAT,
                    req->lun, addr_type, addr_num);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];
    req->in_len = (req->req_len < TIB_LEN) ? req->req_len : TIB_LEN;

    memset(outbuf, 0, TIB_LEN);
    outbuf[1] = TIB_LEN - 2;
    outbuf[2] = session_num;
    outbuf[3] = track_num;
    outbuf[5] = TIB_TRACK_MODE_CD & 0x0f;
    outbuf[6] = TIB_DATA_MODE_ISO_10149 & 0x0f;
    outbuf[7] = TIB_LRA_VALID;

    /* Track size */
    outbuf[24] = (track_size >> 24) & 0xff;
    outbuf[25] = (track_size >> 16) & 0xff;
    outbuf[26] = (track_size >> 8) & 0xff;
    outbuf[27] = (track_size) & 0xff;

    /* Last recorded address */
    outbuf[28] = (last_addr >> 24) & 0xff;
    outbuf[29] = (last_addr >> 16) & 0xff;
    outbuf[30] = (last_addr >> 8) & 0xff;
    outbuf[31] = (last_addr) & 0xff;

    SPICE_DEBUG("read_track_information, lun:%" G_GUINT32_FORMAT 
                "addr_type: %" G_GUINT32_FORMAT " addr_num: %" G_GUINT32_FORMAT,
                req->lun, addr_type, addr_num);

    cd_scsi_cmd_complete_good(dev, req);
}

#define READ_TOC_TRACK_DESC_LEN     8
#define READ_TOC_RESP_LEN           (4 + 2*READ_TOC_TRACK_DESC_LEN)

static void cd_scsi_cmd_read_toc(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t msf, format, track_num;
    uint32_t last_blk = dev->num_blocks - 1;
    
    req->xfer_dir = SCSI_XFER_FROM_DEV;

    msf = (req->cdb[1] >> 1) & 0x1;
    format = req->cdb[2] & 0xf;
    track_num = req->cdb[6];

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];
    req->in_len = (req->req_len < READ_TOC_RESP_LEN) ? req->req_len : READ_TOC_RESP_LEN;

    memset(outbuf, 0, READ_TOC_RESP_LEN);
    outbuf[1] = READ_TOC_RESP_LEN - 2; /* length excluding the counter itself */
    outbuf[2] = 1; /* first track/session */
    outbuf[3] = 1; /* last track/session */

    outbuf[5] = 0x04; /* Data CD, no Q-subchannel */
    outbuf[6] = 0x01; /* Track number */
    outbuf[10] = msf ? 0x02 : 0x0;

    outbuf[13] = 0x04; /* Data CD, no Q-subchannel */
    outbuf[14] = 0xaa; /* Track number */
    if (msf) {
        last_blk = 0xff300000;
    }
    outbuf[16] = last_blk >> 24;
    outbuf[17] = last_blk >> 16;
    outbuf[18] = last_blk >> 8;
    outbuf[19] = last_blk;

    SPICE_DEBUG("read_toc, lun:%" G_GUINT32_FORMAT " len: %" G_GUINT64_FORMAT 
                " msf: %x format: 0x%02x track/session: 0x%02x",
                req->lun, req->in_len, msf, format, track_num);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_MODE_PARAM_6_LEN_HEADER              4
#define CD_MODE_PARAM_10_LEN_HEADER             8

#define CD_MODE_PAGE_LEN_RW_ERROR               12

static uint32_t cd_scsi_add_mode_page_rw_error_recovery(cd_scsi_lu *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_RW_ERROR;

    outbuf[0] = MODE_PAGE_R_W_ERROR;
    outbuf[1] = CD_MODE_PAGE_LEN_RW_ERROR - 2;
    outbuf[3] = 1; /* read retry count */

    return page_len;
}

#define CD_MODE_PAGE_LEN_POWER                  12

static uint32_t cd_scsi_add_mode_page_power_condition(cd_scsi_lu *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_POWER;

    outbuf[0] = MODE_PAGE_POWER;
    outbuf[1] = CD_MODE_PAGE_LEN_POWER - 2;

    return page_len;
}

#define CD_MODE_PAGE_LEN_FAULT_FAIL             12
#define CD_MODE_PAGE_FAULT_FAIL_FLAG_PERF       0x80

static uint32_t cd_scsi_add_mode_page_fault_reporting(cd_scsi_lu *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_FAULT_FAIL;

    outbuf[0] = MODE_PAGE_FAULT_FAIL;
    outbuf[1] = CD_MODE_PAGE_LEN_FAULT_FAIL - 2;
    outbuf[2] |= CD_MODE_PAGE_FAULT_FAIL_FLAG_PERF;

    return page_len;
}

#define CD_MODE_PAGE_LEN_CAPS_MECH_STATUS_RO    26
/* byte 2 */
#define CD_MODE_PAGE_CAPS_CD_R_READ             0x01
#define CD_MODE_PAGE_CAPS_CD_RW_READ            (0x01 << 1)
#define CD_MODE_PAGE_CAPS_DVD_ROM_READ          (0x01 << 3)
#define CD_MODE_PAGE_CAPS_DVD_R_READ            (0x01 << 4)
#define CD_MODE_PAGE_CAPS_DVD_RAM_READ          (0x01 << 5)
/* byte 6 */
#define CD_MODE_PAGE_CAPS_EJECT                 (0x01 << 3)
#define CD_MODE_PAGE_CAPS_LOADING_TRAY          (0x01 << 5)

static uint32_t cd_scsi_add_mode_page_caps_mech_status(cd_scsi_lu *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_CAPS_MECH_STATUS_RO; /* no write */

    outbuf[0] = MODE_PAGE_CAPS_MECH_STATUS;
    outbuf[1] = page_len;
    outbuf[2] = CD_MODE_PAGE_CAPS_CD_R_READ | CD_MODE_PAGE_CAPS_CD_RW_READ |
                CD_MODE_PAGE_CAPS_DVD_ROM_READ | CD_MODE_PAGE_CAPS_DVD_R_READ |
                CD_MODE_PAGE_CAPS_DVD_RAM_READ;
    outbuf[6] = CD_MODE_PAGE_CAPS_LOADING_TRAY | CD_MODE_PAGE_CAPS_EJECT;

    return page_len;
}

static void cd_scsi_cmd_mode_sense_10(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    int long_lba, dbd, page, sub_page, pc;
    uint32_t resp_len = CD_MODE_PARAM_10_LEN_HEADER;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    long_lba = (req->cdb[1] >> 4) & 0x1;
    dbd = (req->cdb[1] >> 3) & 0x1;
    page = req->cdb[2] & 0x3f;    
    pc = req->cdb[2] >> 6;
    sub_page = req->cdb[3] & 0xf;

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];

    memset(outbuf, 0, req->req_len);
    outbuf[2] =  0; /* medium type */
    
    switch (page) {
    case MODE_PAGE_R_W_ERROR:
        /* Read/Write Error Recovery */
        resp_len += cd_scsi_add_mode_page_rw_error_recovery(dev, outbuf + resp_len);
        break;
    case MODE_PAGE_POWER:
        /* Power Condistions */
        resp_len += cd_scsi_add_mode_page_power_condition(dev, outbuf + resp_len);
        break;
    case MODE_PAGE_FAULT_FAIL:
        /* Fault / Failure Reporting Control */
        resp_len += cd_scsi_add_mode_page_fault_reporting(dev, outbuf + resp_len);
        break;
    case MODE_PAGE_CAPS_MECH_STATUS:
        resp_len += cd_scsi_add_mode_page_caps_mech_status(dev, outbuf + resp_len);
        break;

    /* not implemented */
    case MODE_PAGE_WRITE_PARAMETER: /* Writer Parameters */
    case MODE_PAGE_MRW:
    case MODE_PAGE_MRW_VENDOR: /* MRW (Mount Rainier Re-writable Disks */
    case MODE_PAGE_CD_DEVICE: /* CD Device parameters */
    case MODE_PAGE_TO_PROTECT: /* Time-out and Protect */
    default:
        SPICE_DEBUG("mode_sense_10, lun:%" G_GUINT32_FORMAT
                    " page 0x%x not implemented",
                    req->lun, (unsigned)page);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }

    outbuf[0] = ((resp_len - 2) >> 8) & 0xff;
    outbuf[1] = (resp_len - 2) & 0xff;

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    SPICE_DEBUG("mode_sense_10, lun:%" G_GUINT32_FORMAT
                " long_lba %d, dbd %d, page %d, sub_page %d, pc %d; "
                "resp_len %" G_GUINT32_FORMAT,
                req->lun, long_lba, dbd, page, sub_page, pc, resp_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_mode_select_6(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *block_desc_data, *mode_data;
    uint32_t page_format, save_pages, list_len; /* cdb */
    uint32_t num_blocks = 0, block_len = 0; /* block descriptor */
    uint32_t mode_len, medium_type, dev_param, block_desc_len; /* mode param header */
    uint32_t page_num = 0, page_len = 0; /* mode page */

    page_format = (req->cdb[1] >> 4) & 0x1;
    save_pages = req->cdb[1] & 0x1;
    list_len = req->cdb[4];

    if (list_len > req->buf_len) {
        SPICE_DEBUG("mode_select_6, lun:%" G_GUINT32_FORMAT
                    " pf:%" G_GUINT32_FORMAT " sp:%" G_GUINT32_FORMAT
                    " list_len:%" G_GUINT32_FORMAT " exceeds data_len:%" G_GUINT32_FORMAT,
                    req->lun, page_format, save_pages, list_len, req->buf_len);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_PARAM_LEN);
        return;
    }

    mode_len = req->buf[0];
    medium_type = req->buf[1];
    dev_param = req->buf[2];
    block_desc_len = req->buf[3];

    if (block_desc_len) {
        block_desc_data = &req->buf[CD_MODE_PARAM_6_LEN_HEADER];
        num_blocks = (block_desc_data[3] << 16) | (block_desc_data[2] << 8) | block_desc_data[3];
        block_len = (block_desc_data[5] << 16) | (block_desc_data[6] << 8) | block_desc_data[7];
    }

    if (mode_len) {
        mode_data = &req->buf[CD_MODE_PARAM_6_LEN_HEADER];
        if (block_desc_len) {
            mode_data += block_desc_len;
        }
        page_num = mode_data[0] & 0x3f;
        page_len = mode_data[1];
    }

    SPICE_DEBUG("mode_select_6, lun:%" G_GUINT32_FORMAT
                " pf:%" G_GUINT32_FORMAT " sp:%" G_GUINT32_FORMAT
                " list_len:%" G_GUINT32_FORMAT " data_len:%" G_GUINT32_FORMAT
                " mode_len:%" G_GUINT32_FORMAT " medium:%" G_GUINT32_FORMAT
                " dev_param:%" G_GUINT32_FORMAT " blk_desc_len:%" G_GUINT32_FORMAT
                " num_blocks:%" G_GUINT32_FORMAT " block_len:%" G_GUINT32_FORMAT
                " page_num:%" G_GUINT32_FORMAT " page_len:%" G_GUINT32_FORMAT,
                req->lun, page_format, save_pages, list_len, req->buf_len,
                mode_len, medium_type, dev_param, block_desc_len,
                num_blocks, block_len,
                page_num, page_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_mode_select_10(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint32_t page_format, save_pages, list_len;

    page_format = (req->cdb[1] >> 4) & 0x1;
    save_pages = req->cdb[1] & 0x1;
    list_len = (req->cdb[7] << 8) | req->cdb[8];

    if (list_len > req->buf_len) {
        SPICE_DEBUG("mode_select_10, lun:%" G_GUINT32_FORMAT
                    " pf:%" G_GUINT32_FORMAT " sp:%" G_GUINT32_FORMAT
                    " list_len:%" G_GUINT32_FORMAT " exceeds data_len:%" G_GUINT32_FORMAT,
                    req->lun, page_format, save_pages, list_len, req->buf_len);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_PARAM_LEN);
        return;
    }

    SPICE_DEBUG("mode_select_10, lun:%" G_GUINT32_FORMAT
                " pf:%" G_GUINT32_FORMAT " sp:%" G_GUINT32_FORMAT
                " list_len:%" G_GUINT32_FORMAT " data_len:%" G_GUINT32_FORMAT,
                req->lun, page_format, save_pages, list_len, req->buf_len);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_FEATURE_HEADER_LEN               8
#define CD_FEATURE_DESC_LEN                 4

#define CD_PROFILE_DESC_LEN                 4
#define CD_PROFILE_CURRENT                  0x01

/* Profiles List */
#define CD_FEATURE_NUM_PROFILES_LIST        0x00
/* Core - Basic Functionality */
#define CD_FEATURE_NUM_CORE                 0x01
/* Morphing - The device changes its behavior due to external events */
#define CD_FEATURE_NUM_MORPH                0x02
/* Removable Medium - The medium may be removed from the device */
#define CD_FEATURE_NUM_REMOVABLE            0x03
/* Random Readable - PP=1 Read ability for storage devices with random addressing */
#define CD_FEATURE_NUM_RANDOM_READ          0x10
/* CD Read - The ability to read CD specific structures */
#define CD_FEATURE_NUM_CD_READ              0x1E
/* DVD Read - The ability to read DVD specific structures */
#define CD_FEATURE_NUM_DVD_READ             0x1F
/* Power Management - Initiator and device directed power management */
#define CD_FEATURE_NUM_POWER_MNGT           0x100
/* Timeout */
#define CD_FEATURE_NUM_TIMEOUT              0x105

#define CD_FEATURE_REQ_ALL                  0
#define CD_FEATURE_REQ_CURRENT              1
#define CD_FEATURE_REQ_SINGLE               2

#define CD_FEATURE_CURRENT                  0x01
#define CD_FEATURE_PERSISTENT               0x02

#define CD_FEATURE_VERSION_1                (0x01 << 2)

#define CD_FEATURE_PHYS_IF_SCSI             0x01

#define CD_FEATURE_REMOVABLE_LOADING_TRAY   (0x01 << 5)
#define CD_FEATURE_REMOVABLE_EJECT          (0x01 << 3)
#define CD_FEATURE_REMOVABLE_NO_PRVNT_JMPR  (0x01 << 2)
#define CD_FEATURE_REMOVABLE_LOCK           (0x01)

static gboolean cd_scsi_feature_reportable(uint32_t feature, uint32_t start_feature, uint32_t req_type)
{
    return (req_type == CD_FEATURE_REQ_SINGLE && start_feature == feature) ||
           (feature >= start_feature);
}

static uint32_t cd_scsi_add_feature_profiles_list(cd_scsi_lu *dev, uint8_t *outbuf,
                                                  uint32_t start_feature, uint32_t req_type)
{
    uint8_t *profile = outbuf + CD_FEATURE_DESC_LEN;
    uint32_t feature_len = CD_FEATURE_DESC_LEN;
    uint32_t add_len, profile_num;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_PROFILES_LIST, start_feature, req_type)) {
        return 0;
    }
    /* feature descriptor header */
    outbuf[0] = (CD_FEATURE_NUM_PROFILES_LIST >> 8) & 0xff;
    outbuf[1] = CD_FEATURE_NUM_PROFILES_LIST & 0xff;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;

    /* DVD-ROM profile descriptor */
    add_len = CD_PROFILE_DESC_LEN; /* start with single profile, add later */
    profile_num = MMC_PROFILE_DVD_ROM;

    profile[0] = (profile_num >> 8) & 0xff; /* feature code */
    profile[1] = profile_num & 0xff;
    profile[2] = (!dev->cd_rom) ? CD_PROFILE_CURRENT : 0;

    /* next profile */
    add_len += CD_PROFILE_DESC_LEN;
    profile += CD_PROFILE_DESC_LEN;

    /* CD-ROM profile descriptor */
    profile_num = MMC_PROFILE_CD_ROM;
    profile[0] = (profile_num >> 8) & 0xff;
    profile[1] = profile_num & 0xff;
    profile[2] = dev->cd_rom ? CD_PROFILE_CURRENT : 0;

    outbuf[3] = add_len;
    feature_len += add_len;

    return feature_len;
}

#define CD_FEATURE_CORE_PHYS_PROFILE_LEN    4

static uint32_t cd_scsi_add_feature_core(cd_scsi_lu *dev, uint8_t *outbuf,
                                         uint32_t start_feature, uint32_t req_type)
{
    uint8_t *profile = outbuf + CD_FEATURE_DESC_LEN;
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_CORE_PHYS_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_CORE, start_feature, req_type)) {
        return 0;
    }
    outbuf[0] = (CD_FEATURE_NUM_CORE >> 8) & 0xff;
    outbuf[1] = CD_FEATURE_NUM_CORE & 0xff;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_CORE_PHYS_PROFILE_LEN;

    profile[3] = CD_FEATURE_PHYS_IF_SCSI;

    return feature_len;
}

#define CD_FEATURE_MORPH_PROGILE_LEN    4
#define CD_FEATURE_MORPH_ASYNC_EVENTS   0x01

static uint32_t cd_scsi_add_feature_morph(cd_scsi_lu *dev, uint8_t *outbuf,
                                          uint32_t start_feature, uint32_t req_type)
{
    uint8_t *profile = outbuf + CD_FEATURE_DESC_LEN;
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_MORPH_PROGILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_MORPH, start_feature, req_type)) {
        return 0;
    }
    outbuf[1] = CD_FEATURE_NUM_MORPH;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_MORPH_PROGILE_LEN;

    profile[0] = CD_FEATURE_MORPH_ASYNC_EVENTS;

    return feature_len;
}

#define CD_FEATURE_REMOVABLE_PROFILE_LEN    4

static uint32_t cd_scsi_add_feature_removable(cd_scsi_lu *dev, uint8_t *outbuf,
                                              uint32_t start_feature, uint32_t req_type)
{
    uint8_t *profile = outbuf + CD_FEATURE_DESC_LEN;
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_REMOVABLE_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_REMOVABLE, start_feature, req_type)) {
        return 0;
    }
    outbuf[1] = CD_FEATURE_NUM_REMOVABLE;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_REMOVABLE_PROFILE_LEN;

    profile[0] = CD_FEATURE_REMOVABLE_NO_PRVNT_JMPR;
    if (dev->removable) {
        profile[0] |= (CD_FEATURE_REMOVABLE_LOADING_TRAY | CD_FEATURE_REMOVABLE_EJECT);
    }

    return feature_len;
}

#define CD_FEATURE_RANDOM_READ_PROFILE_LEN    8

static uint32_t cd_scsi_add_feature_random_read(cd_scsi_lu *dev, uint8_t *outbuf,
                                                uint32_t start_feature, uint32_t req_type)
{
    uint8_t *profile = outbuf + CD_FEATURE_DESC_LEN;
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_RANDOM_READ_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_RANDOM_READ, start_feature, req_type)) {
        return 0;
    }
    outbuf[0] = (CD_FEATURE_NUM_RANDOM_READ >> 8) & 0xff;
    outbuf[1] = CD_FEATURE_NUM_RANDOM_READ & 0xff;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_RANDOM_READ_PROFILE_LEN;

    profile[0] = (dev->block_size >> 24) & 0xff;
    profile[1] = (dev->block_size >> 16) & 0xff;
    profile[2] = (dev->block_size >> 8) & 0xff;
    profile[3] = (dev->block_size) & 0xff;
    profile[5] = (dev->cd_rom) ? 0x01 : 0x10; /* logical blocks per readable unit */

    return feature_len;
}

#define CD_FEATURE_CD_READ_PROFILE_LEN    4

static uint32_t cd_scsi_add_feature_cd_read(cd_scsi_lu *dev, uint8_t *outbuf,
                                            uint32_t start_feature, uint32_t req_type)
{
    uint8_t *profile = outbuf + CD_FEATURE_DESC_LEN;
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_CD_READ_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_CD_READ, start_feature, req_type)) {
        return 0;
    }
    outbuf[0] = (CD_FEATURE_NUM_CD_READ >> 8) & 0xff;
    outbuf[1] = (CD_FEATURE_NUM_CD_READ) & 0xff;
    outbuf[2] = CD_FEATURE_VERSION_1 | CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_CD_READ_PROFILE_LEN;

    profile[0] = 0; /* C2 Errors, CD-Text not supporte */

    return feature_len;
}

#define CD_FEATURE_DVD_READ_PROFILE_LEN    0

static uint32_t cd_scsi_add_feature_dvd_read(cd_scsi_lu *dev, uint8_t *outbuf,
                                             uint32_t start_feature, uint32_t req_type)
{
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_DVD_READ_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_CD_READ, start_feature, req_type)) {
        return 0;
    }
    outbuf[0] = (CD_FEATURE_NUM_DVD_READ >> 8) & 0xff;
    outbuf[1] = (CD_FEATURE_NUM_DVD_READ) & 0xff;
    outbuf[2] = CD_FEATURE_VERSION_1 | CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_DVD_READ_PROFILE_LEN;

    return feature_len;
}

#define CD_FEATURE_POWER_MNGT_PROFILE_LEN    0

static uint32_t cd_scsi_add_feature_power_mgmt(cd_scsi_lu *dev, uint8_t *outbuf,
                                               uint32_t start_feature, uint32_t req_type)
{
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_POWER_MNGT_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_POWER_MNGT, start_feature, req_type)) {
        return 0;
    }
    outbuf[0] = (CD_FEATURE_NUM_POWER_MNGT >> 8) & 0xff;
    outbuf[1] = (CD_FEATURE_NUM_POWER_MNGT) & 0xff;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_POWER_MNGT_PROFILE_LEN;

    return feature_len;
}

#define CD_FEATURE_TIMEOUT_PROFILE_LEN    0

static uint32_t cd_scsi_add_feature_timeout(cd_scsi_lu *dev, uint8_t *outbuf,
                                            uint32_t start_feature, uint32_t req_type)
{
    uint32_t feature_len = CD_FEATURE_DESC_LEN + CD_FEATURE_TIMEOUT_PROFILE_LEN;

    if (!cd_scsi_feature_reportable(CD_FEATURE_NUM_TIMEOUT, start_feature, req_type)) {
        return 0;
    }
    outbuf[0] = (CD_FEATURE_NUM_TIMEOUT >> 8) & 0xff;
    outbuf[1] = CD_FEATURE_NUM_TIMEOUT & 0xff;
    outbuf[2] = CD_FEATURE_PERSISTENT | CD_FEATURE_CURRENT;
    outbuf[3] = CD_FEATURE_TIMEOUT_PROFILE_LEN;

    return feature_len;
}

static void cd_scsi_cmd_get_configuration(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t profile_num = (!dev->cd_rom) ? MMC_PROFILE_DVD_ROM : MMC_PROFILE_CD_ROM;
    uint32_t req_type, start_feature, resp_len;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    req_type = req->cdb[1] & 0x3;
    start_feature = (req->cdb[2] << 8) | req->cdb[3]; 
    req->req_len = (req->cdb[7] << 8) | req->cdb[8];

    memset(outbuf, 0, req->req_len);

    /* at least Feature Header should be present, to be filled later */
    resp_len = CD_FEATURE_HEADER_LEN;

    switch (req_type) {
    case CD_FEATURE_REQ_ALL:
    case CD_FEATURE_REQ_CURRENT:
        resp_len += cd_scsi_add_feature_profiles_list(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_core(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_morph(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_removable(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_random_read(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_cd_read(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_dvd_read(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_power_mgmt(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_timeout(dev, outbuf + resp_len, start_feature, req_type);
        break;
    case CD_FEATURE_REQ_SINGLE:
        switch (start_feature) {
        case CD_FEATURE_NUM_CORE:
            resp_len += cd_scsi_add_feature_core(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_MORPH:
            resp_len += cd_scsi_add_feature_morph(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_REMOVABLE:
            resp_len += cd_scsi_add_feature_removable(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_RANDOM_READ:
            resp_len += cd_scsi_add_feature_random_read(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_CD_READ:
            resp_len += cd_scsi_add_feature_cd_read(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_DVD_READ:
            resp_len += cd_scsi_add_feature_dvd_read(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_POWER_MNGT:
            resp_len += cd_scsi_add_feature_power_mgmt(dev, outbuf + resp_len, start_feature, req_type);
            break;
        case CD_FEATURE_NUM_TIMEOUT:
            resp_len += cd_scsi_add_feature_timeout(dev, outbuf + resp_len, start_feature, req_type);
            break;
        default:
            break;
        }
        break;

    default:
        SPICE_DEBUG("get_configuration, lun:%" G_GUINT32_FORMAT 
                    " invalid rt:%" G_GUINT32_FORMAT " start_f:%" G_GUINT32_FORMAT,
                    req->lun, req_type, start_feature);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    } 

    /* set total data len */
    outbuf[0] = (resp_len >> 24) & 0xff;
    outbuf[1] = (resp_len >> 16) & 0xff;
    outbuf[2] = (resp_len >> 8) & 0xff;
    outbuf[3] = resp_len & 0xff;

    /* report current profile num */
    outbuf[6] = (profile_num >> 8) & 0xff;
    outbuf[7] = profile_num & 0xff;

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    SPICE_DEBUG("get_configuration, lun:%" G_GUINT32_FORMAT 
                " rt:%" G_GUINT32_FORMAT " start_f:%" G_GUINT32_FORMAT 
                " resp_len:%" G_GUINT32_FORMAT, 
                req->lun, req_type, start_feature, resp_len);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_GET_EVENT_STATUS_IMMED   0x01
#define CD_GET_EVENT_HEADER_NEA     (0x01 << 7)
#define CD_GET_EVENT_HEADER_LEN     4

#define CD_GET_EVENT_CLASS_OPER_CHANGE      (0x01)
#define CD_GET_EVENT_CLASS_POWER_MGMT       (0x02)
#define CD_GET_EVENT_CLASS_EXTERNAL_REQ     (0x03)
#define CD_GET_EVENT_CLASS_MEDIA            (0x04)
#define CD_GET_EVENT_CLASS_MULTI_INITIATOR  (0x05)
#define CD_GET_EVENT_CLASS_DEV_BUSY         (0x06)

#define CD_GET_EVENT_LEN_MEDIA              4

#define CD_MEDIA_EVENT_NO_CHANGE            0x0
#define CD_MEDIA_EVENT_EJECT_REQ            0x1
#define CD_MEDIA_EVENT_NEW_MEDIA            0x2
#define CD_MEDIA_EVENT_MEDIA_REMOVAL        0x3
#define CD_MEDIA_EVENT_MEDIA_CHANGED        0x4
#define CD_MEDIA_EVENT_BG_FORMAT_COMPLETE   0x5
#define CD_MEDIA_EVENT_BG_FORMAT_RESTART    0x6

#define CD_MEDIA_STATUS_MEDIA_PRESENT       0x1
#define CD_MEDIA_STATUS_TRAY_OPEN           0x2

static uint32_t cd_scsi_cmd_get_event_resp_add_media(cd_scsi_lu *dev, uint8_t *outbuf)
{
    outbuf[0] = CD_MEDIA_EVENT_NO_CHANGE & 0x0f;
    outbuf[1] = CD_MEDIA_STATUS_MEDIA_PRESENT;

    return CD_GET_EVENT_LEN_MEDIA;
}

static void cd_scsi_cmd_get_event_status_notification(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t immed, class_req;
    uint32_t resp_len = CD_GET_EVENT_HEADER_LEN;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    immed = req->cdb[1] & CD_GET_EVENT_STATUS_IMMED;
    class_req = req->cdb[4];
    req->req_len = (req->cdb[7] << 8) | req->cdb[8];

    memset(outbuf, 0, req->req_len);
    if (class_req & CD_GET_EVENT_CLASS_MEDIA) {
        outbuf[2] = CD_GET_EVENT_CLASS_MEDIA;
        outbuf[3] = (0x01 << CD_GET_EVENT_CLASS_MEDIA);
        resp_len += cd_scsi_cmd_get_event_resp_add_media(dev, outbuf + resp_len);
    } else {
        outbuf[2] = CD_GET_EVENT_HEADER_NEA;
    }

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    SPICE_DEBUG("get_event_status_notification, lun:%" G_GUINT32_FORMAT 
                " imm:%" G_GUINT32_FORMAT " class_req:%02x",
                req->lun, immed, class_req);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_EXT_REQ_EVENT_FORMAT_NO_CHG          0x00
#define CD_EXT_REQ_EVENT_FORMAT_LU_KEY_DOWN     0x01
#define CD_EXT_REQ_EVENT_FORMAT_LU_KEY_UP       0x02
#define CD_EXT_REQ_EVENT_FORMAT_REQ_NOTIFY      0x03

#define CD_EXT_REQ_STATUS_READY                 0x00
#define CD_EXT_REQ_STATUS_OTHER_PREVENT         0x01

#define CD_EXT_REQ_CODE_NO_REQUEST              0x00
#define CD_EXT_REQ_CODE_OVERRUN                 0x01
#define CD_EXT_REQ_CODE_PLAY                    0x101
#define CD_EXT_REQ_CODE_REWIND                  0x102
#define CD_EXT_REQ_CODE_FAST_FW                 0x103
#define CD_EXT_REQ_CODE_PAUSE                   0x104
#define CD_EXT_REQ_CODE_STOP                    0x106
#define CD_EXT_REQ_CODE_ASCII_BASE              0x200 /* SCSII value is LSB */

static void cd_scsi_cmd_send_event(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *param, *event;
    uint32_t immed, param_list_len;
    uint32_t event_param_len, notification_class;
    uint32_t ext_req_event, ext_req_status, pers_prevent, ext_req_code;

    req->xfer_dir = SCSI_XFER_TO_DEV;

    immed = req->cdb[1] & 0x01;
    param_list_len = (req->cdb[8] << 8) | req->cdb[9];

    if (req->buf_len < param_list_len) {
        SPICE_DEBUG("send_event, lun:%" G_GUINT32_FORMAT
                    " invalid param list len:0x%x, buf_len:0x%x",
                    req->lun, param_list_len, req->buf_len);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_PARAM_LEN);
        return;
    }
    param = req->buf;
    event_param_len = (param[0] << 8) | param[1];

    notification_class = param[2] & 0x07;
    if (notification_class != CD_GET_EVENT_CLASS_EXTERNAL_REQ) {
        SPICE_DEBUG("send_event, lun:%" G_GUINT32_FORMAT 
                    " invalid notification class:0x%x",
                    req->lun, notification_class);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }

    event = param + CD_GET_EVENT_HEADER_LEN;
    ext_req_event = event[0] & 0xff;    
    ext_req_status = event[1] & 0x0f;
    pers_prevent = event[1] & 0x80;
    ext_req_code = (event[2] << 8) | event[3];    

    SPICE_DEBUG("send_event, lun:0x%" G_GUINT32_FORMAT
                " immed:%" G_GUINT32_FORMAT " param_len:%" G_GUINT32_FORMAT
                " ext_req_event:0x%x ext_req_status:0x%x"
                " pers_prevent:0x%x ext_req_code:0x%x",
                req->lun, immed, event_param_len, ext_req_event, 
                ext_req_status, pers_prevent, ext_req_code);

    /* ToDo: process the event */

    cd_scsi_cmd_complete_good(dev, req);
}



#define CD_MEDIUM_REMOVAL_REQ_ALLOW                 0x00
#define CD_MEDIUM_REMOVAL_REQ_PREVENT               0x01
#define CD_MEDIUM_REMOVAL_REQ_ALLOW_CHANGER         0x02
#define CD_MEDIUM_REMOVAL_REQ_PREVENT_CHANGER       0x03

static void cd_scsi_cmd_allow_medium_removal(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint32_t prevent;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    prevent = req->cdb[4] & 0x03;
    dev->prevent_media_removal = (prevent == CD_MEDIUM_REMOVAL_REQ_ALLOW || 
                                  prevent == CD_MEDIUM_REMOVAL_REQ_ALLOW_CHANGER);
    req->in_len = 0;

    SPICE_DEBUG("allow_medium_removal, lun:%" G_GUINT32_FORMAT " prevent:0x%02x",
                req->lun, prevent);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_report_key(cd_scsi_lu *dev, cd_scsi_request *req)
{
    SPICE_DEBUG("report_key - content protection unsupported, lun:%" G_GUINT32_FORMAT, req->lun);
    req->xfer_dir = SCSI_XFER_NONE;
    cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_OPCODE);
}

static void cd_scsi_cmd_send_key(cd_scsi_lu *dev, cd_scsi_request *req)
{
    SPICE_DEBUG("send_key - content protection unsupported, lun:%" G_GUINT32_FORMAT, req->lun);
    req->xfer_dir = SCSI_XFER_NONE;
    cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_OPCODE);
}

/* byte 1 */
#define CD_START_STOP_FLAG_IMMED                    0x01

/* byte 4 */
#define CD_START_STOP_FLAG_START                    0x01
#define CD_START_STOP_FLAG_LOEJ                     0x02

/* POWER CONDITION field values */
#define CD_START_STOP_POWER_COND_START_VALID        0x00
#define CD_START_STOP_POWER_COND_ACTIVE             0x01
#define CD_START_STOP_POWER_COND_IDLE               0x02
#define CD_START_STOP_POWER_COND_STANDBY            0x03
#define CD_START_STOP_POWER_COND_LU_CONTROL         0x07
#define CD_START_STOP_POWER_COND_FORCE_IDLE_0       0x0a
#define CD_START_STOP_POWER_COND_FORCE_STANDBY_0    0x0b

static inline const char *cd_scsi_start_stop_power_cond_name(uint32_t power_cond)
{
    switch (power_cond) {
    case CD_START_STOP_POWER_COND_START_VALID:
        return "START_VALID";
    case CD_START_STOP_POWER_COND_ACTIVE:
        return "ACTIVE";
    case CD_START_STOP_POWER_COND_IDLE:
        return "IDLE";
    case CD_START_STOP_POWER_COND_STANDBY:
        return "STANDBY";
    case CD_START_STOP_POWER_COND_LU_CONTROL:
        return "LU_CONTROL";
    case CD_START_STOP_POWER_COND_FORCE_IDLE_0:
        return "FORCE_IDLE_0";
    case CD_START_STOP_POWER_COND_FORCE_STANDBY_0:
        return "FORCE_STANDBY_0";
    default:
        return "RESERVED";
    }
}

static void cd_scsi_cmd_start_stop_unit(cd_scsi_lu *dev, cd_scsi_request *req)
{
    gboolean immed, start, load_eject;
    uint32_t power_cond;

    req->xfer_dir = SCSI_XFER_NONE;
    req->in_len = 0;

    immed = (req->cdb[1] & CD_START_STOP_FLAG_IMMED) ? TRUE : FALSE;
    start = (req->cdb[4] & CD_START_STOP_FLAG_START) ? TRUE : FALSE;
    load_eject = (req->cdb[4] & CD_START_STOP_FLAG_LOEJ) ? TRUE : FALSE;
    power_cond = req->cdb[4] >> 4;

    SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT
                " immed:%d start:%d load_eject:%d power_cond:0x%x(%s)",
                req->lun, immed, start, load_eject, power_cond,
                cd_scsi_start_stop_power_cond_name(power_cond));

    switch (power_cond) {
    case CD_START_STOP_POWER_COND_START_VALID:
        if (!start) { /* stop the unit */
            if (load_eject) { /* eject medium */
                dev->loaded = FALSE;
                SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " ejected", req->lun);
            }
            dev->power_cond = CD_SCSI_POWER_STOPPED;
            SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " stopped", req->lun);
        } else { /* start the unit */
            dev->power_cond = CD_SCSI_POWER_ACTIVE;
            SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " started", req->lun);

            if (load_eject) { /* load medium */
                dev->loaded = TRUE;
                SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " loaded", req->lun);
            }
        }
        break;
    case CD_START_STOP_POWER_COND_ACTIVE:
        dev->power_cond = CD_SCSI_POWER_ACTIVE;
        SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " active", req->lun);
        break;
    case CD_START_STOP_POWER_COND_IDLE:
    case CD_START_STOP_POWER_COND_FORCE_IDLE_0:
        dev->power_cond = CD_SCSI_POWER_IDLE;
        SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " idle", req->lun);
        break;
    case CD_START_STOP_POWER_COND_STANDBY:
    case CD_START_STOP_POWER_COND_FORCE_STANDBY_0:
        dev->power_cond = CD_SCSI_POWER_STANDBY;
        SPICE_DEBUG("start_stop_unit, lun:0x%" G_GUINT32_FORMAT " standby", req->lun);
        break;
    case CD_START_STOP_POWER_COND_LU_CONTROL:
        break;
    default:
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }
    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_PERF_TYPE_PERFORMANCE                0x00
#define CD_PERF_TYPE_UNUSABLE_AREA              0x01
#define CD_PERF_TYPE_DEFECT_STATUS              0x02
#define CD_PERF_TYPE_WRITE_SPEED                0x03

#define CD_PERF_HEADER_LEN                      8

#define CD_PERF_TYPE_PERFORMANCE_DESCR_LEN      16

#define CD_PERF_TYPE_PERFORMANCE_REPORT_NOMINAL 0x00
#define CD_PERF_TYPE_PERFORMANCE_REPORT_ALL     0x01
#define CD_PERF_TYPE_PERFORMANCE_REPORT_EXCEPT  0x10


static void cd_scsi_get_performance_resp_empty(cd_scsi_lu *dev, cd_scsi_request *req,
                                               uint32_t type, uint32_t data_type,
                                               uint32_t max_num_descr)
{
    uint8_t *outbuf = req->buf;
    uint32_t write = (data_type >> 2) & 0x01;

    memset(outbuf, 0, CD_PERF_HEADER_LEN);
    if (write) {
        outbuf[4] = 0x02;
    }
    req->in_len = CD_PERF_HEADER_LEN;

    SPICE_DEBUG("get_performance, lun:%" G_GUINT32_FORMAT
                " type:0x%x data_type:0x%x - sending empty response",
                req->lun, type, data_type);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_get_performance_resp_performance(cd_scsi_lu *dev, cd_scsi_request *req, 
                                                     uint32_t start_lba,
                                                     uint32_t data_type,
                                                     uint32_t max_num_descr)
{
    uint8_t *outbuf = req->buf, *perf_desc;
    uint32_t resp_len = CD_PERF_HEADER_LEN +
                        CD_PERF_TYPE_PERFORMANCE_DESCR_LEN;
    uint32_t perf_data_len = resp_len - 4; /* not incl. Perf Data Length */
    uint32_t perf_kb = 10000;
    uint32_t end_lba = dev->num_blocks - 1;
    uint32_t except, write, tolerance;

    except = data_type & 0x03;
    if (except != CD_PERF_TYPE_PERFORMANCE_REPORT_ALL) {
        start_lba = 0;
    }
    write = (data_type >> 2) & 0x01;
    tolerance = (data_type >> 3) & 0x03;

    SPICE_DEBUG("get_performance, lun:%" G_GUINT32_FORMAT
                " performance type:0x00 data_type:0x%x"
                " except:0x%x write:0x%x tolerance:0x%x"
                " max_num:%" G_GUINT32_FORMAT,
                req->lun, data_type, except, write,
                tolerance, max_num_descr);
    
    if (write) {
        SPICE_DEBUG("get_performance, lun:%" G_GUINT32_FORMAT
                " performance type:0x00 data_type:0x%x - write unsupported",
                req->lun, data_type);
        cd_scsi_get_performance_resp_empty(dev, req, CD_PERF_TYPE_PERFORMANCE,
                                           data_type, max_num_descr);
        return;
    }

    memset(outbuf, 0, resp_len);

    outbuf[0] = (perf_data_len >> 24) & 0xff;
    outbuf[1] = (perf_data_len >> 16) & 0xff;
    outbuf[2] = (perf_data_len >> 8) & 0xff;
    outbuf[3] = perf_data_len & 0xff;

    perf_desc = outbuf + CD_PERF_HEADER_LEN;

    perf_desc[0] = (start_lba >> 24) & 0xff;
    perf_desc[1] = (start_lba >> 16) & 0xff;
    perf_desc[2] = (start_lba >> 8) & 0xff;
    perf_desc[3] = start_lba & 0xff;

    perf_desc[4] = (perf_kb >> 24) & 0xff;
    perf_desc[5] = (perf_kb >> 16) & 0xff;
    perf_desc[6] = (perf_kb >> 8) & 0xff;
    perf_desc[7] = perf_kb & 0xff;

    perf_desc[8] = (end_lba >> 24) & 0xff;
    perf_desc[9] = (end_lba >> 16) & 0xff;
    perf_desc[10] = (end_lba >> 8) & 0xff;
    perf_desc[11] = end_lba & 0xff;

    perf_desc[12] = (perf_kb >> 24) & 0xff;
    perf_desc[13] = (perf_kb >> 16) & 0xff;
    perf_desc[14] = (perf_kb >> 8) & 0xff;
    perf_desc[15] = perf_kb & 0xff;

    req->req_len = CD_PERF_HEADER_LEN +
                   (max_num_descr * CD_PERF_TYPE_PERFORMANCE_DESCR_LEN);

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_get_performance(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint32_t data_type, max_num_descr, start_lba, type;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    data_type = req->cdb[1] & 0x0f;
    start_lba = (req->cdb[2] << 24) |
                (req->cdb[3] << 16) |
                (req->cdb[4] << 8) |
                 req->cdb[5];
    max_num_descr = (req->cdb[8] << 8) | req->cdb[9];
    type = req->cdb[10];

    switch (type) {
    case CD_PERF_TYPE_PERFORMANCE:
        cd_scsi_get_performance_resp_performance(dev, req, start_lba,
                                                 data_type, max_num_descr);
        break;
    case CD_PERF_TYPE_UNUSABLE_AREA: /* not writable */
    case CD_PERF_TYPE_DEFECT_STATUS: /* not restricted overwrite media */
    case CD_PERF_TYPE_WRITE_SPEED: /* unsupported, irrelevant */
    default:
        SPICE_DEBUG("get_performance, lun:%" G_GUINT32_FORMAT
                " unsupported type:0x%x"
                " data_type:0x%x max_num:%" G_GUINT32_FORMAT,
                req->lun, type, data_type, max_num_descr);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        return;
    }
}

#define CD_MECHANISM_STATUS_HDR_LEN     8

#define CD_CHANGER_FAULT_FLAG           0x80

#define CD_CHANGER_READY                0x00
#define CD_CHANGER_LOADING              0x01
#define CD_CHANGER_UNLOADING            0x02
#define CD_CHANGER_INITIALIZING         0x03

#define CD_MECHANISM_STATE_IDLE         0x00
#define CD_MECHANISM_STATE_PLAYING      0x01
#define CD_MECHANISM_STATE_SCANNING     0x02
/* ACTIVE: with Initiator, Composite or Other ports
  (i.e. READ, PLAY CD, SCAN during PLAY CD) */
#define CD_MECHANISM_STATE_ACTIVE       0x03
#define CD_MECHANISM_STATE_NO_INFO      0x07

#define CD_MECHANISM_STATUS_SLOT_LEN    4

#define CD_MECHANISM_SLOT_DISK_CHANGED  0x01
#define CD_MECHANISM_SLOT_DISK_PRESENT  0x80
#define CD_MECHANISM_SLOT_DISK_CWP_V    0x02
#define CD_MECHANISM_SLOT_DISK_CWP      0x01

static void cd_scsi_cmd_mechanism_status(cd_scsi_lu *dev, cd_scsi_request *req)
{
    uint8_t *outbuf = req->buf, *slot;
    uint32_t resp_len = CD_MECHANISM_STATUS_HDR_LEN;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    req->req_len = (req->cdb[8] << 8) | req->cdb[9];   
    memset(outbuf, 0, req->req_len);

    outbuf[0] = 0x01; /* current slot */
    outbuf[0] |= (CD_CHANGER_READY << 4);
    outbuf[1] |= (CD_MECHANISM_STATE_IDLE << 4);

    /* Slot table response descriptor */
    slot = outbuf + CD_MECHANISM_STATUS_HDR_LEN;
    if (dev->loaded) {
        slot[0] |= 0x80;
    }
    resp_len += CD_MECHANISM_STATUS_SLOT_LEN;

    req->in_len = (req->req_len < resp_len) ? req->req_len : resp_len;

    SPICE_DEBUG("mechanism_status, lun:%" G_GUINT32_FORMAT, req->lun);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_read_async_complete(GObject *src_object,
                                        GAsyncResult *result,
                                        gpointer user_data)
{
    GFileInputStream *stream = G_FILE_INPUT_STREAM(src_object);
    cd_scsi_request *req = (cd_scsi_request *)user_data;
    cd_scsi_target *st = (cd_scsi_target *)req->priv_data;
    cd_scsi_lu *dev = &st->units[req->lun];
    GError *error = NULL;
    gsize bytes_read;
    gboolean finished;

    req->req_state = SCSI_REQ_COMPLETE;
    req->cancel_id = 0;

//    g_assert(stream == dev->stream);
    if (stream != dev->stream) {
        uint32_t opcode = (uint32_t)req->cdb[0];
        SPICE_DEBUG("read_async_complete BAD STREAM, lun: %" G_GUINT32_FORMAT
                    " req: %" G_GUINT64_FORMAT " op: 0x%02x",
                    req->lun, req->req_len, opcode);
        cd_scsi_sense_check_cond(dev, req, &sense_code_TARGET_FAILURE);
        cd_scsi_dev_request_complete(st->user_data, req);
        return;
    }

    bytes_read = g_input_stream_read_finish(G_INPUT_STREAM(stream), result, &error);
    finished = bytes_read > 0;
    if (finished) {
        SPICE_DEBUG("read_async_complete, lun: %" G_GUINT32_FORMAT
                    " finished: %d bytes_read: %" G_GUINT64_FORMAT
                    " req: %"  G_GUINT64_FORMAT, 
                    req->lun, finished, (uint64_t)bytes_read, req->req_len);

        req->in_len = (bytes_read <= req->req_len) ? bytes_read : req->req_len;
        req->status = GOOD;
    } else {
        if (error != NULL) {
            SPICE_ERROR("g_input_stream_read_finish failed: %s", error->message);
            g_clear_error (&error);
        } else {
            SPICE_ERROR("g_input_stream_read_finish failed (no err provided)");
        }
        req->in_len = 0;
        req->status = GOOD;
    }
    cd_scsi_dev_request_complete(st->user_data, req);
}

static void cd_scsi_read_async_canceled(GCancellable *cancellable, gpointer user_data)
{
    cd_scsi_request *req = (cd_scsi_request *)user_data;
    cd_scsi_target *st = (cd_scsi_target *)req->priv_data;

    g_assert(cancellable == st->cancellable);
    g_cancellable_disconnect(cancellable, req->cancel_id);
    req->cancel_id = 0;

    req->req_state = (st->state == CD_SCSI_TGT_STATE_RUNNING) ? SCSI_REQ_CANCELED : SCSI_REQ_DISPOSED;
    req->in_len = 0;
    req->status = GOOD;

    cd_scsi_dev_request_complete(st->user_data, req);
}

static int cd_scsi_read_async_start(cd_scsi_lu *dev, cd_scsi_request *req)
{
    cd_scsi_target *st = dev->tgt;
    GFileInputStream *stream = dev->stream;

    SPICE_DEBUG("read_async_start, lun:%" G_GUINT32_FORMAT
                " lba: %" G_GUINT64_FORMAT " offset: %" G_GUINT64_FORMAT
                " cnt: %" G_GUINT64_FORMAT " len: %" G_GUINT64_FORMAT,
                req->lun, req->lba, req->offset, req->count, req->req_len);

    req->cancel_id = g_cancellable_connect(st->cancellable,
                                           G_CALLBACK(cd_scsi_read_async_canceled),
                                           req, /* data */
                                           NULL); /* data destroy cb */
    if (req->cancel_id == 0) {
        /* already canceled */
        return -1;
    }

    g_seekable_seek(G_SEEKABLE(stream),
                    req->offset,
                    G_SEEK_SET,
                    NULL, /* cancellable */
                    NULL); /* error */

    g_input_stream_read_async(G_INPUT_STREAM(stream),
                              req->buf, /* buffer to fill */
                              req->req_len,
                              G_PRIORITY_DEFAULT,
                              st->cancellable,
                              cd_scsi_read_async_complete,
                              (gpointer)req); /* callback argument */
    return 0;
}

static void cd_scsi_cmd_read(cd_scsi_lu *dev, cd_scsi_request *req)
{
    if (dev->power_cond == CD_SCSI_POWER_STOPPED) {
        SPICE_DEBUG("read, lun: %" G_GUINT32_FORMAT " is stopped", req->lun);
        cd_scsi_sense_check_cond(dev, req, &sense_code_INIT_CMD_REQUIRED);
        return;
    } else if (!dev->loaded) {
        SPICE_DEBUG("read, lun: %" G_GUINT32_FORMAT " is not loaded", req->lun);
        cd_scsi_sense_check_cond(dev, req, &sense_code_NO_MEDIUM);
        return;
    }

    req->cdb_len = scsi_cdb_length(req->cdb);

    req->lba = scsi_cdb_lba(req->cdb, req->cdb_len);
    req->offset = req->lba * dev->block_size;

    req->count = scsi_cdb_xfer_length(req->cdb, req->cdb_len); /* xfer in blocks */
    req->req_len = req->count * dev->block_size;

    cd_scsi_read_async_start(dev, req);
}

void cd_scsi_dev_request_submit(void *scsi_target, cd_scsi_request *req)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;
    uint32_t lun = req->lun;
    uint32_t opcode = (uint32_t)req->cdb[0];
    cd_scsi_lu *dev = &st->units[lun];

    SPICE_DEBUG("request_submit, lun: %" G_GUINT32_FORMAT " op: 0x%02x", lun, opcode);

    if (st->cur_req != NULL) {
        SPICE_ERROR("request_submit, request not idle");
        cd_scsi_sense_check_cond(dev, req, &sense_code_TARGET_FAILURE);
        goto done;
    }
    if (req->req_state != SCSI_REQ_IDLE) {
        SPICE_ERROR("request_submit, prev request outstanding");
        cd_scsi_sense_check_cond(dev, req, &sense_code_TARGET_FAILURE);
        goto done;
    }
    req->req_state = SCSI_REQ_RUNNING;
    st->cur_req = req;

    /* INQUIRY should send response even for non-existing LUNs */
    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("request_submit, illegal lun:%" G_GUINT32_FORMAT, lun);
        if (opcode == INQUIRY) {
            if (req->cdb[1] & 0x1) {
                cd_scsi_cmd_inquiry_vpd_no_lun(dev, req, PERIF_QUALIFIER_UNSUPPORTED);
            } else {
                cd_scsi_cmd_inquiry_standard_no_lun(dev, req, PERIF_QUALIFIER_UNSUPPORTED);
            }
        } else {
            cd_scsi_sense_check_cond(dev, req, &sense_code_LUN_NOT_SUPPORTED);
        }
        goto done;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("request_submit, absent lun:%" G_GUINT32_FORMAT, lun);
        if (opcode == INQUIRY) {
            if (req->cdb[1] & 0x1) {
                cd_scsi_cmd_inquiry_vpd_no_lun(dev, req, PERIF_QUALIFIER_NOT_CONNECTED);
            } else {
                cd_scsi_cmd_inquiry_standard_no_lun(dev, req, PERIF_QUALIFIER_NOT_CONNECTED);
            }
        } else {
            cd_scsi_sense_check_cond(dev, req, &sense_code_LUN_NOT_SUPPORTED);
        }
        goto done;
    }

    if (dev->short_sense.key != NO_SENSE) {
        if (dev->short_sense.key == UNIT_ATTENTION) {
            if (!cd_scsi_opcode_ua_supress(opcode)) {
                /* return sense with UA */
                SPICE_DEBUG("request_submit, UA");
                cd_scsi_sense_check_cond(dev, req, &dev->short_sense);
                goto done;
            }
        } else if (opcode != REQUEST_SENSE) {
            SPICE_DEBUG("request_submit, lun:%" G_GUINT32_FORMAT 
                        " pending sense: 0x%02x %02x %02x", 
                        lun, (int)dev->short_sense.key, (int)dev->short_sense.asc,
                        (int)dev->short_sense.ascq);
            cd_scsi_pending_sense(dev, req);
            goto done;
        }
    }

    /* save the target to be used in callbacks where only req is passed */
    req->priv_data = (void *)st;

    req->req_len = 0;

    switch (opcode) {
    case REPORT_LUNS:
        cd_scsi_cmd_report_luns(st, dev, req);
        break;
    case TEST_UNIT_READY:
        cd_scsi_cmd_test_unit_ready(dev, req);
        break;
    case INQUIRY:
        cd_scsi_cmd_inquiry(dev, req);
        break;
    case REQUEST_SENSE:
        cd_scsi_cmd_request_sense(dev, req);
        break;
    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
        cd_scsi_cmd_read(dev, req);
        break;
    case READ_CAPACITY_10:
        cd_scsi_cmd_read_capacity(dev, req);
        break;
    case READ_TOC:
        cd_scsi_cmd_read_toc(dev, req);
        break;
    case GET_EVENT_STATUS_NOTIFICATION:
        cd_scsi_cmd_get_event_status_notification(dev, req);
        break;
    case READ_DISC_INFORMATION:
        cd_scsi_cmd_get_read_disc_information(dev, req);
        break;
    case READ_TRACK_INFORMATION:
        cd_scsi_cmd_get_read_track_information(dev, req);
        break;
    case MODE_SENSE_10:
        cd_scsi_cmd_mode_sense_10(dev, req);
        break;
    case MODE_SELECT:
        cd_scsi_cmd_mode_select_6(dev, req);
        break;
    case MODE_SELECT_10:
        cd_scsi_cmd_mode_select_10(dev, req);
        break;
    case GET_CONFIGURATION:
        cd_scsi_cmd_get_configuration(dev, req);
        break;
    case ALLOW_MEDIUM_REMOVAL:
        cd_scsi_cmd_allow_medium_removal(dev, req);
        break;
    case MMC_SEND_EVENT:
        cd_scsi_cmd_send_event(dev, req);
        break;
    case MMC_REPORT_KEY:
        cd_scsi_cmd_report_key(dev, req);
        break;
    case MMC_SEND_KEY:
        cd_scsi_cmd_send_key(dev, req);
        break;
    case START_STOP:
        cd_scsi_cmd_start_stop_unit(dev, req);
        break;
    case MMC_GET_PERFORMANCE:
        cd_scsi_cmd_get_performance(dev, req);
        break;
    case MECHANISM_STATUS:
        cd_scsi_cmd_mechanism_status(dev, req);
        break;
    default:
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_OPCODE);
        break;
    }

    if (req->req_len > INT32_MAX) {
        cd_scsi_sense_check_cond(dev, req, &sense_code_INVALID_FIELD);
        goto done;
    }

done:
    SPICE_DEBUG("request_submit done, lun: %" G_GUINT32_FORMAT 
                " op: 0x%02x state: %s status: %" G_GUINT32_FORMAT " len: %" G_GUINT64_FORMAT,
                lun, opcode, cd_scsi_req_state_str(req->req_state), req->status, req->in_len);

    if (req->req_state == SCSI_REQ_COMPLETE) {
        cd_scsi_dev_request_complete(st->user_data, req);
    }
}

void cd_scsi_dev_request_cancel(void *scsi_target, cd_scsi_request *req)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;

    if (st->cur_req == req) {
        if (req->req_state == SCSI_REQ_RUNNING) {
            SPICE_DEBUG("request_cancel: lun: %" G_GUINT32_FORMAT
                         " op: 0x%02x len: %" G_GUINT64_FORMAT,
                        req->lun, (unsigned int)req->cdb[0], req->req_len);
            g_cancellable_cancel(st->cancellable);
        } else {
            SPICE_DEBUG("request_cancel: request is not running");
        }
    } else {
        SPICE_DEBUG("request_cancel: other request is outstanding");
    }
}

void cd_scsi_dev_request_release(void *scsi_target, cd_scsi_request *req)
{
    cd_scsi_target *st = (cd_scsi_target *)scsi_target;

    st->cur_req = NULL;
    cd_scsi_req_init(req);

    if (st->state == CD_SCSI_TGT_STATE_RESET) {
        cd_scsi_target_do_reset(st);
    }
}