#pragma once

#ifndef _IDENTFAT_H
#define _IDENTFAT_H

#include <stdint.h>
#include <stddef.h>

#include <partition/mbrgpt.h>
#include <partition/identfstype.h>

typedef struct FatFsDesc{
    FS_TYPE_ID FSType;
    uint8_t PartitionID;
    uint8_t Prdv;
    uint32_t DriverID;
    uint64_t PStart;
    uint64_t PEnd;
    uint64_t Size;
}FF_DESC;

FS_TYPE IdentifyFat(uint32_t DriverID,uint32_t PartitionID,bool Use_Virt_Image);

#endif