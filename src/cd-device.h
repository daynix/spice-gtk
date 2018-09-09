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

#ifndef __CD_DEVICE_H__
#define __CD_DEVICE_H__

typedef struct _SpiceCdLU
{
    char *filename;
    GFile *file_object;
    GFileInputStream *stream;
    uint64_t size;
    uint32_t blockSize;
    uint32_t loaded : 1;
    uint32_t device : 1;
} SpiceCdLU;

int cd_device_open_stream(SpiceCdLU *unit, const char *filename);
int cd_device_load(SpiceCdLU *unit, gboolean load);
int cd_device_check(SpiceCdLU *unit);

#endif
