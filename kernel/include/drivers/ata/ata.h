/*
* SPDX-License-Identifier: GPL-2.0-only
* File: ata.h
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

#ifndef _ATA_H
#define _ATA_H

#ifdef __x86_64__
#include <klib/kio.h>
#include <klib/klib.h>
#include <acpi/acpi.h>

#define ATA_PRIMARY 0x1F0
#define ATA_SECONDARY 0x170

#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_CTRL 0x376

#define ATA_MASTER 0xA0
#define ATA_SLAVE 0xB0

#define ATA_WAIT 0x00
#define ATA_IDENTIFY 0xEC
#define ATA_READ 0x20
#define ATA_WRITE 0x30

extern char ata_name[40];

#define ATA_OKAY 0
#define ATA_DISK_NOT_IDENTIFIED 1
#define ATA_DISK_ERR 2
#define ATA_DISK_NOT_READY 3

namespace ATA{
    u8 Init();

    /*Do not use it*/ 
    u8 FRegVsDEV_R(uint64_t lba, uint32_t SectorCount, void* Buffer);
    /*Do not use it*/ 
    u8 FRegVsDEV_W(uint64_t lba, uint32_t SectorCount, void* Buffer);
    
    u8 Read(u32 lba, u8* buffer, u32 sector_count);
    u8 Write(u32 lba, u8* buffer, u32 sector_count);
}
#else

#endif

#endif