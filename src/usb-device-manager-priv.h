/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2011,2012 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

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

#include "usb-device-manager.h"

G_BEGIN_DECLS

#ifdef USE_USBREDIR
void spice_usb_device_manager_device_error(
    SpiceUsbDeviceManager *manager, SpiceUsbDevice *device, GError *err);

guint16 spice_usb_device_get_busnum(const SpiceUsbDevice *device);
guint8 spice_usb_device_get_devaddr(const SpiceUsbDevice *device);
guint16 spice_usb_device_get_vid(const SpiceUsbDevice *device);
guint16 spice_usb_device_get_pid(const SpiceUsbDevice *device);
gboolean spice_usb_device_is_isochronous(const SpiceUsbDevice *device);

#endif

G_END_DECLS
