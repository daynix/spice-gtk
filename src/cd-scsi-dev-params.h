/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   CD SCSI device parameters

   Copyright (C) 2018 Red Hat, Inc.

   Red Hat Authors:
   Alexander Nezhinsky<anezhins@redhat.com>

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

#include <gio/gio.h>

typedef struct CdScsiDeviceParameters {
    const char *vendor;
    const char *product;
    const char *version;
    const char *serial;
} CdScsiDeviceParameters;

typedef struct CdScsiDeviceInfo {
    CdScsiDeviceParameters parameters;
    uint32_t started    : 1;
    uint32_t locked     : 1;
    uint32_t loaded     : 1;
} CdScsiDeviceInfo;

typedef struct CdScsiMediaParameters {
    GFileInputStream *stream;
    uint64_t size;
    uint32_t block_size;
} CdScsiMediaParameters;
