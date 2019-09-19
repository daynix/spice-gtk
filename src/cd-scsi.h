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

#pragma once

#include "cd-scsi-dev-params.h"
#include "cd-usb-bulk-msd.h"
#include "scsi-constants.h"

#if defined(G_OS_WIN32)
#include <winsock2.h>
#include <windows.h>
/* Windows is always LE at the moment */
#define le32toh(x)          (x)
#define htole32(x)          (x)
#define htobe32(x)          htonl(x)
#endif

typedef enum ScsiXferDir {
    SCSI_XFER_NONE = 0,  /* TEST_UNIT_READY, ...           */
    SCSI_XFER_FROM_DEV,  /* READ, INQUIRY, MODE_SENSE, ... */
    SCSI_XFER_TO_DEV,    /* WRITE, MODE_SELECT, ...        */
} ScsiXferDir;

#define SCSI_CDB_BUF_SIZE   16

typedef enum CdScsiReqState {
    SCSI_REQ_IDLE = 0,
    SCSI_REQ_RUNNING,
    SCSI_REQ_COMPLETE,
    SCSI_REQ_CANCELED,
    SCSI_REQ_DISPOSED,
} CdScsiReqState;

typedef struct CdScsiRequest {
    /* request */
    uint8_t cdb[SCSI_CDB_BUF_SIZE];
    uint32_t cdb_len;

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
typedef struct CdScsiTarget CdScsiTarget;

/* to be used in callbacks */
CdScsiTarget *cd_scsi_target_alloc(void *target_user_data, uint32_t max_luns);
void cd_scsi_target_free(CdScsiTarget *scsi_target);

int cd_scsi_dev_realize(CdScsiTarget *scsi_target, uint32_t lun,
                        const CdScsiDeviceParameters *dev_params);
int cd_scsi_dev_unrealize(CdScsiTarget *scsi_target, uint32_t lun);

int cd_scsi_dev_lock(CdScsiTarget *scsi_target, uint32_t lun, gboolean lock);
int cd_scsi_dev_load(CdScsiTarget *scsi_target, uint32_t lun,
                     const CdScsiMediaParameters *media_params);
int cd_scsi_dev_get_info(CdScsiTarget *scsi_target, uint32_t lun, CdScsiDeviceInfo *lun_info);
int cd_scsi_dev_unload(CdScsiTarget *scsi_target, uint32_t lun);

void cd_scsi_dev_request_submit(CdScsiTarget *scsi_target, CdScsiRequest *request);
void cd_scsi_dev_request_cancel(CdScsiTarget *scsi_target, CdScsiRequest *request);
void cd_scsi_dev_request_release(CdScsiTarget *scsi_target, CdScsiRequest *request);

int cd_scsi_dev_reset(CdScsiTarget *scsi_target, uint32_t lun);

int cd_scsi_target_reset(CdScsiTarget *scsi_target);

/* Callbacks
 * These callbacks are used by upper layer to implement specific SCSI
 * target devices.
 */
void cd_scsi_dev_request_complete(void *target_user_data, CdScsiRequest *request);
void cd_scsi_dev_changed(void *target_user_data, uint32_t lun);
void cd_scsi_dev_reset_complete(void *target_user_data, uint32_t lun);
void cd_scsi_target_reset_complete(void *target_user_data);
