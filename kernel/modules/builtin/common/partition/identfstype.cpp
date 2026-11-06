//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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