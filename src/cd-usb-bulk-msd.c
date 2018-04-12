/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
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

#include <gio/gio.h>
#include "spice/types.h"
#include "cd-usb-bulk-msd.h"

#define MAX_UNITS   32

typedef struct _bulk_msd_unit
{
    cd_usb_bulk_unit_parameters params;
    GCancellable *cancel;
    uint32_t present    : 1;
} bulk_msd_unit;

typedef struct _bulk_msd_device
{
    int max_units;
    bulk_msd_unit units[MAX_UNITS];
} bulk_msd_device;


void *cd_usb_bulk_msd_alloc(int max_lun)
{
    if (max_lun <= 0 || max_lun > MAX_UNITS) {
        return NULL;
    }
    return g_new0(bulk_msd_device, 1);
} 

void cd_usb_bulk_msd_free(void *dev)
{
    g_free(dev);
}

int cd_usb_bulk_msd_realize(void *device,
    const cd_usb_bulk_unit_parameters *params)
{
    bulk_msd_device *dev = (bulk_msd_device *)device;
    dev->units[params->lun].params = *params;
    dev->units[params->lun].present = TRUE;
    dev->units[params->lun].cancel = NULL;
    return 0;
}

int cd_usb_bulk_msd_unrealize(void *device, uint32_t lun)
{
    bulk_msd_device *dev = (bulk_msd_device *)device;
    dev->units[lun].present = FALSE;
    return 0;
}

int cd_usb_bulk_msd_reset(void *device)
{
    bulk_msd_device *dev = (bulk_msd_device *)device;
    return (dev != NULL) ? 0 : -1;
}

int cd_usb_bulk_msd_write(void *device,
    uint8_t *buf, uint32_t data_len)
{
    bulk_msd_device *dev = (bulk_msd_device *)device;
    return (dev != NULL) ? 0 : -1;
}

int cd_usb_bulk_msd_read(void *device,
    uint32_t max_len)
{
    bulk_msd_device *dev = (bulk_msd_device *)device;
    uint32_t lun = 0;
    cd_usb_bulk_msd_read_complete(dev->units[lun].params.user_data, NULL, 0, -1);
    return 0;
}

int cd_usb_bulk_msd_cancel_read(void *device)
{
    return -1;
}
