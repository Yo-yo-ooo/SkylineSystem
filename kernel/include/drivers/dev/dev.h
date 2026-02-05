#pragma once

#ifndef _DEV_H
#define _DEV_H

#ifdef __x86_64__
#include <klib/klib.h>
#include <klib/algorithm/stl/map.h>
#include <klib/algorithm/stl/vector.h>
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
    //Storage Device Must impl this!
    uint8_t (*Read)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t (*Write)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t (*Read_)(uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t (*Write_)(uint64_t lba, uint32_t SectorCount, void* Buffer);

    //Not Storage Device Must impl this!
    uint8_t (*ReadBytes_)(uint64_t address, uint32_t Count, void* Buffer);
    uint8_t (*WriteBytes_)(uint64_t address, uint32_t Count, void* Buffer);
    uint32_t (*GetMaxSectorCount_)();
    uint8_t (*ReadBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    uint8_t (*WriteBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    uint32_t (*GetMaxSectorCount)(void*);

    uint64_t (*MemoryMap)(uint64_t length,uint64_t prot,uint64_t offset,uint64_t VADDR);
    
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
    uint64_t DescBaseAddr;
    uint32_t DescLength;
}VDL;

typedef struct DevList VsDevInfo;


namespace Dev{

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

    void AddDevice(VDL DeviceInfo,VsDevType DeviceType,DevOPS OPS);
    VDL FindDevice(VsDevType DeviceType,uint32_t DeviceIndex);
    uint8_t DeviceRead(VsDevType DeviceType,uint32_t DevIDX,size_t offset,void* Buffer,size_t nbytes);
    uint8_t DeviceWrite(VsDevType DeviceType,uint32_t DevIDX,size_t offset,void* Buffer,size_t nbytes);
    uint64_t DeviceMemoryMap(VsDevType DeviceType,
        uint32_t DevIDX,
        uint64_t length,uint64_t prot,
        uint64_t offset,uint64_t VADDR
    );

    extern EasySTL::Map<VsDevType,EasySTL::vector<VDL>> DeviceInfos;
    extern EasySTL::Map<VsDevType,uint64_t  /*Storage DevOPS<--Pointer*/> Type2DeviceOPS;
};


#endif