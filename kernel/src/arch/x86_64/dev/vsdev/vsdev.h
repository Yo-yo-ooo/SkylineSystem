#pragma once

#ifndef _VSDEV_H
#define _VSDEV_H

#include "../../../../klib/klib.h"
#include "../../../../conf.h"

typedef enum VsDevType
{
    SATA = 0,
    SATAPI,
    ATA,
    NVME,
    SAS,
    Undefined
}VsDevType;

typedef struct SALOPS{ //存储器抽象层
    u8 (*Read)(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 (*Write)(uint64_t lba, uint32_t SectorCount, void* Buffer);

    //可选 Can Select
    u8 (*ReadBytes)(uint64_t lba, uint32_t Count, void* Buffer);
    u8 (*WriteBytes)(uint64_t lba, uint32_t Count, void* Buffer);
}SALOPS;

typedef struct VsDevList{
    VsDevType type;
    SALOPS* ops;
}VDL;

typedef struct VsDevInfo {
    VsDevList vsdevl_info;
    u64 MaxSectorCount;    //按照SectorSize计
    u64 SectorSize;
} VsDevInfo;


class VsDev
{
private:
    VsDevList DevList[MAX_VSDEV_COUNT];
    uint32_t vsdev_list_idx;
public:
    VsDev();
    void AddStorageDevice(VsDevType type,SALOPS* ops);
    /*获取存储器的相关信息*/
    VsDevInfo GetSDEV(u32 idx);
    /*获取该类型存储设备下数量*/
    u32 GetSDEVTCount(VsDevType type); 
    ~VsDev();
};


#endif