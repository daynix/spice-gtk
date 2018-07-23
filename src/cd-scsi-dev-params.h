/* cd-scsi-params.h */

#ifndef _CD_SCSI_DEV_PARAMS_H_
#define _CD_SCSI_DEV_PARAMS_H_

#include <gio/gio.h>

typedef struct _CdScsiDeviceParameters
{
    const char *vendor;
    const char *product;
    const char *version;
    const char *serial;
} CdScsiDeviceParameters;

typedef struct _CdScsiDeviceInfo
{
    CdScsiDeviceParameters parameters;
    uint32_t started    : 1;
    uint32_t locked     : 1;
    uint32_t loaded     : 1;
} CdScsiDeviceInfo;

typedef struct _CdScsiMediaParameters
{
    GFileInputStream *stream;
    uint64_t size;
    uint32_t block_size;
} CdScsiMediaParameters;

#endif /* _CD_SCSI_DEV_PARAMS_H_ */