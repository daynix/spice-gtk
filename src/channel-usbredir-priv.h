/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2011 Red Hat, Inc.

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

#ifdef USE_USBREDIR

#include "spice-client.h"
#include "usb-backend.h"

G_BEGIN_DECLS

/* Note: this must be called before calling any other functions, and the
   context should not be destroyed before the last device has been
   disconnected */
void spice_usbredir_channel_set_context(SpiceUsbredirChannel *channel,
                                        SpiceUsbBackend      *context);

void spice_usbredir_channel_disconnect_device_async(SpiceUsbredirChannel *channel,
                                                    GCancellable *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer user_data);

gboolean spice_usbredir_channel_disconnect_device_finish(SpiceUsbredirChannel *channel,
                                                         GAsyncResult         *res,
                                                         GError              **err);

/* Note the context must be set, and the channel must be brought up
   (through spice_channel_connect()), before calling this. */
void spice_usbredir_channel_connect_device_async(SpiceUsbredirChannel *channel,
                                                 SpiceUsbDevice *device,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data);
gboolean spice_usbredir_channel_connect_device_finish(
                                        SpiceUsbredirChannel *channel,
                                        GAsyncResult         *res,
                                        GError              **err);

void spice_usbredir_channel_disconnect_device(SpiceUsbredirChannel *channel);

SpiceUsbDevice *spice_usbredir_channel_get_device(SpiceUsbredirChannel *channel);

void spice_usbredir_channel_lock(SpiceUsbredirChannel *channel);

void spice_usbredir_channel_unlock(SpiceUsbredirChannel *channel);

void spice_usbredir_channel_get_guest_filter(
                          SpiceUsbredirChannel               *channel,
                          const struct usbredirfilter_rule  **rules_ret,
                          int                                *rules_count_ret);

/* Callback for USB backend */
int spice_usbredir_write(SpiceUsbredirChannel *channel, uint8_t *data, int count);

G_END_DECLS

#endif /* USE_USBREDIR */
