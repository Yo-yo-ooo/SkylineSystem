/*
* SPDX-License-Identifier: GPL-2.0-only
* File: identfstype.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <partition/identfstype.h>
#include <limits.h>
#include <mem/heap.h>
#include <drivers/dev/dev.h>

#include <fs/fatfs/identfat.h>
#include <fs/lwext4/ext4.h>

#include <conf.h>

FS_TYPE_ID IdentifyFSType(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID) {
    FS_TYPE TMP = IdentifyFat(DriverType, DriverID, PartitionID, ENABLE_VIRT_IMAGE);
    if (TMP.ErrorCode == 0 && TMP.TypeID != PARTITION_TYPE_UNKNOWN) 
        return TMP.TypeID;
    TMP = IdentifyExtx(DriverType, DriverID, PartitionID, ENABLE_VIRT_IMAGE);
    if (TMP.ErrorCode == 0 && TMP.TypeID != PARTITION_TYPE_UNKNOWN)
        return TMP.TypeID;
    return PARTITION_TYPE_UNKNOWN;
}