#pragma once

#ifndef _DEV_H
#define _DEV_H

#include <klib/klib.h>
#include <conf.h>
#include <fs/vfs.h>

typedef enum VsDevType
{
    SATA = 0,
    IDE, // SATAPI/IDE
    NVME,
    SAS,
    Undefined,
    NSDEV //Not Storage Device
}VsDevType;

typedef struct DevOPS{ //存储器抽象层
    u8 (*Read)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 (*Write)(void*,uint64_t lba, uint32_t SectorCount, void* Buffer);

    //可选 Can Select
    u8 (*ReadBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    u8 (*WriteBytes)(void*,uint64_t address, uint32_t Count, void* Buffer);
    uint32_t (*GetMaxSectorCount)(void*);

}DevOPS;

typedef struct DevList{
    spinlock_t lock;
    VsDevType type;
    DevOPS ops;
    char* Name;
    u64 MaxSectorCount;    //按照SectorSize计
    u64 SectorSize;
    bool buf; //Y/N class
    void* classp; //class pointer
}VDL;

typedef struct DevList VsDevInfo;


namespace Dev
{

    extern VDL DevList_[MAX_VSDEV_COUNT];
    extern uint32_t vsdev_list_idx;

    constexpr u8 RW_OK = 1;
    constexpr u8 RW_ERROR = 0;

    void Init();

    char* TypeToString(VsDevType type);
    void AddStorageDevice(VsDevType type,DevOPS ops,
        u32 SectorCount = 0,void* Class = nullptr);
    /*获取存储器的相关信息*/
    VsDevInfo GetSDEV(u32 idx);

    void SetSDev(u32 idx);

    u8 Read(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 Write(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 ReadBytes(uint64_t address, uint32_t Count, void* Buffer);
    u8 WriteBytes(uint64_t address, uint32_t Count, void* Buffer);

    void LookUp(vnode_t *node, const char *name);
    size_t special_read(struct vnode_t *Node, uint8_t *buffer, size_t off, size_t len);
    size_t special_write(struct vnode_t *Node, uint8_t *buffer, size_t off, size_t len);

};


#endif