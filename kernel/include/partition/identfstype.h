/*
* SPDX-License-Identifier: GPL-2.0-only
* File: identfstype.h
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
#pragma once

#include <partition/mbrgpt.h>

#define PARTITION_TYPE_UNKNOWN      0
#define PARTITION_TYPE_EXT4         1
#define PARTITION_TYPE_EXT3         2
#define PARTITION_TYPE_EXT2         3
#define PARTITION_TYPE_FAT32        4
#define PARTITION_TYPE_FAT16        5
#define PARTITION_TYPE_FAT12        6
#define PARTITION_TYPE_EXFAT        7

typedef uint8_t FS_TYPE_ID;

/*
ErrorCode:
0: [MBR/GPT]    All is OK!
1: [MBR]        Can't MBR Partitions Count 
2: [MBR]        Can't Get Partition Start ADDR
3: [GPT]        Can't Get GPT Partitions Count 
4: [GPT]        Can't Get Partition Start ADDR
5: [FS]         Read Partition Start Addr ERROR
6: [FS]         Read Partition Base INFO ERROR
7: [FS]         Check FS Feature ERROR
*/
typedef struct FS_TYPE_{           
    uint8_t TypeID;     //See Line 5 ~ Line 12
    uint8_t ErrorCode;
}FS_TYPE;

FS_TYPE_ID IdentifyFSType(uint32_t DriverID,uint32_t PartitionID);

bool FSAllIdentify();
void FSPrintDesc();