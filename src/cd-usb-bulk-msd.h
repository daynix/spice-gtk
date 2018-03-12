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

#ifndef __CD_USB_BULK_MSD_H__
#define __CD_USB_BULK_MSD_H__

typedef struct _cd_usb_bulk_unit_parameters
{
    void *user_data; /* user data to be used on read callback */
    uint32_t lun;
    uint32_t block_size;
    const char *vendor;
    const char *product;
    const char *version;
    const char *serial;
    GInputStream *stream;
} cd_usb_bulk_unit_parameters;

/* called on completed read data bulk transfer
user_data - user_data in unit parameters structure
status - error code
*/
void cd_usb_bulk_msd_read_callback(void *user_data,
    uint32_t length,
    int status);

/* MSD backend api */
void *cd_usb_bulk_msd_alloc(int max_lun);
void cd_usb_bulk_msd_free(void *device);

/* configure a new Logical Unit to be represented by the device
returns: error code
*/
int cd_usb_bulk_msd_realize(void *device,
    const cd_usb_bulk_unit_parameters *params);

/* detach a Logical Unit
returns: error code
*/
int cd_usb_bulk_msd_unrealize(void *device, uint32_t lun);

/* reset the device instance; cancel all IO ops, reset state
returns: error code
*/
int cd_usb_bulk_msd_reset(void *device);


/* perform a write data bulk transfer
data_len - length of available data to write
returns: error code
*/
int cd_usb_bulk_msd_write(void *device,
    uint8_t *buf, uint32_t data_len);

/* perform a read data bulk transfer
max_len - length of available buffer to fill
If data available immediately, should call cd_usb_bulk_msd_read_callback()
returns: 0 - success, -1 - error
*/
int cd_usb_bulk_msd_read(void *device,
    uint8_t *buf, uint32_t max_len);

/* cancels pending read data bulk transfer
returns: error code
*/
int cd_usb_bulk_msd_cancel_read(void *device);

#endif /* __CD_USB_BULK_MSD_H__ */

