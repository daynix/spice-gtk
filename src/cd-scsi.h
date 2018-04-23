/* cd-scsi.h */

#ifndef _CD_SCSI_H_
#define _CD_SCSI_H_

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

typedef struct _scsi_short_sense
{
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
} scsi_short_sense;

typedef enum _scsi_xfer_dir
{
    SCSI_XFER_NONE = 0,  /* TEST_UNIT_READY, ...            */
    SCSI_XFER_FROM_DEV,  /* READ, INQUIRY, MODE_SENSE, ...  */
    SCSI_XFER_TO_DEV,    /* WRITE, MODE_SELECT, ...         */
} scsi_xfer_dir;

#define SCSI_CDB_BUF_SIZE   16
#define SCSI_SENSE_BUF_SIZE 64

typedef enum _cd_scsi_req_state
{
    SCSI_REQ_IDLE = 0,
    SCSI_REQ_RUNNING,
    SCSI_REQ_COMPLETE,
    SCSI_REQ_CANCELED,
} cd_scsi_req_state;

typedef struct _cd_scsi_request
{
    /* request */
    uint8_t cdb[SCSI_CDB_BUF_SIZE];
    uint32_t cdb_len;

    uint32_t tag;
    uint32_t lun;

    uint8_t *buf;
    uint32_t buf_len;

    /* internal */
    cd_scsi_req_state req_state;
    scsi_xfer_dir xfer_dir;

    void *priv_data;

    uint64_t lba; /* offset in logical blocks if relevant */
    uint64_t count; /* count in logical blocks */

    uint64_t offset; /* scsi cdb offset, normalized to bytes */
    uint64_t req_len; /* scsi cdb request length, normalized to bytes */

    /* result */
    uint64_t in_len; /* length of data actually available after read */
    uint32_t status; /* SCSI status code */

} cd_scsi_request;

cd_scsi_req_state cd_scsi_get_req_state(cd_scsi_request *req);

/* SCSI target/device API */

void *cd_scsi_target_alloc(void *target_user_data, uint32_t max_luns); /* to be used in callbacks */
void cd_scsi_target_free(void *scsi_target);

int cd_scsi_dev_realize(void *scsi_target, uint32_t lun, cd_scsi_device_parameters *param);
int cd_scsi_dev_unrealize(void *scsi_target, uint32_t lun);

void cd_scsi_dev_request_submit(void *scsi_target, cd_scsi_request *request);
void cd_scsi_dev_request_release(void *scsi_target, cd_scsi_request *request);

int cd_scsi_dev_reset(void *scsi_target, uint32_t lun);

int cd_scsi_target_reset(void *scsi_target);

/* Callbacks */

void cd_scsi_dev_request_complete(void *target_user_data, cd_scsi_request *request);
void cd_scsi_dev_reset_complete(void *target_user_data, uint32_t lun);
void cd_scsi_target_reset_complete(void *target_user_data);

#endif /* _CD_SCSI_H_ */