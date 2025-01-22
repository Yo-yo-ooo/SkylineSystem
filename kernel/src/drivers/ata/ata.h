#pragma once

#ifndef _ATA_H
#define _ATA_H

#include "../../klib/klib.h"
#include "../../klib/kio.h"
#include "../../acpi/acpi.h"

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

#endif