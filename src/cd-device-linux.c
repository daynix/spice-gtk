#include "config.h"

#include <glib-object.h>

#ifndef G_OS_WIN32 
#ifdef USE_USBREDIR
#include <inttypes.h>
#include <gio/gio.h>
#include "cd-device.h"
#include "spice-client.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

int device_cd_open_stream(SpiceCdLU *unit, const char *filename)
{
    int error = 0;
    int fd = open(
        filename,
        O_RDONLY | O_NONBLOCK);
    if (fd > 0) {
        struct stat file_stat;
        if (fstat(fd, &file_stat) || file_stat.st_size == 0) {
            file_stat.st_size = 0;
            ioctl(fd, BLKGETSIZE64, &file_stat.st_size);
            ioctl(fd, BLKSSZGET, &unit->blockSize);
        }
        unit->size = file_stat.st_size;
        if (unit->filename) {
            g_free(unit->filename);
        }
        close(fd);
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
