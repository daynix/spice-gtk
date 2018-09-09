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

#ifndef __CD_SCSI_H__
#define __CD_SCSI_H__

#include "cd-scsi-dev-params.h"
#include "cd-usb-bulk-msd.h"
#include "scsi-constants.h"

#define FIXED_SENSE_CURRENT 0x70
#define FIXED_SENSE_LEN 18

#if defined(G_OS_WIN32)
#include <winsock2.h>
#include <windows.h>
/* Windows is always LE at the moment */
#define le32toh(x)          (x)
#define htole32(x)          (x)
#define htobe32(x)          htonl(x)
#endif

typedef enum _ScsiXferDir
{
    SCSI_XFER_NONE = 0,  /* TEST_UNIT_READY, ...           */
    SCSI_XFER_FROM_DEV,  /* READ, INQUIRY, MODE_SENSE, ... */
    SCSI_XFER_TO_DEV,    /* WRITE, MODE_SELECT, ...        */
} ScsiXferDir;

#define SCSI_CDB_BUF_SIZE   16
#define SCSI_SENSE_BUF_SIZE 64

typedef enum _CdScsiReqState
{
    SCSI_REQ_IDLE = 0,
    SCSI_REQ_RUNNING,
    SCSI_REQ_COMPLETE,
    SCSI_REQ_CANCELED,
    SCSI_REQ_DISPOSED,
} CdScsiReqState;

typedef struct _CdScsiRequest
{
    /* request */
    uint8_t cdb[SCSI_CDB_BUF_SIZE];
    uint32_t cdb_len;

    uint32_t tag;
    uint32_t lun;

    uint8_t *buf;
    uint32_t buf_len;

    /* internal */
    CdScsiReqState req_state;
    ScsiXferDir xfer_dir;
    uint64_t cancel_id;
    void *priv_data;

    uint64_t lba; /* offset in logical blocks if relevant */
    uint64_t count; /* count in logical blocks */

    uint64_t offset; /* scsi cdb offset, normalized to bytes */
    uint64_t req_len; /* scsi cdb request length, normalized to bytes */

    /* result */
    uint64_t in_len; /* length of data actually available after read */
    uint32_t status; /* SCSI status code */

} CdScsiRequest;

CdScsiReqState cd_scsi_get_req_state(CdScsiRequest *req);

/* SCSI target/device API */

void *cd_scsi_target_alloc(void *target_user_data, uint32_t max_luns); /* to be used in callbacks */
void cd_scsi_target_free(void *scsi_target);

int cd_scsi_dev_realize(void *scsi_target, uint32_t lun, const CdScsiDeviceParameters *dev_params);
int cd_scsi_dev_unrealize(void *scsi_target, uint32_t lun);

int cd_scsi_dev_lock(void *scsi_target, uint32_t lun, gboolean lock);
int cd_scsi_dev_load(void *scsi_target, uint32_t lun, const CdScsiMediaParameters *media_params);
int cd_scsi_dev_get_info(void *scsi_target, uint32_t lun, CdScsiDeviceInfo *lun_info);
int cd_scsi_dev_unload(void *scsi_target, uint32_t lun);

void cd_scsi_dev_request_submit(void *scsi_target, CdScsiRequest *request);
void cd_scsi_dev_request_cancel(void *scsi_target, CdScsiRequest *request);
void cd_scsi_dev_request_release(void *scsi_target, CdScsiRequest *request);

int cd_scsi_dev_reset(void *scsi_target, uint32_t lun);

int cd_scsi_target_reset(void *scsi_target);

/* Callbacks */

void cd_scsi_dev_request_complete(void *target_user_data, CdScsiRequest *request);
void cd_scsi_dev_changed(void *target_user_data, uint32_t lun);
void cd_scsi_dev_reset_complete(void *target_user_data, uint32_t lun);
void cd_scsi_target_reset_complete(void *target_user_data);

#endif /* __CD_SCSI_H__ */
