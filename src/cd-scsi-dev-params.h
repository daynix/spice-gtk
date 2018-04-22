/* cd-scsi-params.h */

#ifndef _CD_SCSI_DEV_PARAMS_H_
#define _CD_SCSI_DEV_PARAMS_H_

#include <gio/gio.h>

typedef struct _cd_scsi_device_parameters
{
    uint64_t size;
    uint32_t block_size;

    const char *vendor;
    const char *product;
    const char *version;
    const char *serial;

    GFileInputStream *stream;
} cd_scsi_device_parameters;

#endif /* _CD_SCSI_DEV_PARAMS_H_ */