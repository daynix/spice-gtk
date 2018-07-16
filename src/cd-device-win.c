#include "config.h"
#include <glib-object.h>

#ifdef G_OS_WIN32 
#ifdef USE_USBREDIR

#include <inttypes.h>
#include <gio/gio.h>
#include <windows.h>
#include "cd-device.h"
#include "spice-client.h"

int device_cd_open_stream(SpiceCdLU *unit, const char *filename)
{
    int error = 0;
    HANDLE h = CreateFileA(
        filename,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = { 0 };
        if (!GetFileSizeEx(h, &size)) {
            uint64_t buffer[256];
            unsigned long ret;
            if (DeviceIoControl(h,
                IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                NULL,
                0,
                buffer,
                sizeof(buffer),
                &ret,
                NULL))
            {
                DISK_GEOMETRY_EX *pg = (DISK_GEOMETRY_EX *)buffer;
                unit->blockSize = pg->Geometry.BytesPerSector;
                size = pg->DiskSize;
            }
        }
        unit->size = size.QuadPart;
        if (unit->filename) {
            g_free(unit->filename);
        }
        CloseHandle(h);
        unit->filename = g_strdup(filename);
        unit->file_object = g_file_new_for_path(filename);
        unit->stream = g_file_read(unit->file_object, NULL, NULL);
        if (!unit->stream) {
            SPICE_DEBUG("%s: can't open stream on %s", __FUNCTION__, filename);
            g_object_unref(unit->file_object);
            unit->file_object = NULL;
            error = -1; //TODO
        }
    }
    else {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, filename);
        error = -1; //TODO
    }
    return error;
}

int device_cd_load(SpiceCdLU *unit, gboolean load)
{
    SPICE_DEBUG("%s: not implemented", __FUNCTION__);
    return -1;
}

int device_cd_check(SpiceCdLU *unit)
{
    SPICE_DEBUG("%s: not implemented", __FUNCTION__);
    return -1;
}

#endif
#endif
