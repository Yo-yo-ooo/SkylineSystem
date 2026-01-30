#pragma once

#ifndef _DEV_H
#define _DEV_H

#ifdef __x86_64__
#include <klib/klib.h>
#else
#include <klib/types.h>
#endif
#include <stdint.h>
#include <conf.h>

typedef enum VsDevType
{
    SATA = 0,
    IDE, // SATAPI/IDE
    NVME,
    SAS,
    Undefined,
    NSDEV, //Not Storage Device
    FrameBuffer,
}VsDevType;

typedef struct DevOPS{ //存储器抽象层
    uint8_t (*Read)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t (*Write)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t (*Read_)(uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t (*Write_)(uint64_t lba, uint32_t SectorCount, void* Buffer);

    //可选 Can Select
    uint8_t (*ReadBytes_)(uint64_t address, uint32_t Count, void* Buffer);
    uint8_t (*WriteBytes_)(uint64_t address, uint32_t Count, void* Buffer);
    uint32_t (*GetMaxSectorCount_)();
    uint8_t (*ReadBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    uint8_t (*WriteBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    uint32_t (*GetMaxSectorCount)(void*);

    uint64_t (*ioctl)(uint64_t cmd, uint64_t arg);
}DevOPS;

typedef struct DevList{
    spinlock_t lock;
    VsDevType type;
    DevOPS ops;
    char* Name;
    uint64_t MaxSectorCount;    //按照SectorSize计
    uint64_t SectorSize;
    void* classp; //class pointer
}VDL;

typedef struct DevList VsDevInfo;


namespace Dev
{
    typedef struct DeviceListNL_t{
        VDL *list; //This Storage Device Specific Information //32
        DeviceListNL_t *Next;
    }DeviceListNL_t;
    typedef struct DeviceList_t{
        DeviceListNL_t *Node; //This Node Storage Type of Device //256
        DeviceList_t *Next;
    }DeviceList_t;   

    extern VDL DevList_[MAX_VSDEV_COUNT];
    extern uint32_t vsdev_list_idx;

    constexpr uint8_t RW_OK = 1;
    constexpr uint8_t RW_ERROR = 0;

    void Init();

    char* TypeToString(VsDevType type);
    void AddStorageDevice(VsDevType type,DevOPS ops,
        uint32_t SectorCount = 0,void* Class = nullptr);
    /*获取存储器的相关信息*/
    VsDevInfo GetSDEV(uint32_t idx);

    void SetSDev(uint32_t idx);

    uint8_t Read(uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t Write(uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t ReadBytes(uint64_t address, uint32_t Count, void* Buffer);
    uint8_t WriteBytes(uint64_t address, uint32_t Count, void* Buffer);

    
};


#endif