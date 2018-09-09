/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
Copyright (C) 2018 Red Hat, Inc.

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

#include "config.h"
#include <glib-object.h>

#ifdef G_OS_WIN32
#ifdef USE_USBREDIR

#include <inttypes.h>
#include <gio/gio.h>
#include <windows.h>
#include <ntddcdrm.h>
#include <ntddmmc.h>
#include "cd-device.h"
#include "spice-client.h"

static gboolean is_device_name(const char *filename)
{
    gboolean b = strlen(filename) == 2 && filename[1] == ':';
    return b;
}

static HANDLE open_file(const char *filename)
{
    HANDLE h = CreateFileA(
        filename,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        0,
        NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = NULL;
    }
    return h;
}

static uint32_t ioctl_out(HANDLE h, uint32_t code, void *out_buffer, uint32_t out_size)
{
    uint32_t error;
    DWORD ret;
    BOOL b = DeviceIoControl(h,
        code,
        NULL,
        0,
        out_buffer,
        out_size,
        &ret,
        NULL);
        error = b ? 0 : GetLastError();
    return error;
}

static uint32_t ioctl_none(HANDLE h, uint32_t code)
{
    return ioctl_out(h, code, NULL, 0);
}

static gboolean check_device(HANDLE h)
{
    GET_CONFIGURATION_IOCTL_INPUT cfgIn = { FeatureCdRead, SCSI_GET_CONFIGURATION_REQUEST_TYPE_ALL };
    DWORD ret;
    GET_CONFIGURATION_HEADER cfgOut;
    return DeviceIoControl(h, IOCTL_CDROM_GET_CONFIGURATION,
        &cfgIn, sizeof(cfgIn), &cfgOut, sizeof(cfgOut),
        &ret, NULL);
}

int cd_device_open_stream(SpiceCdLU *unit, const char *filename)
{
    int error = 0;
    HANDLE h;
    unit->device = 0;
    if (!unit->filename && !filename) {
        SPICE_DEBUG("%s: unnamed file", __FUNCTION__);
        return -1; // TODO
    }
    if (unit->filename && filename) {
        g_free(unit->filename);
        unit->filename = NULL;
    }
    if (!filename) {
        // reopening the stream on existing file name
    } else if (is_device_name(filename)) {
        unit->filename = g_strdup_printf("\\\\.\\%s", filename);
    } else {
        unit->filename = g_strdup(filename);
    }
    h = open_file(unit->filename);
    if (h) {
        LARGE_INTEGER size = { 0 };
        if (!GetFileSizeEx(h, &size)) {
            uint64_t buffer[256];
            unit->device = check_device(h);
            SPICE_DEBUG("%s: CD device %srecognized on %s",
                __FUNCTION__, unit->device ? "" : "NOT ", unit->filename);
            uint32_t res = ioctl_out(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                buffer, sizeof(buffer));
            if (!res)
            {
                DISK_GEOMETRY_EX *pg = (DISK_GEOMETRY_EX *)buffer;
                unit->blockSize = pg->Geometry.BytesPerSector;
                size = pg->DiskSize;
            } else {
                SPICE_DEBUG("%s: can't obtain size of %s (error %u)",
                    __FUNCTION__, unit->filename, res);
            }
        }
        unit->size = size.QuadPart;
        CloseHandle(h);
        if (unit->size) {
            unit->file_object = g_file_new_for_path(unit->filename);
            unit->stream = g_file_read(unit->file_object, NULL, NULL);
        }
        if (!unit->stream) {
            SPICE_DEBUG("%s: can't open stream on %s", __FUNCTION__, unit->filename);
            g_object_unref(unit->file_object);
            unit->file_object = NULL;
            error = -1; //TODO
        }
    } else {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, unit->filename);
        error = -1; //TODO
    }
    return error;
}

int cd_device_load(SpiceCdLU *unit, gboolean load)
{
    int error = 0;
    HANDLE h;
    if (!unit->device || !unit->filename) {
        return -1; //TODO
    }
    h = open_file(unit->filename);
    if (h) {
        uint32_t res = ioctl_none(h, load ? IOCTL_STORAGE_LOAD_MEDIA : IOCTL_STORAGE_EJECT_MEDIA);
        if (res) {
            SPICE_DEBUG("%s: can't %sload %s, win error %u",
                __FUNCTION__, load ? "" : "un", unit->filename, res);
            error = -1; //TODO
        } else {
            SPICE_DEBUG("%s: device %s [%s]",
                __FUNCTION__, load ? "loaded" : "ejected", unit->filename);
        }
        CloseHandle(h);
    }
    return error;
}

int cd_device_check(SpiceCdLU *unit)
{
    int error = 0;
    CDROM_DISK_DATA data;
    HANDLE h;
    if (!unit->device || !unit->filename) {
        return -1; //TODO
    }
    h = open_file(unit->filename);
    if (h) {
        uint32_t res = ioctl_none(h, IOCTL_STORAGE_CHECK_VERIFY);
        if (!res) {
            res = ioctl_out(h, IOCTL_CDROM_DISK_TYPE, &data, sizeof(data));
        }
        if (res != 0 || data.DiskData != CDROM_DISK_DATA_TRACK) {
            error = -1; //TODO
        }
        CloseHandle(h);
    }
    return error;
}

#endif
#endif
