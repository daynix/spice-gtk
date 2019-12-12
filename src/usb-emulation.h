/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
    Copyright (C) 2019 Red Hat, Inc.

    Red Hat Authors:
    Yuri Benditovich<ybendito@redhat.com>

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

#include "usbredirparser.h"
#include "usb-backend.h"

typedef struct SpiceUsbEmulatedDevice SpiceUsbEmulatedDevice;
typedef SpiceUsbEmulatedDevice*
(*SpiceUsbEmulatedDeviceCreate)(SpiceUsbBackend *be,
                                SpiceUsbDevice *parent,
                                void *create_params,
                                GError **err);

/*
    function table for emulated USB device
    must be first member of device structure
    all functions are mandatory for implementation
*/
typedef struct UsbDeviceOps {
    gboolean (*get_descriptor)(SpiceUsbEmulatedDevice *device,
                               uint8_t type, uint8_t index,
                               void **buffer, uint16_t *size);
    gchar * (*get_product_description)(SpiceUsbEmulatedDevice *device);
    void (*attach)(SpiceUsbEmulatedDevice *device, struct usbredirparser *parser);
    void (*reset)(SpiceUsbEmulatedDevice *device);
    /*
        processing is synchronous, default = stall:
        - return success without data: set status to 0
        - return error - set status to error
        - return success with data - set status to 0,
                                    set buffer to some buffer
                                    set length to out len
                                    truncation is automatic
    */
    void (*control_request)(SpiceUsbEmulatedDevice *device,
                            uint8_t *data, int data_len,
                            struct usb_redir_control_packet_header *h,
                            void **buffer);
    /*
        processing is synchronous:
        - set h->status to resulting status, default = stall
    */
    void (*bulk_out_request)(SpiceUsbEmulatedDevice *device,
                             uint8_t ep, uint8_t *data, int data_len,
                             uint8_t *status);
    /*
        if returns true, processing is asynchronous
        otherwise header contains error status
    */
    gboolean (*bulk_in_request)(SpiceUsbEmulatedDevice *device, uint64_t id,
                            struct usb_redir_bulk_packet_header *bulk_header);
    void (*cancel_request)(SpiceUsbEmulatedDevice *device, uint64_t id);
    void (*detach)(SpiceUsbEmulatedDevice *device);
    void (*unrealize)(SpiceUsbEmulatedDevice *device);
} UsbDeviceOps;

static inline const UsbDeviceOps *device_ops(SpiceUsbEmulatedDevice *dev)
{
    return (const UsbDeviceOps *)dev;
}

gboolean
spice_usb_backend_create_emulated_device(SpiceUsbBackend *be,
                                         SpiceUsbEmulatedDeviceCreate create_proc,
                                         void *create_params,
                                         GError **err);
