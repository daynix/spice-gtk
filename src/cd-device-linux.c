#include "config.h"

#include <glib-object.h>

#ifndef G_OS_WIN32 
#ifdef USE_USBREDIR
#include <inttypes.h>
#include <gio/gio.h>
#include "cd-device.h"
#include "spice-client.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/cdrom.h>

int cd_device_open_stream(SpiceCdLU *unit, const char *filename)
{
    int error = 0;
    unit->device = 0;

    if (!unit->filename && !filename) {
        SPICE_DEBUG("%s: file name not provided", __FUNCTION__);
        return -1; // TODO
    }
    if (unit->filename && filename) {
        g_free(unit->filename);
        unit->filename = NULL;
    }
    if (filename) {
        unit->filename = g_strdup(filename);
    }

    int fd = open(unit->filename, O_RDONLY | O_NONBLOCK);
    if (fd > 0) {
        struct stat file_stat = { 0 };
        if (fstat(fd, &file_stat) || file_stat.st_size == 0) {
            file_stat.st_size = 0;
            unit->device = 1;
            if (!ioctl(fd, BLKGETSIZE64, &file_stat.st_size) &&
                !ioctl(fd, BLKSSZGET, &unit->blockSize)) {
            }
        }
        unit->size = file_stat.st_size;
        close(fd);
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
    }
    else {
        SPICE_DEBUG("%s: can't open file %s", __FUNCTION__, unit->filename);
        error = -1; //TODO
    }

    return error;
}

int cd_device_load(SpiceCdLU *unit, gboolean load)
{
    int error;
    if (!unit->device || !unit->filename) {
        return -1; //TODO
    }
    int fd = open(unit->filename, O_RDONLY | O_NONBLOCK);
    if (fd > 0) {
        if (load) {
            error = ioctl(fd, CDROMCLOSETRAY, 0);
        } else {
            error = ioctl(fd, CDROM_LOCKDOOR, 0);
            error = ioctl(fd, CDROMEJECT, 0);
        }
        if (error) {
            // note that ejecting might be available only for root
            // loading might be available also for regular user
            SPICE_DEBUG("%s: can't %sload %s, res %d, errno %d",
                __FUNCTION__, load ? "" : "un", unit->filename, error, errno);
        }
        close(fd);
    } else {
        error = -1; //TODO
    }
    return error;
}

int cd_device_check(SpiceCdLU *unit)
{
    int error;
    if (!unit->device || !unit->filename) {
        return -1; //TODO
    }
    int fd = open(unit->filename, O_RDONLY | O_NONBLOCK);
    if (fd > 0) {
        error = ioctl(fd, CDROM_DRIVE_STATUS, 0);
        error = (error == CDS_DISC_OK) ? 0 : -1;
        if (!error) {
            error = ioctl(fd, CDROM_DISC_STATUS, 0);
            error = (error == CDS_DATA_1) ? 0 : -1;
        }
        close(fd);
    }
    else {
        error = -1; //TODO
    }
    return error;
}

#endif
#endif
