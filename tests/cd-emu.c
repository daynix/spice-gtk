/*
   Copyright (C) 2019 Red Hat, Inc.

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

/* Mock some code in usb-backend.c
 *
 * We include directly the source we want to modify, this allows to:
 * - access static functions;
 * - access private declarations;
 * - mock (replace) some external function calls.
 * All without changing a single line of code, adding files or similar.
 * This works as the linker prefer object symbols to library symbols;
 * as the symbols here will be in an object and the other version is
 * in a library this will work.
 *
 * The defines before the include allows to replace the external
 * function we need to replace. You can always declare the original
 * function and wrap it in the mock function if needed.
 */
#define spice_usbredir_write mock_spice_usbredir_write
#define spice_channel_get_state mock_spice_channel_get_state
#include "../src/usb-backend.c"

#include "usb-device-cd.h"

static SpiceUsbBackendDevice *device = NULL;

/* simple usb manager hotplug callback emulation. */
static void
test_hotplug_callback(void *user_data, SpiceUsbBackendDevice *dev, gboolean added)
{
    // ignore not emulated devices
    const UsbDeviceInformation *info = spice_usb_backend_device_get_info(dev);
    if (info->bus != BUS_NUMBER_FOR_EMULATED_USB) {
        return;
    }

    if (added) {
        g_assert_null(device);
        device = spice_usb_backend_device_ref(dev);
    } else {
        g_assert_nonnull(device);
        g_assert(device == dev);
        spice_usb_backend_device_unref(dev);
        device = NULL;
    }
}

static void multiple(const void *param)
{
    guint limit = GPOINTER_TO_UINT(param);
    CdEmulationParams params = { "test-cd-emu.iso", 1 };
    GError *err = NULL;
    SpiceUsbBackend * be = spice_usb_backend_new(&err);
    g_assert_nonnull(be);
    g_assert_null(err);
    spice_usb_backend_register_hotplug(be, NULL, test_hotplug_callback, &err);
    g_assert_null(err);
    for (int i = 0; i < limit; i++) {
        // allocate a CD emulation device
        g_assert_true(create_emulated_cd(be, &params, &err));
        g_assert_null(err);
        g_assert_nonnull(device);

        // emulate automatic CD ejection, this should release the
        // object
        spice_usb_backend_device_eject(be, device);
        g_assert_null(device);
    }
    spice_usb_backend_deregister_hotplug(be);
    spice_usb_backend_delete(be);
}

static unsigned int messages_sent = 0;
static unsigned int hellos_sent = 0;
static SpiceUsbBackendChannel *usb_ch;

int
mock_spice_usbredir_write(SpiceUsbredirChannel *channel, uint8_t *data, int count)
{
    messages_sent++;
    g_assert_cmpint(count, >=, 4);
    const uint32_t type = data[0] + data[1] * 0x100u + data[2] * 0x10000u + data[3] * 0x1000000u;
    if (type == usb_redir_hello) {
        hellos_sent++;
    }

    // return we handled the data
    spice_usb_backend_return_write_data(usb_ch, data);
    return count;
}

// channel state to return from Mock function
static enum spice_channel_state ch_state = SPICE_CHANNEL_STATE_UNCONNECTED;

enum spice_channel_state
mock_spice_channel_get_state(SpiceChannel *channel)
{
    return ch_state;
}

// number of GObjects allocated we expect will be freed
static unsigned gobjects_allocated = 0;
static void decrement_allocated(gpointer data G_GNUC_UNUSED, GObject *old_gobject G_GNUC_UNUSED)
{
    g_assert_cmpint(gobjects_allocated, !=, 0);
    gobjects_allocated--;
}

#define DATA_START \
    do { static const uint8_t data[] = {
#define DATA_SEND \
        }; \
        spice_usb_backend_read_guest_data(usb_ch, (uint8_t*)data, G_N_ELEMENTS(data)); \
    } while(0)

static void
device_iteration(const int loop, const bool attach_on_connect)
{
    GError *err = NULL;
    unsigned int hellos_expected, messages_expected;

    hellos_expected = hellos_sent;
    messages_expected = messages_sent;

    if (ch_state == SPICE_CHANNEL_STATE_UNCONNECTED) {
        ch_state = SPICE_CHANNEL_STATE_CONNECTING;
    }
    if (attach_on_connect) {
        g_assert_true(spice_usb_backend_channel_attach(usb_ch, device, &err));
        g_assert_null(err);
        if (ch_state == SPICE_CHANNEL_STATE_READY) {
            hellos_expected = MIN(hellos_expected + 1, 1);
            messages_expected++;
        } else {
            g_assert_cmpint(messages_sent, ==, messages_expected);
        }
    }
    g_assert_cmpint(hellos_sent, ==, hellos_expected);
    g_assert_cmpint(messages_sent, >=, messages_expected);

    // try to get initial data
    if (ch_state == SPICE_CHANNEL_STATE_CONNECTING) {
        ch_state = SPICE_CHANNEL_STATE_READY;
        spice_usb_backend_channel_flush_writes(usb_ch);
        hellos_expected = MIN(hellos_expected + 1, 1);
        messages_expected++;
    }

    // we should get an hello (only one!)
    g_assert_cmpint(hellos_sent, ==, hellos_expected);
    g_assert_cmpint(messages_sent, >=, messages_expected);

    // send hello reply
    if (loop == 0) {
        DATA_START
            0x00,0x00,0x00,0x00,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00, //000 ....D.......
            0x71,0x65,0x6d,0x75,0x20,0x75,0x73,0x62,0x2d,0x72,0x65,0x64, //00c qemu usb-red
            0x69,0x72,0x20,0x67,0x75,0x65,0x73,0x74,0x20,0x33,0x2e,0x30, //018 ir guest 3.0
            0x2e,0x31,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, //024 .1..........
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, //030 ............
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, //03c ............
            0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x00,                     //048 ........
        DATA_SEND;
    }

    if (!attach_on_connect) {
        g_assert_true(spice_usb_backend_channel_attach(usb_ch, device, &err));
        g_assert_null(err);
    }
    g_assert_cmpint(hellos_sent, ==, 1);
    g_assert_cmpint(messages_sent, >, 1);

    spice_usb_backend_channel_detach(usb_ch);
}

static void attach(const void *param)
{
    const bool attach_on_connect = !!GPOINTER_TO_UINT(param);

    hellos_sent = 0;
    messages_sent = 0;
    ch_state = SPICE_CHANNEL_STATE_UNCONNECTED;

    SpiceSession *session = spice_session_new();
    g_assert_nonnull(session);
    g_object_weak_ref(G_OBJECT(session), decrement_allocated, NULL);
    SpiceChannel *ch = spice_channel_new(session, SPICE_CHANNEL_USBREDIR, 0);
    g_assert_nonnull(ch);
    g_object_weak_ref(G_OBJECT(ch), decrement_allocated, NULL);
    gobjects_allocated = 2;

    /*
     * real test, allocate a channel usbredir, emulate device
     * initialization.
     * Filter some call.
     * Start sequence:
     * - spice_usb_backend_new
     * - spice_usb_backend_register_hotplug
     * - spice_usb_backend_create_emulated_device
     * - spice_usb_backend_channel_new
     * - spice_usb_backend_channel_attach (if redir on connect)
     * - spice_usb_backend_channel_flush_writes
     * - spice_usbredir_write_callback
     * - spice_usb_backend_return_write_data
     * - spice_usb_backend_read_guest_data
     * - spice_usb_backend_channel_attach (if not redir on connect)
     */
    GError *err = NULL;
    SpiceUsbBackend * be = spice_usb_backend_new(&err);
    g_assert_nonnull(be);
    g_assert_null(err);
    spice_usb_backend_register_hotplug(be, NULL, test_hotplug_callback, &err);
    g_assert_null(err);

    CdEmulationParams params = { "test-cd-emu.iso", 1 };
    g_assert_true(create_emulated_cd(be, &params, &err));
    g_assert_null(err);
    g_assert_nonnull(device);
    g_assert_false(device->edev_configured);

    usb_ch = spice_usb_backend_channel_new(be, SPICE_USBREDIR_CHANNEL(ch));
    g_assert_nonnull(usb_ch);

    for (int loop = 0; loop < 2; loop++) {
        device_iteration(loop, attach_on_connect);
    }

/*

> to server/guest
< to client from server/guest

> usb_redir_interface_info,
> usb_redir_ep_info,
> usb_redir_device_connect

< usb_redir_reset
< usb_redir_control_packet

> usb_redir_control_packet

*/

    // cleanup
    spice_usb_backend_device_unref(device);
    device = NULL;
    spice_usb_backend_channel_delete(usb_ch);
    usb_ch = NULL;
    spice_usb_backend_deregister_hotplug(be);
    spice_usb_backend_delete(be);

    // this it's the correct sequence to free session!
    // g_object_unref is not enough, causing wrong reference countings
    spice_session_disconnect(session);
    g_object_unref(session);
    while (g_main_context_iteration(NULL, FALSE)) {
        continue;
    }

    g_assert_cmpint(gobjects_allocated, ==, 0);
}

#define TEST_CD_ISO_FILE "test-cd-emu.iso"

static void
write_test_iso(void)
{
    uint8_t sector[2048];
    FILE *f = fopen(TEST_CD_ISO_FILE, "wb");
    g_assert_nonnull(f);
    memset(sector, 0, sizeof(sector));
    strcpy((char*) sector, "sector 0");
    fwrite(sector, sizeof(sector), 1, f);
    fclose(f);
}

int main(int argc, char* argv[])
{
    write_test_iso();

    g_test_init(&argc, &argv, NULL);

    g_test_add_data_func("/cd-emu/simple", GUINT_TO_POINTER(1), multiple);
    g_test_add_data_func("/cd-emu/multiple", GUINT_TO_POINTER(128), multiple);
    g_test_add_data_func("/cd-emu/attach_no_auto", GUINT_TO_POINTER(0), attach);
    g_test_add_data_func("/cd-emu/attach_auto", GUINT_TO_POINTER(1), attach);

    int ret =  g_test_run();

    unlink(TEST_CD_ISO_FILE);
    return ret;
}
