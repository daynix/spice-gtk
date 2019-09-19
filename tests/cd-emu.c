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
#include <gio/gio.h>

#include "usb-device-cd.h"
#include "usb-emulation.h"

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

    int ret =  g_test_run();

    unlink(TEST_CD_ISO_FILE);
    return ret;
}
