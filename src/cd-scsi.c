/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   CD device emulation - SCSI engine

   Copyright (C) 2018 Red Hat, Inc.

   Red Hat Authors:
   Alexander Nezhinsky<anezhins@redhat.com>

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

#ifdef USE_USBREDIR

#define SPICE_ERROR(fmt, ...) \
    do { SPICE_DEBUG("dev-scsi error: " fmt , ## __VA_ARGS__); } while (0)

#define FIXED_SENSE_CURRENT 0x70
#define FIXED_SENSE_LEN 18

#define MAX_LUNS   32

typedef enum CdScsiPowerCondition {
    CD_SCSI_POWER_STOPPED,
    CD_SCSI_POWER_ACTIVE,
    CD_SCSI_POWER_IDLE,
    CD_SCSI_POWER_STANDBY
} CdScsiPowerCondition;

typedef struct ScsiShortSense {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
    const char *descr;
} ScsiShortSense;

#define CD_MEDIA_EVENT_NO_CHANGE            0x0
#define CD_MEDIA_EVENT_EJECT_REQ            0x1 /* user request (mechanical switch) to
                                                 * eject the media */
#define CD_MEDIA_EVENT_NEW_MEDIA            0x2 /* new media received */
#define CD_MEDIA_EVENT_MEDIA_REMOVAL        0x3 /* media removed */
#define CD_MEDIA_EVENT_MEDIA_CHANGED        0x4 /* user request to load new media */
#define CD_MEDIA_EVENT_BG_FORMAT_COMPLETE   0x5
#define CD_MEDIA_EVENT_BG_FORMAT_RESTART    0x6

#define CD_POWER_EVENT_NO_CHANGE            0x0
#define CD_POWER_EVENT_CHANGE_SUCCESS       0x1
#define CD_POWER_EVENT_CHANGE_FALED         0x2

typedef struct CdScsiLU {
    CdScsiTarget *tgt;
    uint32_t lun;

    gboolean realized;
    gboolean removable;
    gboolean loaded;
    gboolean prevent_media_removal;
    gboolean cd_rom;

    CdScsiPowerCondition power_cond;
    uint32_t power_event;
    uint32_t media_event;

    uint32_t claim_version;

    uint64_t size;
    uint32_t block_size;
    uint32_t num_blocks;

    char *vendor;
    char *product;
    char *version;
    char *serial;

    GFileInputStream *stream;

    ScsiShortSense short_sense; /* currently held sense of the scsi device */
    uint8_t fixed_sense[FIXED_SENSE_LEN];
} CdScsiLU;

typedef enum CdScsiTargetState {
    CD_SCSI_TGT_STATE_RUNNING,
    CD_SCSI_TGT_STATE_RESET,
} CdScsiTargetState;

struct CdScsiTarget {
    void *user_data;

    CdScsiTargetState state;
    CdScsiRequest *cur_req;
    GCancellable *cancellable;

    uint32_t max_luns;
    CdScsiLU units[MAX_LUNS];
};

static const char* scsi_cmd_name[256];

/* Predefined sense codes */
#define SENSE_CODE(name) \
    static const ScsiShortSense name G_GNUC_UNUSED

SENSE_CODE(sense_code_NO_SENSE) = {
    .key = NO_SENSE , .asc = 0x00 , .ascq = 0x00,
    .descr = ""
};

SENSE_CODE(sense_code_NOT_READY_CAUSE_NOT_REPORTABLE) = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x00,
    .descr = "CAUSE NOT REPORTABLE"
};

SENSE_CODE(sense_code_BECOMING_READY) = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x01,
    .descr = "IN PROCESS OF BECOMING READY"
};

SENSE_CODE(sense_code_INIT_CMD_REQUIRED) = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x02,
    .descr = "INITIALIZING COMMAND REQUIRED"
};

SENSE_CODE(sense_code_INTERVENTION_REQUIRED) = {
    .key = NOT_READY, .asc = 0x04, .ascq = 0x03,
    .descr = "MANUAL INTERVENTION REQUIRED"
};

SENSE_CODE(sense_code_NOT_READY_NO_MEDIUM) = {
    .key = NOT_READY, .asc = 0x3a, .ascq = 0x00,
    .descr = "MEDIUM NOT PRESENT"
};

SENSE_CODE(sense_code_NOT_READY_NO_MEDIUM_TRAY_CLOSED) = {
    .key = NOT_READY, .asc = 0x3a, .ascq = 0x01,
    .descr = "MEDIUM NOT PRESENT - TRAY CLOSED"
};

SENSE_CODE(sense_code_NOT_READY_NO_MEDIUM_TRAY_OPEN) = {
    .key = NOT_READY, .asc = 0x3a, .ascq = 0x02,
    .descr = "MEDIUM NOT PRESENT - TRAY OPEN"
};

SENSE_CODE(sense_code_TARGET_FAILURE) = {
    .key = HARDWARE_ERROR, .asc = 0x44, .ascq = 0x00,
    .descr = "INTERNAL TARGET FAILURE"
};

SENSE_CODE(sense_code_INVALID_OPCODE) = {
    .key = ILLEGAL_REQUEST, .asc = 0x20, .ascq = 0x00,
    .descr = "INVALID COMMAND OPERATION CODE"
};

SENSE_CODE(sense_code_LBA_OUT_OF_RANGE) = {
    .key = ILLEGAL_REQUEST, .asc = 0x21, .ascq = 0x00,
    .descr = "LOGICAL BLOCK ADDRESS OUT OF RANGE"
};

SENSE_CODE(sense_code_INVALID_CDB_FIELD) = {
    .key = ILLEGAL_REQUEST, .asc = 0x24, .ascq = 0x00,
    .descr = "INVALID FIELD IN CDB"
};

SENSE_CODE(sense_code_INVALID_PARAM_FIELD) = {
    .key = ILLEGAL_REQUEST, .asc = 0x26, .ascq = 0x00,
    .descr = "INVALID FIELD IN PARAMETER LIST"
};

SENSE_CODE(sense_code_INVALID_PARAM_LEN) = {
    .key = ILLEGAL_REQUEST, .asc = 0x1a, .ascq = 0x00,
    .descr = "PARAMETER LIST LENGTH ERROR"
};

SENSE_CODE(sense_code_LUN_NOT_SUPPORTED) = {
    .key = ILLEGAL_REQUEST, .asc = 0x25, .ascq = 0x00,
    .descr = "LOGICAL UNIT NOT SUPPORTED"
};

SENSE_CODE(sense_code_SAVING_PARAMS_NOT_SUPPORTED) = {
    .key = ILLEGAL_REQUEST, .asc = 0x39, .ascq = 0x00,
    .descr = "SAVING PARAMETERS NOT SUPPORTED"
};

SENSE_CODE(sense_code_INCOMPATIBLE_MEDIUM) = {
    .key = ILLEGAL_REQUEST, .asc = 0x30, .ascq = 0x00,
    .descr = "INCOMPATIBLE MEDIUM INSTALLED"
};

SENSE_CODE(sense_code_MEDIUM_REMOVAL_PREVENTED) = {
    .key = ILLEGAL_REQUEST, .asc = 0x53, .ascq = 0x02,
    .descr = "MEDIUM REMOVAL PREVENTED"
};

SENSE_CODE(sense_code_PARAMETERS_CHANGED) = {
    .key = UNIT_ATTENTION, .asc = 0x2a, .ascq = 0x00,
    .descr = "PARAMETERS CHANGED"
};

SENSE_CODE(sense_code_POWER_ON_RESET) = {
    .key = UNIT_ATTENTION, .asc = 0x29, .ascq = 0x00,
    .descr = "POWER ON, RESET, OR BUS DEVICE RESET"
};

SENSE_CODE(sense_code_SCSI_BUS_RESET) = {
    .key = UNIT_ATTENTION, .asc = 0x29, .ascq = 0x02,
    .descr = "SCSI BUS RESET"
};

SENSE_CODE(sense_code_UA_NO_MEDIUM) = {
    .key = UNIT_ATTENTION, .asc = 0x3a, .ascq = 0x00,
    .descr = "MEDIUM NOT PRESENT"
};

SENSE_CODE(sense_code_MEDIUM_CHANGED) = {
    .key = UNIT_ATTENTION, .asc = 0x28, .ascq = 0x00,
    .descr = "MEDIUM CHANGED"
};

SENSE_CODE(sense_code_REPORTED_LUNS_CHANGED) = {
    .key = UNIT_ATTENTION, .asc = 0x3f, .ascq = 0x0e,
    .descr = "REPORTED LUNS CHANGED"
};

SENSE_CODE(sense_code_DEVICE_INTERNAL_RESET) = {
    .key = UNIT_ATTENTION, .asc = 0x29, .ascq = 0x04,
    .descr = "DEVICE INTERNAL RESET"
};

SENSE_CODE(sense_code_UNIT_ATTENTION_MEDIUM_REMOVAL_REQUEST) = {
    .key = UNIT_ATTENTION, .asc = 0x5a, .ascq = 0x01,
    .descr = "OPERATOR MEDIUM REMOVAL REQUEST"
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

static inline const char *CdScsiReqState_str(CdScsiReqState state)
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

static const char *cd_scsi_sense_key_descr(uint8_t sense_key)
{
    switch(sense_key) {
    case NO_SENSE:
        return "NO SENSE";
    case RECOVERED_ERROR:
        return "RECOVERED ERROR";
    case NOT_READY:
        return "LUN NOT READY";
    case MEDIUM_ERROR:
        return "MEDIUM ERROR";
    case HARDWARE_ERROR:
        return "HARDWARE ERROR";
    case ILLEGAL_REQUEST:
        return "ILLEGAL REQUEST";
    case UNIT_ATTENTION:
        return "UNIT ATTENTION";
    case BLANK_CHECK:
        return "BLANK CHECK";
    case ABORTED_COMMAND:
        return "ABORTED COMMAND";
    default:
        return "???";
    }
}
static uint32_t cd_scsi_build_fixed_sense(uint8_t *buf, const ScsiShortSense *short_sense)
{
    memset(buf, 0, FIXED_SENSE_LEN);

    buf[0] = FIXED_SENSE_CURRENT;
    buf[2] = short_sense->key;
    buf[7] = 10;
    buf[12] = short_sense->asc;
    buf[13] = short_sense->ascq;

    return FIXED_SENSE_LEN;
}

static inline void cd_scsi_req_init(CdScsiRequest *req)
{
    req->req_state = SCSI_REQ_IDLE;
    req->xfer_dir = SCSI_XFER_NONE;
    req->priv_data = NULL;
    req->in_len = 0;
    req->status = GOOD;
}

static inline void cd_scsi_dev_sense_reset(CdScsiLU *dev)
{
    memset(&dev->short_sense, 0, sizeof(dev->short_sense));
    cd_scsi_build_fixed_sense(dev->fixed_sense, &dev->short_sense);
}

static inline void cd_scsi_dev_sense_set(CdScsiLU *dev, const ScsiShortSense *short_sense)
{
    if (short_sense != NULL) {
        /* copy short sense and generate full sense in fixed format */
        dev->short_sense = *short_sense;
        cd_scsi_build_fixed_sense(dev->fixed_sense, short_sense);
    }
}

static inline void cd_scsi_dev_sense_set_power_on(CdScsiLU *dev)
{
    cd_scsi_dev_sense_set(dev, &sense_code_POWER_ON_RESET);
}

static void cd_scsi_cmd_complete_check_cond(CdScsiLU *dev, CdScsiRequest *req,
                                            const ScsiShortSense *short_sense)
{
    req->req_state = SCSI_REQ_COMPLETE;
    req->status = CHECK_CONDITION;
    req->in_len = 0;

    cd_scsi_dev_sense_set(dev, short_sense);

    SPICE_DEBUG("CHECK_COND, request lun:%u"
                " op: 0x%02x, pending sense: 0x%02x %02x %02x - %s, %s",
                dev->lun, (uint32_t)req->cdb[0],
                (uint32_t)dev->short_sense.key,
                (uint32_t)dev->short_sense.asc,
                (uint32_t)dev->short_sense.ascq,
                cd_scsi_sense_key_descr(dev->short_sense.key),
                dev->short_sense.descr);
}

static void cd_scsi_cmd_complete_good(CdScsiLU *dev, CdScsiRequest *req)
{
    req->req_state = SCSI_REQ_COMPLETE;
    req->status = GOOD;
}

/* SCSI Target */

SPICE_CONSTRUCTOR_FUNC(cd_scsi_cmd_names_init)
{
    uint32_t opcode;

    for (opcode = 0; opcode < 256; opcode++) {
        scsi_cmd_name[opcode] = "UNSUPPORTED";
    }

    scsi_cmd_name[REPORT_LUNS] = "REPORT LUNS";
    scsi_cmd_name[TEST_UNIT_READY] = "TEST UNIT READY";
    scsi_cmd_name[INQUIRY] = "INQUIRY";
    scsi_cmd_name[REQUEST_SENSE] = "REQUEST SENSE";
    scsi_cmd_name[READ_6] = "READ(6)";
    scsi_cmd_name[READ_10] = "READ(10)";
    scsi_cmd_name[READ_12] = "READ(12)";
    scsi_cmd_name[READ_16] = "READ(16)";
    scsi_cmd_name[READ_CAPACITY_10] = "READ CAPACITY(10)";
    scsi_cmd_name[READ_TOC] = "READ TOC";
    scsi_cmd_name[GET_EVENT_STATUS_NOTIFICATION] = "GET EVENT/STATUS NOTIFICATION";
    scsi_cmd_name[READ_DISC_INFORMATION] = "READ DISC INFO";
    scsi_cmd_name[READ_TRACK_INFORMATION] = "READ TRACK INFO";
    scsi_cmd_name[MODE_SENSE_10] = "MODE SENSE(10)";
    scsi_cmd_name[MODE_SELECT] = "MODE SELECT(6)";
    scsi_cmd_name[MODE_SELECT_10] = "MODE SELECT(10)";
    scsi_cmd_name[GET_CONFIGURATION] = "GET CONFIGURATION";
    scsi_cmd_name[ALLOW_MEDIUM_REMOVAL] = "PREVENT ALLOW MEDIUM REMOVAL";
    scsi_cmd_name[MMC_SEND_EVENT] = "SEND EVENT";
    scsi_cmd_name[MMC_REPORT_KEY] = "REPORT KEY";
    scsi_cmd_name[MMC_SEND_KEY] = "SEND_KEY";
    scsi_cmd_name[START_STOP] = "START STOP UNIT";
    scsi_cmd_name[MMC_GET_PERFORMANCE] = "GET PERFORMANCE";
    scsi_cmd_name[MMC_MECHANISM_STATUS] = "MECHANISM STATUS";
}

CdScsiTarget *cd_scsi_target_alloc(void *target_user_data, uint32_t max_luns)
{
    CdScsiTarget *st;

    if (max_luns == 0 || max_luns > MAX_LUNS) {
        SPICE_ERROR("Alloc, illegal max_luns:%u", max_luns);
        return NULL;
    }

    st = g_malloc0(sizeof(*st));

    st->user_data = target_user_data;
    st->state = CD_SCSI_TGT_STATE_RUNNING;
    st->cur_req = NULL;
    st->cancellable = g_cancellable_new();
    st->max_luns = max_luns;

    return st;
}

void cd_scsi_target_free(CdScsiTarget *st)
{
    uint32_t lun;

    cd_scsi_target_reset(st);
    for (lun = 0; lun < st->max_luns; lun++) {
        CdScsiLU *unit = &st->units[lun];
        if (unit->realized) {
            cd_scsi_dev_unrealize(st, lun);
        }
        g_clear_object(&unit->stream);
    }
    g_clear_object(&st->cancellable);
    g_free(st);
}

/* SCSI Device */

static inline gboolean cd_scsi_target_lun_legal(const CdScsiTarget *st, uint32_t lun)
{
    return (lun < st->max_luns) ? TRUE : FALSE;
}

static inline gboolean cd_scsi_target_lun_realized(const CdScsiTarget *st, uint32_t lun)
{
    return st->units[lun].realized;
}

int cd_scsi_dev_realize(CdScsiTarget *st, uint32_t lun,
                        const CdScsiDeviceParameters *dev_params)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Realize, illegal lun:%u", lun);
        return -1;
    }
    if (cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Realize, already realized lun:%u", lun);
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
    dev->power_event = CD_POWER_EVENT_NO_CHANGE;
    dev->media_event = CD_MEDIA_EVENT_NO_CHANGE;

    dev->claim_version = 3; /* 0 : none; 2,3,5 : SPC/MMC-x */

    dev->vendor = g_strdup(dev_params->vendor);
    dev->product = g_strdup(dev_params->product);
    dev->version = g_strdup(dev_params->version);
    dev->serial = g_strdup(dev_params->serial);

    cd_scsi_dev_sense_set_power_on(dev);

    SPICE_DEBUG("Realize lun:%u bs:%u VR:[%s] PT:[%s] ver:[%s] SN[%s]",
                lun, dev->block_size, dev->vendor,
                dev->product, dev->version, dev->serial);
    return 0;
}

static void cd_scsi_lu_media_reset(CdScsiLU *dev)
{
    /* media_event is not set here, as it depends on the context */
    g_clear_object(&dev->stream);
    dev->size = 0;
    dev->block_size = 0;
    dev->num_blocks = 0;
}

int cd_scsi_dev_lock(CdScsiTarget *st, uint32_t lun, gboolean lock)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Lock, illegal lun:%u", lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Lock, unrealized lun:%u", lun);
        return -1;
    }
    dev = &st->units[lun];
    dev->prevent_media_removal = lock;
    SPICE_DEBUG("lun:%u %slock", lun, lock ? "un" :"");
    return 0;
}

static void cd_scsi_lu_load(CdScsiLU *dev,
                            const CdScsiMediaParameters *media_params)
{
    if (media_params != NULL) {
        dev->media_event = CD_MEDIA_EVENT_NEW_MEDIA;
        dev->stream = g_object_ref(media_params->stream);
        dev->size = media_params->size;
        dev->block_size = media_params->block_size;
        dev->num_blocks = media_params->size / media_params->block_size;
        dev->loaded = TRUE;
    } else {
        dev->media_event = CD_MEDIA_EVENT_MEDIA_REMOVAL;
        cd_scsi_lu_media_reset(dev);
        dev->loaded = FALSE;
    }
}

static void cd_scsi_lu_unload(CdScsiLU *dev)
{
    dev->media_event = CD_MEDIA_EVENT_MEDIA_REMOVAL;
    cd_scsi_lu_media_reset(dev);
    dev->loaded = FALSE;
}

int cd_scsi_dev_load(CdScsiTarget *st, uint32_t lun,
                     const CdScsiMediaParameters *media_params)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Load, illegal lun:%u", lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Load, unrealized lun:%u", lun);
        return -1;
    }
    dev = &st->units[lun];

    cd_scsi_lu_load(dev, media_params);
    dev->power_cond = CD_SCSI_POWER_ACTIVE;
    dev->power_event = CD_POWER_EVENT_CHANGE_SUCCESS;

    cd_scsi_dev_sense_set(dev, &sense_code_MEDIUM_CHANGED);

    SPICE_DEBUG("Load lun:%u size:%" G_GUINT64_FORMAT
                " blk_sz:%u num_blocks:%u",
                lun, dev->size, dev->block_size, dev->num_blocks);
    return 0;
}

int cd_scsi_dev_get_info(CdScsiTarget *st, uint32_t lun, CdScsiDeviceInfo *lun_info)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Load, illegal lun:%u", lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Load, unrealized lun:%u", lun);
        return -1;
    }
    dev = &st->units[lun];

    lun_info->started = dev->power_cond == CD_SCSI_POWER_ACTIVE;
    lun_info->locked = dev->prevent_media_removal;
    lun_info->loaded = dev->loaded;

    lun_info->parameters.vendor = dev->vendor;
    lun_info->parameters.product = dev->product;
    lun_info->parameters.version = dev->version;
    lun_info->parameters.serial = dev->serial;

    return 0;
}

int cd_scsi_dev_unload(CdScsiTarget *st, uint32_t lun)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Unload, illegal lun:%u", lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Unload, unrealized lun:%u", lun);
        return -1;
    }
    dev = &st->units[lun];
    if (!dev->loaded) {
        SPICE_ERROR("Unload, lun:%u not loaded yet", lun);
        return 0;
    }
    if (dev->prevent_media_removal) {
        SPICE_ERROR("Unload, lun:%u prevent_media_removal set", lun);
        return -1;
    }

    cd_scsi_lu_unload(dev);
    dev->power_cond = CD_SCSI_POWER_STOPPED;
    dev->power_event = CD_POWER_EVENT_CHANGE_SUCCESS;

    cd_scsi_dev_sense_set(dev, &sense_code_UA_NO_MEDIUM);

    SPICE_DEBUG("Unload lun:%u", lun);
    return 0;
}

int cd_scsi_dev_unrealize(CdScsiTarget *st, uint32_t lun)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Unrealize, illegal lun:%u", lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Unrealize, absent lun:%u", lun);
        return -1;
    }
    dev = &st->units[lun];

    g_clear_pointer(&dev->vendor, g_free);
    g_clear_pointer(&dev->product, g_free);
    g_clear_pointer(&dev->version, g_free);
    g_clear_pointer(&dev->serial, g_free);

    g_clear_object(&dev->stream);

    dev->loaded = FALSE;
    dev->realized = FALSE;
    dev->power_cond = CD_SCSI_POWER_STOPPED;

    SPICE_DEBUG("Unrealize lun:%u", lun);
    return 0;
}

int cd_scsi_dev_reset(CdScsiTarget *st, uint32_t lun)
{
    CdScsiLU *dev;

    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("Device reset, illegal lun:%u", lun);
        return -1;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("Device reset, absent lun:%u", lun);
        return -1;
    }
    dev = &st->units[lun];

    /* if we reset the 'prevent' flag we can't create
     * the unit that is locked from the beginning, so
     * we keep this flag as persistent over resets
     */
    /* dev->prevent_media_removal = FALSE; */
    dev->power_cond = CD_SCSI_POWER_ACTIVE;
    dev->power_event = CD_POWER_EVENT_CHANGE_SUCCESS;
    cd_scsi_dev_sense_set_power_on(dev);

    SPICE_DEBUG("Device reset lun:%u", lun);
    return 0;
}

static void cd_scsi_target_do_reset(CdScsiTarget *st)
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

int cd_scsi_target_reset(CdScsiTarget *st)
{
    if (st->state == CD_SCSI_TGT_STATE_RESET) {
        SPICE_DEBUG("Target already in reset");
        return -1;
    }

    st->state = CD_SCSI_TGT_STATE_RESET;

    if (st->cur_req != NULL) {
        cd_scsi_dev_request_cancel(st, st->cur_req);
        if (st->cur_req != NULL) {
            SPICE_DEBUG("Target reset in progress...");
            return 0;
        }
    }

    cd_scsi_target_do_reset(st);
    return 0;
}

CdScsiReqState cd_scsi_get_req_state(CdScsiRequest *req)
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

static unsigned int scsi_cdb_length(const uint8_t *cdb)
{
    unsigned int cdb_len;

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
        cdb_len = 0;
    }
    return cdb_len;
}

static uint64_t scsi_cdb_lba(const uint8_t *cdb, int cdb_len)
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

static uint32_t scsi_cdb_xfer_length(const uint8_t *cdb, int cdb_len)
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

static void cd_scsi_cmd_test_unit_ready(CdScsiLU *dev, CdScsiRequest *req)
{
    req->xfer_dir = SCSI_XFER_NONE;
    req->in_len = 0;

    if (dev->loaded) {
        if (dev->power_cond != CD_SCSI_POWER_STOPPED) {
            cd_scsi_cmd_complete_good(dev, req);
        } else {
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INIT_CMD_REQUIRED);
        }
    } else {
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_NOT_READY_NO_MEDIUM);
    }
}

static void cd_scsi_cmd_request_sense(CdScsiLU *dev, CdScsiRequest *req)
{
    req->xfer_dir = SCSI_XFER_FROM_DEV;

    req->req_len = req->cdb[4];
    req->in_len = MIN(req->req_len, sizeof(dev->fixed_sense));

    if (dev->short_sense.key != NO_SENSE) {
        SPICE_DEBUG("%s, lun:%u reported sense: 0x%02x %02x %02x - %s, %s",
                    __FUNCTION__, req->lun,
                    dev->short_sense.key, dev->short_sense.asc, dev->short_sense.ascq,
                    cd_scsi_sense_key_descr(dev->short_sense.key),
                    dev->short_sense.descr);
    }
    memcpy(req->buf, dev->fixed_sense, sizeof(dev->fixed_sense));
    cd_scsi_dev_sense_reset(dev); /* clear reported sense */

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_report_luns(CdScsiTarget *st, CdScsiLU *dev,
                                    CdScsiRequest *req)
{
    uint8_t *out_buf = req->buf;
    uint32_t max_luns = st->max_luns;
    uint32_t buflen = 8; /* header length */
    uint32_t lun;

    req->req_len = scsi_cdb_xfer_length(req->cdb, 12);
    req->xfer_dir = SCSI_XFER_FROM_DEV;

    /* check SELECT REPORT field */
    if (req->cdb[2] == 0x01) {
        /* only well known logical units */
        max_luns = 0;
    }

    memset(out_buf, 0, 8);

    for (lun = 0; lun < max_luns; lun++) {
        if (st->units[lun].realized) {
            out_buf[buflen++] = 0;
            out_buf[buflen++] = (uint8_t)(lun);
            memset(&out_buf[buflen], 0, 6);
            buflen += 6;
        }
    }

    /* fill LUN LIST LENGTH */
    out_buf[0] = (uint8_t)((buflen-8) >> 24);
    out_buf[1] = (uint8_t)((buflen-8) >> 16);
    out_buf[2] = (uint8_t)((buflen-8) >> 8);
    out_buf[3] = (uint8_t)((buflen-8));

    req->in_len = buflen;
    cd_scsi_cmd_complete_good(dev, req);
}

#define SCSI_MAX_INQUIRY_LEN        256
#define SCSI_MAX_MODE_LEN           256

static void cd_scsi_cmd_inquiry_vpd_no_lun(CdScsiLU *dev, CdScsiRequest *req,
                                           uint32_t perif_qual)
{
    uint8_t *outbuf = req->buf;
    uint8_t page_code = req->cdb[2];
    uint32_t resp_len = 4;

    outbuf[0] = (perif_qual << 5) | TYPE_ROM;
    outbuf[1] = page_code ; /* this page */
    outbuf[2] = 0x00; /* page length MSB */
    outbuf[3] = 0x00; /* page length LSB - no more data */

    req->in_len = MIN(req->req_len, resp_len);

    SPICE_DEBUG("inquiry_vpd, unsupported lun:%u"
                " perif_qual:0x%x resp_len: %" G_GUINT64_FORMAT,
                req->lun, perif_qual, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_inquiry_vpd(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    uint8_t page_code = req->cdb[2];
    int buflen = 4;
    int start = 4;

    outbuf[0] = TYPE_ROM;
    outbuf[1] = page_code ; /* this page */
    outbuf[2] = 0x00; /* page length MSB */
    outbuf[3] = 0x00; /* page length LSB, to write later */

    switch (page_code) {
    case 0x00: /* Supported page codes, mandatory */
    {
        outbuf[buflen++] = 0x00; // list of supported pages (this page)
        if (dev->serial) {
            outbuf[buflen++] = 0x80; // unit serial number
        }
        outbuf[buflen++] = 0x83; // device identification

        SPICE_DEBUG("Inquiry EVPD[Supported pages] lun:%u"
                    " req_len: %" G_GUINT64_FORMAT " resp_len: %d",
                    req->lun, req->req_len, buflen);
        break;
    }
    case 0x80: /* Device serial number, optional */
    {
        int serial_len;

        serial_len = strlen(dev->serial);
        if (serial_len > 36) {
            serial_len = 36;
        }
        memcpy(outbuf+buflen, dev->serial, serial_len);
        buflen += serial_len;

        SPICE_DEBUG("Inquiry EVPD[Serial num] lun:%u"
                    " req_len: %" G_GUINT64_FORMAT " resp_len: %d",
                    req->lun, req->req_len, buflen);
        break;
    }
    case 0x83: /* Device identification page, mandatory */
    {
        int serial_len = strlen(dev->serial);
        int max_len = 20;

        if (serial_len > max_len) {
            serial_len = max_len;
        }

        outbuf[buflen++] = 0x2; // ASCII
        outbuf[buflen++] = 0;   // not officially assigned
        outbuf[buflen++] = 0;   // reserved
        outbuf[buflen++] = serial_len; // length of data following

        memcpy(outbuf+buflen, dev->serial, serial_len);
        buflen += serial_len;

        SPICE_DEBUG("Inquiry EVPD[Device id] lun:%u"
                    " req_len: %" G_GUINT64_FORMAT " resp_len: %d",
                    req->lun, req->req_len, buflen);
        break;
    }

    default:
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        SPICE_DEBUG("inquiry_standard, lun:%u invalid page_code: %02x",
                    req->lun, (int)page_code);
        return;
    }

    /* done with EVPD */
    g_assert(buflen - start <= 255);
    outbuf[3] = buflen - start; /* page length LSB */

    req->in_len = buflen;
    cd_scsi_cmd_complete_good(dev, req);
}

#define INQUIRY_STANDARD_LEN_MIN            36
#define INQUIRY_STANDARD_LEN                96
#define INQUIRY_STANDARD_LEN_NO_VER         57

#define PERIF_QUALIFIER_CONNECTED           0x00
#define PERIF_QUALIFIER_NOT_CONNECTED       0x01
#define PERIF_QUALIFIER_UNSUPPORTED         0x03

#define INQUIRY_REMOVABLE_MEDIUM            0x80

#define INQUIRY_VERSION_NONE                0x00
#define INQUIRY_VERSION_SPC3                0x05

/* byte 3 */
#define INQUIRY_RESP_HISUP                  (0x01 << 4)
#define INQUIRY_RESP_NORM_ACA               (0x01 << 5)
#define INQUIRY_RESP_DATA_FORMAT_SPC3       0x02

#define INQUIRY_VERSION_DESC_SAM2           0x040
#define INQUIRY_VERSION_DESC_SPC3           0x300
#define INQUIRY_VERSION_DESC_MMC3           0x2A0
#define INQUIRY_VERSION_DESC_SBC2           0x320

static void cd_scsi_cmd_inquiry_standard_no_lun(CdScsiLU *dev, CdScsiRequest *req,
                                                uint32_t perif_qual)
{
    uint8_t *outbuf = req->buf;
    uint32_t resp_len = INQUIRY_STANDARD_LEN_MIN;

    memset(req->buf, 0, INQUIRY_STANDARD_LEN_MIN);

    outbuf[0] = (perif_qual << 5) | TYPE_ROM;
    outbuf[2] = INQUIRY_VERSION_NONE;
    outbuf[3] = INQUIRY_RESP_DATA_FORMAT_SPC3;

    outbuf[4] = resp_len - 4; /* additional length, after header */

    req->in_len = MIN(req->req_len, resp_len);

    SPICE_DEBUG("inquiry_standard, unsupported lun:%u perif_qual:0x%x "
                "inquiry_len: %u resp_len: %" G_GUINT64_FORMAT,
                req->lun, perif_qual, resp_len, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_inquiry_standard(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t resp_len =
        (dev->claim_version == 0) ? INQUIRY_STANDARD_LEN_NO_VER : INQUIRY_STANDARD_LEN;

    outbuf[0] = (PERIF_QUALIFIER_CONNECTED << 5) | TYPE_ROM;
    outbuf[1] = (dev->removable) ? INQUIRY_REMOVABLE_MEDIUM : 0;
    outbuf[2] = (dev->claim_version == 0) ? INQUIRY_VERSION_NONE : INQUIRY_VERSION_SPC3;
    outbuf[3] = INQUIRY_RESP_NORM_ACA | INQUIRY_RESP_HISUP | INQUIRY_RESP_DATA_FORMAT_SPC3;

    outbuf[4] = resp_len - 4; /* additional length, after header */

    /* (outbuf[6,7] = 0) means also {BQue=0,CmdQue=0} - no queueing at all */

    strpadcpy((char *) &outbuf[8], 8, dev->vendor, ' ');
    strpadcpy((char *) &outbuf[16], 16, dev->product, ' ');
    memcpy(&outbuf[32], dev->version, MIN(4, strlen(dev->version)));

    if (dev->claim_version > 0) {
        /* now supporting only 3 */
        outbuf[58] = (INQUIRY_VERSION_DESC_SAM2 >> 8) & 0xff;
        outbuf[59] = INQUIRY_VERSION_DESC_SAM2 & 0xff;

        outbuf[60] = (INQUIRY_VERSION_DESC_SPC3 >> 8) & 0xff;
        outbuf[61] = INQUIRY_VERSION_DESC_SPC3 & 0xff;

        outbuf[62] = (INQUIRY_VERSION_DESC_MMC3 >> 8) & 0xff;
        outbuf[63] = INQUIRY_VERSION_DESC_MMC3 & 0xff;

        outbuf[64] = (INQUIRY_VERSION_DESC_SBC2 >> 8) & 0xff;
        outbuf[65] = INQUIRY_VERSION_DESC_SBC2 & 0xff;
    }

    req->in_len = MIN(req->req_len, resp_len);

    SPICE_DEBUG("inquiry_standard, lun:%u"
                " inquiry_len: %u resp_len: %" G_GUINT64_FORMAT,
                req->lun, resp_len, req->in_len);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_INQUIRY_FLAG_EVPD                0x01
#define CD_INQUIRY_FLAG_CMD_DT              0x02

static void cd_scsi_cmd_inquiry(CdScsiLU *dev, CdScsiRequest *req)
{
    gboolean evpd, cmd_data;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    evpd = (req->cdb[1] & CD_INQUIRY_FLAG_EVPD) ? TRUE : FALSE;
    cmd_data = (req->cdb[1] & CD_INQUIRY_FLAG_CMD_DT) ? TRUE : FALSE;

    if (cmd_data) {
        SPICE_DEBUG("inquiry, lun:%u CmdDT bit set - unsupported, "
                    "cdb[1]:0x%02x cdb[1]:0x%02x",
                    req->lun, (int)req->cdb[1], (int)req->cdb[2]);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }

    req->req_len = req->cdb[4] | (req->cdb[3] << 8);
    memset(req->buf, 0, req->req_len);

    if (evpd) { /* enable vital product data */
        cd_scsi_cmd_inquiry_vpd(dev, req);
    } else { /* standard inquiry data */
        if (req->cdb[2] != 0) {
            SPICE_DEBUG("inquiry_standard, lun:%u non-zero page code: %02x",
                        req->lun, (int)req->cdb[2]);
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
            return;
        }
        cd_scsi_cmd_inquiry_standard(dev, req);
    }
}

static void cd_scsi_cmd_read_capacity(CdScsiLU *dev, CdScsiRequest *req)
{
    uint32_t last_blk = dev->num_blocks - 1;
    uint32_t blk_size = dev->block_size;
    uint32_t *last_blk_out = (uint32_t *)req->buf;
    uint32_t *blk_size_out = (uint32_t *)(req->buf + 4);

    req->xfer_dir = SCSI_XFER_FROM_DEV;
    req->req_len = 8;

    *last_blk_out = htobe32(last_blk);
    *blk_size_out = htobe32(blk_size);

    SPICE_DEBUG("Read capacity, lun:%u last_blk: %u blk_sz: %u",
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

#define RDI_DISC_PMA_TYPE_CD_ROM    0x00
#define RDI_DISC_PMA_TYPE_CDI       0x10
#define RDI_DISC_PMA_TYPE_DDCD      0x20
#define RDI_DISC_PMA_TYPE_UNDEFINED 0xFF

static void cd_scsi_cmd_get_read_disc_information(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t data_type;
    uint32_t first_track = 1;
    uint32_t last_track = 1;
    uint32_t num_sessions = 1;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    data_type = req->cdb[1] & 0x7;
    if (data_type != RDI_TYPE_STANDARD) {
        SPICE_DEBUG("read_disc_information, lun:%u"
                    " unsupported data type: %02x",
                    req->lun, data_type);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];
    req->in_len = MIN(req->req_len, RDI_STANDARD_LEN);

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

    SPICE_DEBUG("read_disc_information, lun:%u len: %" G_GUINT64_FORMAT,
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

static void cd_scsi_cmd_get_read_track_information(CdScsiLU *dev, CdScsiRequest *req)
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
            SPICE_DEBUG("read_track_information, lun:%u"
                        " addr_type LBA: %u"
                        " invalid LBA: %u",
                        req->lun, addr_type, addr_num);
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
            return;
        }
        break;
    case RTI_ADDR_TYPE_TRACK_NUM:
        if (addr_num != track_num) {
            SPICE_DEBUG("read_track_information, lun:%u"
                        " addr_type track: %u"
                        " invalid track: %u",
                        req->lun, addr_type, addr_num);
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
            return;
        }
        break;
    case RTI_ADDR_TYPE_SESSION_NUM:
        if (addr_num != session_num) {
            SPICE_DEBUG("read_track_information, lun:%u"
                        " addr_type session: %u"
                        " invalid session: %u",
                        req->lun, addr_type, addr_num);
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
            return;
        }
        break;
    default:
        SPICE_DEBUG("read_track_information, lun:%u"
                    " invalid addr_type: %u"
                    " addr_num: %u",
                    req->lun, addr_type, addr_num);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];
    req->in_len = MIN(req->req_len, TIB_LEN);

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

    SPICE_DEBUG("read_track_information, lun:%u"
                "addr_type: %u addr_num: %u",
                req->lun, addr_type, addr_num);

    cd_scsi_cmd_complete_good(dev, req);
}

#define READ_TOC_TRACK_DESC_LEN     8
#define READ_TOC_RESP_LEN           (4 + 2*READ_TOC_TRACK_DESC_LEN)

static void cd_scsi_cmd_read_toc(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t msf, format, track_num;
    uint32_t last_blk = dev->num_blocks - 1;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    msf = (req->cdb[1] >> 1) & 0x1;
    format = req->cdb[2] & 0xf;
    track_num = req->cdb[6];

    req->req_len = (req->cdb[7] << 8) | req->cdb[8];
    req->in_len = MIN(req->req_len, READ_TOC_RESP_LEN);

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

    SPICE_DEBUG("read_toc, lun:%u len: %" G_GUINT64_FORMAT
                " msf: %x format: 0x%02x track/session: 0x%02x",
                req->lun, req->in_len, msf, format, track_num);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_MODE_PARAM_6_LEN_HEADER              4
#define CD_MODE_PARAM_10_LEN_HEADER             8

#define CD_MODE_PAGE_LEN_RW_ERROR               12

static uint32_t cd_scsi_add_mode_page_rw_error_recovery(CdScsiLU *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_RW_ERROR;

    outbuf[0] = MODE_PAGE_R_W_ERROR;
    outbuf[1] = CD_MODE_PAGE_LEN_RW_ERROR - 2;
    outbuf[3] = 1; /* read retry count */

    return page_len;
}

#define CD_MODE_PAGE_LEN_POWER                  12

static uint32_t cd_scsi_add_mode_page_power_condition(CdScsiLU *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_POWER;

    outbuf[0] = MODE_PAGE_POWER;
    outbuf[1] = CD_MODE_PAGE_LEN_POWER - 2;

    return page_len;
}

#define CD_MODE_PAGE_LEN_FAULT_FAIL             12
#define CD_MODE_PAGE_FAULT_FAIL_FLAG_PERF       0x80

static uint32_t cd_scsi_add_mode_page_fault_reporting(CdScsiLU *dev, uint8_t *outbuf)
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
#define CD_MODE_PAGE_CAPS_LOCK_SUPPORT          (0x01)
#define CD_MODE_PAGE_CAPS_LOCK_STATE            (0x01 << 1)
#define CD_MODE_PAGE_CAPS_PREVENT_JUMPER        (0x01 << 2)
#define CD_MODE_PAGE_CAPS_EJECT                 (0x01 << 3)
#define CD_MODE_PAGE_CAPS_LOADING_TRAY          (0x01 << 5)

static uint32_t cd_scsi_add_mode_page_caps_mech_status(CdScsiLU *dev, uint8_t *outbuf)
{
    uint32_t page_len = CD_MODE_PAGE_LEN_CAPS_MECH_STATUS_RO; /* no write */

    outbuf[0] = MODE_PAGE_CAPS_MECH_STATUS;
    outbuf[1] = page_len;
    outbuf[2] = CD_MODE_PAGE_CAPS_CD_R_READ | CD_MODE_PAGE_CAPS_CD_RW_READ |
                CD_MODE_PAGE_CAPS_DVD_ROM_READ | CD_MODE_PAGE_CAPS_DVD_R_READ |
                CD_MODE_PAGE_CAPS_DVD_RAM_READ;
    outbuf[6] = CD_MODE_PAGE_CAPS_LOADING_TRAY | CD_MODE_PAGE_CAPS_EJECT |
                CD_MODE_PAGE_CAPS_LOCK_SUPPORT;
    if (dev->prevent_media_removal) {
        outbuf[6] |= CD_MODE_PAGE_CAPS_LOCK_STATE;
    }

    return page_len;
}

static void cd_scsi_cmd_mode_sense_10(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    int long_lba, dbd, page, sub_page, pc;
    uint32_t resp_len = CD_MODE_PARAM_10_LEN_HEADER;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    long_lba = (req->cdb[1] >> 4) & 0x1;
    dbd = (req->cdb[1] >> 3) & 0x1;
    page = req->cdb[2] & 0x3f;
    pc = req->cdb[2] >> 6;
    sub_page = req->cdb[3];

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
        SPICE_DEBUG("mode_sense_10, lun:%u"
                    " page 0x%x not implemented",
                    req->lun, (unsigned)page);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }

    outbuf[0] = ((resp_len - 2) >> 8) & 0xff;
    outbuf[1] = (resp_len - 2) & 0xff;

    req->in_len = MIN(req->req_len, resp_len);

    SPICE_DEBUG("mode_sense_10, lun:%u"
                " long_lba %d, dbd %d, page %d, sub_page %d, pc %d; "
                "resp_len %u",
                req->lun, long_lba, dbd, page, sub_page, pc, resp_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_mode_select_6(CdScsiLU *dev, CdScsiRequest *req)
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
        SPICE_DEBUG("mode_select_6, lun:%u"
                    " pf:%u sp:%u"
                    " list_len:%u exceeds data_len:%u",
                    req->lun, page_format, save_pages, list_len, req->buf_len);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_PARAM_LEN);
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

    SPICE_DEBUG("mode_select_6, lun:%u"
                " pf:%u sp:%u list_len:%u data_len:%u"
                " mode_len:%u medium:%u dev_param:%u blk_desc_len:%u"
                " num_blocks:%u block_len:%u"
                " page_num:%u page_len:%u",
                req->lun, page_format, save_pages, list_len, req->buf_len,
                mode_len, medium_type, dev_param, block_desc_len,
                num_blocks, block_len,
                page_num, page_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_mode_select_10(CdScsiLU *dev, CdScsiRequest *req)
{
    uint32_t page_format, save_pages, list_len;

    page_format = (req->cdb[1] >> 4) & 0x1;
    save_pages = req->cdb[1] & 0x1;
    list_len = (req->cdb[7] << 8) | req->cdb[8];

    if (list_len > req->buf_len) {
        SPICE_DEBUG("mode_select_10, lun:%u pf:%u sp:%u"
                    " list_len:%u exceeds data_len:%u",
                    req->lun, page_format, save_pages, list_len, req->buf_len);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_PARAM_LEN);
        return;
    }

    SPICE_DEBUG("mode_select_10, lun:%u pf:%u sp:%u"
                " list_len:%u data_len:%u",
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

static gboolean cd_scsi_feature_reportable(uint32_t feature, uint32_t start_feature,
                                           uint32_t req_type)
{
    return (req_type == CD_FEATURE_REQ_SINGLE && start_feature == feature) ||
           (feature >= start_feature);
}

static uint32_t cd_scsi_add_feature_profiles_list(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_core(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_morph(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_removable(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_random_read(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_cd_read(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_dvd_read(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_power_mgmt(CdScsiLU *dev, uint8_t *outbuf,
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

static uint32_t cd_scsi_add_feature_timeout(CdScsiLU *dev, uint8_t *outbuf,
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

static void cd_scsi_cmd_get_configuration(CdScsiLU *dev, CdScsiRequest *req)
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
        resp_len += cd_scsi_add_feature_profiles_list(dev, outbuf + resp_len, start_feature,
                                                      req_type);
        resp_len += cd_scsi_add_feature_core(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_morph(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_removable(dev, outbuf + resp_len, start_feature, req_type);
        resp_len += cd_scsi_add_feature_random_read(dev, outbuf + resp_len, start_feature,
                                                    req_type);
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
            resp_len += cd_scsi_add_feature_removable(dev, outbuf + resp_len, start_feature,
                                                      req_type);
            break;
        case CD_FEATURE_NUM_RANDOM_READ:
            resp_len += cd_scsi_add_feature_random_read(dev, outbuf + resp_len, start_feature,
                                                        req_type);
            break;
        case CD_FEATURE_NUM_CD_READ:
            resp_len += cd_scsi_add_feature_cd_read(dev, outbuf + resp_len, start_feature,
                                                    req_type);
            break;
        case CD_FEATURE_NUM_DVD_READ:
            resp_len += cd_scsi_add_feature_dvd_read(dev, outbuf + resp_len, start_feature,
                                                     req_type);
            break;
        case CD_FEATURE_NUM_POWER_MNGT:
            resp_len += cd_scsi_add_feature_power_mgmt(dev, outbuf + resp_len, start_feature,
                                                       req_type);
            break;
        case CD_FEATURE_NUM_TIMEOUT:
            resp_len += cd_scsi_add_feature_timeout(dev, outbuf + resp_len, start_feature,
                                                    req_type);
            break;
        default:
            break;
        }
        break;

    default:
        SPICE_DEBUG("get_configuration, lun:%u invalid rt:%u start_f:%u",
                    req->lun, req_type, start_feature);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
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

    req->in_len = MIN(req->req_len, resp_len);

    SPICE_DEBUG("get_configuration, lun:%u rt:%u start_f:%u resp_len:%u",
                req->lun, req_type, start_feature, resp_len);

    cd_scsi_cmd_complete_good(dev, req);
}

#define CD_GET_EVENT_STATUS_IMMED            0x01

#define CD_GET_EVENT_HEADER_NO_EVENT_AVAIL  (0x01 << 7)
#define CD_GET_EVENT_HEADER_LEN             4

#define CD_GET_EVENT_CLASS_NONE             (0x00)
#define CD_GET_EVENT_CLASS_OPER_CHANGE      (0x01)
#define CD_GET_EVENT_CLASS_POWER_MGMT       (0x02)
#define CD_GET_EVENT_CLASS_EXTERNAL_REQ     (0x03)
#define CD_GET_EVENT_CLASS_MEDIA            (0x04)
#define CD_GET_EVENT_CLASS_MULTI_INITIATOR  (0x05)
#define CD_GET_EVENT_CLASS_DEV_BUSY         (0x06)

#define CD_GET_EVENT_LEN_MEDIA              4

#define CD_MEDIA_STATUS_MEDIA_PRESENT       0x1
#define CD_MEDIA_STATUS_TRAY_OPEN           0x2

static uint32_t cd_scsi_cmd_get_event_resp_add_media(CdScsiLU *dev, uint8_t *outbuf)
{
    outbuf[0] = (uint8_t)dev->media_event & 0x0f;
    outbuf[1] = (uint8_t)((dev->loaded ? 0 : CD_MEDIA_STATUS_TRAY_OPEN) |
                          (dev->stream != NULL ? CD_MEDIA_STATUS_MEDIA_PRESENT : 0));

    dev->media_event = CD_MEDIA_EVENT_NO_CHANGE; /* reset the event */
    return CD_GET_EVENT_LEN_MEDIA;
}

#define CD_GET_EVENT_LEN_POWER              4

#define CD_POWER_STATUS_ACTIVE              0x1
#define CD_POWER_STATUS_IDLE                0x2

static uint32_t cd_scsi_cmd_get_event_resp_add_power(CdScsiLU *dev, uint8_t *outbuf)
{
    outbuf[0] = (uint8_t)dev->power_event & 0x0f;
    outbuf[1] = (uint8_t)((dev->power_cond == CD_SCSI_POWER_ACTIVE) ?
                           CD_POWER_STATUS_ACTIVE : CD_POWER_STATUS_IDLE);

    dev->power_event = CD_POWER_EVENT_NO_CHANGE; /* reset the event */
    return CD_GET_EVENT_LEN_POWER;
}

static void cd_scsi_cmd_get_event_status_notification(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t resp_len = CD_GET_EVENT_HEADER_LEN;
    const uint32_t power_class_mask = (0x01 << CD_GET_EVENT_CLASS_POWER_MGMT);
    const uint32_t media_class_mask = (0x01 << CD_GET_EVENT_CLASS_MEDIA);
    uint32_t classes_supported =  power_class_mask | media_class_mask;
    uint32_t immed, classes_requested;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    immed = req->cdb[1] & CD_GET_EVENT_STATUS_IMMED;
    classes_requested = req->cdb[4];
    req->req_len = (req->cdb[7] << 8) | req->cdb[8];

    if (!immed) {
        SPICE_DEBUG("get_event_status_notification, lun:%u"
                " imm:0 class_req:%02x, Non-immediate (async) mode unsupported",
                req->lun, classes_requested);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }

    memset(outbuf, 0, req->req_len);
    if ((classes_supported & classes_requested) != 0) {
        if (classes_requested & power_class_mask) {
            outbuf[2] = CD_GET_EVENT_CLASS_POWER_MGMT;
            outbuf[3] = (uint8_t)power_class_mask;

            SPICE_DEBUG("get_event_status_notification, lun:%u"
                        " imm:%u class_req:0x%02x class_sup:0x%02x"
                        " power_event:0x%02x power_cond:0x%02x",
                        req->lun, immed, classes_requested, classes_supported,
                        dev->power_event, dev->power_cond);

            resp_len += cd_scsi_cmd_get_event_resp_add_power(dev, outbuf + resp_len);
        } else if (classes_requested & media_class_mask) {
            outbuf[2] = CD_GET_EVENT_CLASS_MEDIA;
            outbuf[3] = (uint8_t)media_class_mask;

            SPICE_DEBUG("get_event_status_notification, lun:%u"
                        " imm:%u class_req:0x%02x class_sup:0x%02x"
                        " media_event:0x%02x loaded: %d",
                        req->lun, immed, classes_requested, classes_supported,
                        dev->media_event, dev->loaded);

            resp_len += cd_scsi_cmd_get_event_resp_add_media(dev, outbuf + resp_len);
        }
    } else {
        outbuf[2] = CD_GET_EVENT_HEADER_NO_EVENT_AVAIL | CD_GET_EVENT_CLASS_NONE;

        SPICE_DEBUG("get_event_status_notification, lun:%u"
                    " imm:%u class_req:0x%02x class_sup:0x%02x"
                    " none of requested events supported",
                    req->lun, immed, classes_requested, classes_supported);
    }
    outbuf[1] = (uint8_t)(resp_len - 2); /* Event Data Length LSB, length excluding the
                                          * field itself */
    outbuf[3] = (uint8_t)classes_supported;

    req->in_len = MIN(req->req_len, resp_len);
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

static void cd_scsi_cmd_send_event(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *param, *event;
    uint32_t immed, param_list_len;
    uint32_t event_param_len, notification_class;
    uint32_t ext_req_event, ext_req_status, pers_prevent, ext_req_code;

    req->xfer_dir = SCSI_XFER_TO_DEV;

    immed = req->cdb[1] & 0x01;
    param_list_len = (req->cdb[8] << 8) | req->cdb[9];

    if (req->buf_len < param_list_len) {
        SPICE_DEBUG("send_event, lun:%u invalid param list len:0x%x, buf_len:0x%x",
                    req->lun, param_list_len, req->buf_len);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_PARAM_LEN);
        return;
    }
    param = req->buf;
    event_param_len = (param[0] << 8) | param[1];

    notification_class = param[2] & 0x07;
    if (notification_class != CD_GET_EVENT_CLASS_EXTERNAL_REQ) {
        SPICE_DEBUG("send_event, lun:%u invalid notification class:0x%x",
                    req->lun, notification_class);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }

    event = param + CD_GET_EVENT_HEADER_LEN;
    ext_req_event = event[0] & 0xff;
    ext_req_status = event[1] & 0x0f;
    pers_prevent = event[1] & 0x80;
    ext_req_code = (event[2] << 8) | event[3];

    SPICE_DEBUG("send_event, lun:%u immed:%u param_len:%u"
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

static void cd_scsi_cmd_allow_medium_removal(CdScsiLU *dev, CdScsiRequest *req)
{
    uint32_t prevent;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    prevent = req->cdb[4] & 0x03;
    dev->prevent_media_removal = (prevent == CD_MEDIUM_REMOVAL_REQ_PREVENT ||
                                  prevent == CD_MEDIUM_REMOVAL_REQ_PREVENT_CHANGER);
    req->in_len = 0;

    SPICE_DEBUG("allow_medium_removal, lun:%u prevent field::0x%02x flag:%d",
                req->lun, prevent, dev->prevent_media_removal);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_report_key(CdScsiLU *dev, CdScsiRequest *req)
{
    SPICE_DEBUG("report_key - content protection unsupported, lun:%u", req->lun);
    req->xfer_dir = SCSI_XFER_NONE;
    cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_OPCODE);
}

static void cd_scsi_cmd_send_key(CdScsiLU *dev, CdScsiRequest *req)
{
    SPICE_DEBUG("send_key - content protection unsupported, lun:%u", req->lun);
    req->xfer_dir = SCSI_XFER_NONE;
    cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_OPCODE);
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

static void cd_scsi_cmd_start_stop_unit(CdScsiLU *dev, CdScsiRequest *req)
{
    gboolean immed, start, load_eject;
    uint32_t power_cond;

    req->xfer_dir = SCSI_XFER_NONE;
    req->in_len = 0;

    immed = (req->cdb[1] & CD_START_STOP_FLAG_IMMED) ? TRUE : FALSE;
    start = (req->cdb[4] & CD_START_STOP_FLAG_START) ? TRUE : FALSE;
    load_eject = (req->cdb[4] & CD_START_STOP_FLAG_LOEJ) ? TRUE : FALSE;
    power_cond = req->cdb[4] >> 4;

    SPICE_DEBUG("start_stop_unit, lun:%u"
                " immed:%d start:%d load_eject:%d power_cond:0x%x(%s)",
                req->lun, immed, start, load_eject, power_cond,
                cd_scsi_start_stop_power_cond_name(power_cond));

    switch (power_cond) {
    case CD_START_STOP_POWER_COND_START_VALID:
        if (!start) { /* stop the unit */
            if (load_eject) { /* eject medium */
                if (dev->prevent_media_removal) {
                    SPICE_DEBUG("start_stop_unit, lun:%u"
                                " prevent_media_removal set, eject failed", req->lun);
                    cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_MEDIUM_REMOVAL_PREVENTED);
                    return;
                }
                SPICE_DEBUG("start_stop_unit, lun:%u eject", req->lun);
                cd_scsi_lu_unload(dev);
                cd_scsi_dev_changed(dev->tgt->user_data, req->lun);
            }
            dev->power_cond = CD_SCSI_POWER_STOPPED;
            SPICE_DEBUG("start_stop_unit, lun:%u stopped", req->lun);
        } else { /* start the unit */
            dev->power_cond = CD_SCSI_POWER_ACTIVE;
            SPICE_DEBUG("start_stop_unit, lun:%u started", req->lun);

            if (load_eject) { /* load medium */
                SPICE_DEBUG("start_stop_unit, lun:%u load with no media",
                            req->lun);
                cd_scsi_lu_load(dev, NULL);
                cd_scsi_dev_changed(dev->tgt->user_data, req->lun);
            }
        }
        break;
    case CD_START_STOP_POWER_COND_ACTIVE:
        /* not error to specify transition to the current power condition */
        dev->power_cond = CD_SCSI_POWER_ACTIVE;
        SPICE_DEBUG("start_stop_unit, lun:%u active", req->lun);
        break;
    case CD_START_STOP_POWER_COND_IDLE:
    case CD_START_STOP_POWER_COND_FORCE_IDLE_0:
        dev->power_cond = CD_SCSI_POWER_IDLE;
        SPICE_DEBUG("start_stop_unit, lun:%u idle", req->lun);
        break;
    case CD_START_STOP_POWER_COND_STANDBY:
    case CD_START_STOP_POWER_COND_FORCE_STANDBY_0:
        dev->power_cond = CD_SCSI_POWER_STANDBY;
        SPICE_DEBUG("start_stop_unit, lun:%u standby", req->lun);
        break;
    case CD_START_STOP_POWER_COND_LU_CONTROL:
        break;
    default:
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
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


static void cd_scsi_get_performance_resp_empty(CdScsiLU *dev, CdScsiRequest *req,
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

    SPICE_DEBUG("get_performance, lun:%u"
                " type:0x%x data_type:0x%x - sending empty response",
                req->lun, type, data_type);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_get_performance_resp_performance(CdScsiLU *dev, CdScsiRequest *req,
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

    SPICE_DEBUG("get_performance, lun:%u"
                " performance type:0x00 data_type:0x%x"
                " except:0x%x write:0x%x tolerance:0x%x"
                " max_num:%u",
                req->lun, data_type, except, write,
                tolerance, max_num_descr);

    if (write) {
        SPICE_DEBUG("get_performance, lun:%u"
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

    req->in_len = MIN(req->req_len, resp_len);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_cmd_get_performance(CdScsiLU *dev, CdScsiRequest *req)
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
        SPICE_DEBUG("get_performance, lun:%u"
                    " unsupported type:0x%x"
                    " data_type:0x%x max_num:%u",
                    req->lun, type, data_type, max_num_descr);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        return;
    }
}

#define CD_MECHANISM_STATUS_HDR_LEN     8

/* byte 0 */
#define CD_CHANGER_FAULT_FLAG           0x80

#define CD_CHANGER_READY                0x00
#define CD_CHANGER_LOADING              0x01
#define CD_CHANGER_UNLOADING            0x02
#define CD_CHANGER_INITIALIZING         0x03

/* byte 1 */
#define CD_CHANGER_DOOR_OPEN_FLAG       0x10

#define CD_MECHANISM_STATE_IDLE         0x00
#define CD_MECHANISM_STATE_PLAYING      0x01
#define CD_MECHANISM_STATE_SCANNING     0x02
/* ACTIVE: with Initiator, Composite or Other ports
  (i.e. READ, PLAY CD, SCAN during PLAY CD) */
#define CD_MECHANISM_STATE_ACTIVE       0x03
#define CD_MECHANISM_STATE_NO_INFO      0x07

/* slots */
#define CD_MECHANISM_STATUS_SLOT_LEN    4

#define CD_MECHANISM_SLOT_DISK_CHANGED  0x01
#define CD_MECHANISM_SLOT_DISK_PRESENT  0x80
#define CD_MECHANISM_SLOT_DISK_CWP_V    0x02
#define CD_MECHANISM_SLOT_DISK_CWP      0x01

static void cd_scsi_cmd_mechanism_status(CdScsiLU *dev, CdScsiRequest *req)
{
    uint8_t *outbuf = req->buf;
    uint32_t resp_len = CD_MECHANISM_STATUS_HDR_LEN;

    req->xfer_dir = SCSI_XFER_FROM_DEV;

    req->req_len = (req->cdb[8] << 8) | req->cdb[9];
    memset(outbuf, 0, req->req_len);

    /* For non-changer devices the curent slot number field is reserved, set only status */
    outbuf[0] |= (CD_CHANGER_READY << 5);

    outbuf[1] |= (!dev->loaded) ? CD_CHANGER_DOOR_OPEN_FLAG : 0;
    outbuf[1] |= (dev->power_cond == CD_POWER_STATUS_ACTIVE) ?
                 (CD_MECHANISM_STATE_ACTIVE << 5) : (CD_MECHANISM_STATE_IDLE << 5);

    /* For non-changer devices the number of slot tables returned shall be zero, so we leave
       both 'Number of Slots Available' and 'Length of Slot Table' fields as zeros */

    req->in_len = MIN(req->req_len, resp_len);

    SPICE_DEBUG("mechanism_status, lun:%u", req->lun);

    cd_scsi_cmd_complete_good(dev, req);
}

static void cd_scsi_read_async_complete(GObject *src_object,
                                        GAsyncResult *result,
                                        gpointer user_data)
{
    GFileInputStream *stream = G_FILE_INPUT_STREAM(src_object);
    CdScsiRequest *req = (CdScsiRequest *)user_data;
    CdScsiTarget *st = (CdScsiTarget *)req->priv_data;
    CdScsiLU *dev = &st->units[req->lun];
    GError *error = NULL;
    gsize bytes_read;
    gboolean finished;

    req->req_state = SCSI_REQ_COMPLETE;
    req->cancel_id = 0;

//    g_assert(stream == dev->stream);
    if (stream != dev->stream) {
        uint32_t opcode = (uint32_t)req->cdb[0];
        SPICE_DEBUG("read_async_complete BAD STREAM, lun: %u"
                    " req: %" G_GUINT64_FORMAT " op: 0x%02x",
                    req->lun, req->req_len, opcode);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_TARGET_FAILURE);
        cd_scsi_dev_request_complete(st->user_data, req);
        return;
    }

    bytes_read = g_input_stream_read_finish(G_INPUT_STREAM(stream), result, &error);
    finished = bytes_read > 0;
    if (finished) {
        SPICE_DEBUG("read_async_complete, lun: %u"
                    " finished: %d bytes_read: %" G_GUINT64_FORMAT
                    " req: %"  G_GUINT64_FORMAT,
                    req->lun, finished, (uint64_t)bytes_read, req->req_len);

        req->in_len = MIN(bytes_read, req->req_len);
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
    CdScsiRequest *req = (CdScsiRequest *)user_data;
    CdScsiTarget *st = (CdScsiTarget *)req->priv_data;

    g_assert(cancellable == st->cancellable);
    g_cancellable_disconnect(cancellable, req->cancel_id);
    req->cancel_id = 0;

    req->req_state =
        (st->state == CD_SCSI_TGT_STATE_RUNNING) ? SCSI_REQ_CANCELED : SCSI_REQ_DISPOSED;
    req->in_len = 0;
    req->status = GOOD;

    cd_scsi_dev_request_complete(st->user_data, req);
}

static int cd_scsi_read_async_start(CdScsiLU *dev, CdScsiRequest *req)
{
    CdScsiTarget *st = dev->tgt;
    GFileInputStream *stream = dev->stream;

    SPICE_DEBUG("read_async_start, lun:%u"
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

static void cd_scsi_cmd_read(CdScsiLU *dev, CdScsiRequest *req)
{
    if (dev->power_cond == CD_SCSI_POWER_STOPPED) {
        SPICE_DEBUG("read, lun: %u is stopped", req->lun);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INIT_CMD_REQUIRED);
        return;
    } else if (!dev->loaded || dev->stream == NULL) {
        SPICE_DEBUG("read, lun: %u is not loaded", req->lun);
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_NOT_READY_NO_MEDIUM);
        return;
    }

    req->cdb_len = scsi_cdb_length(req->cdb);

    req->lba = scsi_cdb_lba(req->cdb, req->cdb_len);
    req->offset = req->lba * dev->block_size;

    req->count = scsi_cdb_xfer_length(req->cdb, req->cdb_len); /* xfer in blocks */
    req->req_len = (uint64_t) req->count * dev->block_size;

    cd_scsi_read_async_start(dev, req);
}

void cd_scsi_dev_request_submit(CdScsiTarget *st, CdScsiRequest *req)
{
    uint32_t lun = req->lun;
    uint32_t opcode = (uint32_t)req->cdb[0];
    const char *cmd_name = scsi_cmd_name[opcode];
    CdScsiLU *dev = &st->units[lun];

    SPICE_DEBUG("request_submit, lun: %u op: 0x%02x %s", lun, opcode, cmd_name);

    if (st->cur_req != NULL) {
        SPICE_ERROR("request_submit, request not idle");
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_TARGET_FAILURE);
        goto done;
    }
    if (req->req_state != SCSI_REQ_IDLE) {
        SPICE_ERROR("request_submit, prev request outstanding");
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_TARGET_FAILURE);
        goto done;
    }
    req->req_state = SCSI_REQ_RUNNING;
    st->cur_req = req;

    /* INQUIRY should send response even for non-existing LUNs */
    if (!cd_scsi_target_lun_legal(st, lun)) {
        SPICE_ERROR("request_submit, illegal lun:%u", lun);
        if (opcode == INQUIRY) {
            if (req->cdb[1] & 0x1) {
                cd_scsi_cmd_inquiry_vpd_no_lun(dev, req, PERIF_QUALIFIER_UNSUPPORTED);
            } else {
                cd_scsi_cmd_inquiry_standard_no_lun(dev, req, PERIF_QUALIFIER_UNSUPPORTED);
            }
        } else {
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_LUN_NOT_SUPPORTED);
        }
        goto done;
    }
    if (!cd_scsi_target_lun_realized(st, lun)) {
        SPICE_ERROR("request_submit, absent lun:%u", lun);
        if (opcode == INQUIRY) {
            if (req->cdb[1] & 0x1) {
                cd_scsi_cmd_inquiry_vpd_no_lun(dev, req, PERIF_QUALIFIER_NOT_CONNECTED);
            } else {
                cd_scsi_cmd_inquiry_standard_no_lun(dev, req, PERIF_QUALIFIER_NOT_CONNECTED);
            }
        } else {
            cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_LUN_NOT_SUPPORTED);
        }
        goto done;
    }

    if (dev->short_sense.key != NO_SENSE) {
        gboolean pending_sense = TRUE;
        if (dev->short_sense.key == UNIT_ATTENTION) {
            if (cd_scsi_opcode_ua_supress(opcode)) {
                pending_sense = FALSE; /* UA supressed */
            }
        } else if (opcode == REQUEST_SENSE) {
            pending_sense = FALSE; /* sense returned as data */
        }
        if (pending_sense) {
            cd_scsi_cmd_complete_check_cond(dev, req, NULL); /* sense already set */
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
    case MMC_MECHANISM_STATUS:
        cd_scsi_cmd_mechanism_status(dev, req);
        break;
    default:
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_OPCODE);
        break;
    }

    if (req->req_len > INT32_MAX) {
        cd_scsi_cmd_complete_check_cond(dev, req, &sense_code_INVALID_CDB_FIELD);
        goto done;
    }

done:
    SPICE_DEBUG("request_submit done, lun: %u"
                " op: 0x%02x %s, state: %s status: %u len: %" G_GUINT64_FORMAT,
                lun, opcode, cmd_name, CdScsiReqState_str(req->req_state), req->status,
                req->in_len);

    if (req->req_state == SCSI_REQ_COMPLETE) {
        cd_scsi_dev_request_complete(st->user_data, req);
    }
}

void cd_scsi_dev_request_cancel(CdScsiTarget *st, CdScsiRequest *req)
{
    if (st->cur_req == req) {
        if (req->req_state == SCSI_REQ_RUNNING) {
            SPICE_DEBUG("request_cancel: lun: %u"
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

void cd_scsi_dev_request_release(CdScsiTarget *st, CdScsiRequest *req)
{
    st->cur_req = NULL;
    cd_scsi_req_init(req);

    if (st->state == CD_SCSI_TGT_STATE_RESET) {
        cd_scsi_target_do_reset(st);
    }
}

#endif /* USE_USBREDIR */
