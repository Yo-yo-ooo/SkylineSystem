#pragma once

#ifndef _VSDEV_H
#define _VSDEV_H

#include <klib/klib.h>
#include <conf.h>

typedef enum VsDevType
{
    SATA = 0,
    IDE, // SATAPI/IDE
    NVME,
    SAS,
    Undefined
}VsDevType;

typedef struct SALOPS{ //存储器抽象层
    u8 (*Read)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 (*Write)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);

    //可选 Can Select
    u8 (*ReadBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    u8 (*WriteBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    uint32_t (*GetMaxSectorCount)(void*);
}SALOPS;

typedef struct VsDevList{
    atomic_lock lock;
    VsDevType type;
    SALOPS ops;
    char* Name;
    u64 MaxSectorCount;    //按照SectorSize计
    u64 SectorSize;
    bool buf; //Y/N class
    void* classp; //class pointer
}VDL;

typedef struct VsDevList VsDevInfo;


namespace VsDev
{

    extern VsDevList DevList[MAX_VSDEV_COUNT];
    extern uint32_t vsdev_list_idx;

    constexpr u8 RW_OK = 1;
    constexpr u8 RW_ERROR = 0;

    void Init();

    char* TypeToString(VsDevType type);
    void AddStorageDevice(VsDevType type,SALOPS ops,
        u32 SectorCount = 0,void* Class = nullptr);
    /*获取存储器的相关信息*/
    VsDevInfo GetSDEV(u32 idx);
    /*获取该类型存储设备下数量*/
    u32 GetSDEVTCount(VsDevType type); 
    void SetSDev(u32 idx);

    u8 Read(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 Write(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 ReadBytes(uint64_t address, uint32_t Count, void* Buffer);
    u8 WriteBytes(uint64_t address, uint32_t Count, void* Buffer);
};


#endif