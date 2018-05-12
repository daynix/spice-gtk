/* cd-scsi-params.h */

#ifndef _CD_SCSI_DEV_PARAMS_H_
#define _CD_SCSI_DEV_PARAMS_H_

#include <gio/gio.h>

typedef struct _cd_scsi_device_parameters
{
    const char *vendor;
    const char *product;
    const char *version;
    const char *serial;
} cd_scsi_device_parameters;

typedef struct _cd_scsi_media_parameters
{
    GFileInputStream *stream;
    uint64_t size;
    uint32_t block_size;
} cd_scsi_media_parameters;

#endif /* _CD_SCSI_DEV_PARAMS_H_ */