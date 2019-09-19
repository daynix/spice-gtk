/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
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

#include <gio/gio.h>

#include "cd-scsi-dev-params.h"

G_BEGIN_DECLS

typedef enum CdUsbBulkStatus {
    BULK_STATUS_GOOD = 0,
    BULK_STATUS_ERROR,
    BULK_STATUS_CANCELED,
    BULK_STATUS_STALL,
} CdUsbBulkStatus;

typedef struct UsbCdBulkMsdDevice UsbCdBulkMsdDevice;

/* USB backend callbacks */

/* called on completed read data bulk transfer
 * user_data - user_data in unit parameters structure
 * status - bulk status code
 */
void cd_usb_bulk_msd_read_complete(void *user_data,
                                   uint8_t *data, uint32_t length,
                                   CdUsbBulkStatus status);

/* called when state of device's unit changed to signal GUI component
 *  user_data - user_data in unit parameters structure
 */
void cd_usb_bulk_msd_lun_changed(void *user_data, uint32_t lun);

/* called on completed device reset
 * user_data - user_data in unit parameters structure
 * status - error code
 */
void cd_usb_bulk_msd_reset_complete(void *user_data,
                                    int status);

/* MSD backend api */

/* allocate new device descriptor */
UsbCdBulkMsdDevice *cd_usb_bulk_msd_alloc(void *user_data, uint32_t max_lun);

/* free device descriptor */
void cd_usb_bulk_msd_free(UsbCdBulkMsdDevice *device);

/* configure a new Logical Unit to be represen ted by the device
 *  returns: error code
 */
int cd_usb_bulk_msd_realize(UsbCdBulkMsdDevice *device, uint32_t lun,
                            const CdScsiDeviceParameters *dev_params);

/* lock the device, prevent unloading
 * returns: error code
 */
int cd_usb_bulk_msd_lock(UsbCdBulkMsdDevice *device, uint32_t lun, gboolean lock);

/* load new media, if already loaded, simulate media change
 * returns: error code
 */
int cd_usb_bulk_msd_load(UsbCdBulkMsdDevice *device, uint32_t lun,
                         const CdScsiMediaParameters *media_params);

/* query unit parameters and status
 * returns: error code
 */
int cd_usb_bulk_msd_get_info(UsbCdBulkMsdDevice *device, uint32_t lun,
                             CdScsiDeviceInfo *lun_info);

/* unload the media
 * returns: error code
 */
int cd_usb_bulk_msd_unload(UsbCdBulkMsdDevice *device, uint32_t lun);

/* detach a Logical Unit
 * returns: error code
 */
int cd_usb_bulk_msd_unrealize(UsbCdBulkMsdDevice *device, uint32_t lun);

/* reset the device instance; cancel all IO ops, reset state
 * returns: error code
 */
int cd_usb_bulk_msd_reset(UsbCdBulkMsdDevice *device);


/* perform a write data bulk transfer
 * data_len - length of available data to write
 * returns: error code
 */
int cd_usb_bulk_msd_write(UsbCdBulkMsdDevice*device, uint8_t *buf, uint32_t data_len);


/* perform a read data bulk transfer
 * max_len - length of available buffer to fill
 * If data available immediately, should call cd_usb_bulk_msd_read_complete()
 *   and return success
 * If fatal error detected immediately, should call cd_usb_bulk_msd_read_complete()
 *   with error code and return success
 *
 * returns: 0 - success, -1 - error
 */
int cd_usb_bulk_msd_read(UsbCdBulkMsdDevice *device, uint32_t max_len);

/* cancels pending read data bulk transfer
 * returns: error code
 */
int cd_usb_bulk_msd_cancel_read(UsbCdBulkMsdDevice *device);

G_END_DECLS
