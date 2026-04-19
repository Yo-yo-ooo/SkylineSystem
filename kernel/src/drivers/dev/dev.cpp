/*
* SPDX-License-Identifier: GPL-2.0-only
* File: dev.cpp
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
#ifdef __x86_64__
#include <drivers/dev/dev.h>
#include <klib/algorithm/hmap.h>
struct DevManKey {
    VsDevType type;
    uint32_t index;
};

typedef struct DevManEntry {
    DevManKey key;
    VDL *dev;
} DevManEntry;

struct DevStrSearch{
    char *name;
    VDL *dev;
};

// --- 全局状态 ---
static spinlock_t dev_manager_lock = 0;

// --- Hashmap 辅助函数 ---
int devm_compare(const void* a, const void *b, void *udata) {
    const DevManEntry* ea = (const DevManEntry*)a;
    const DevManEntry* eb = (const DevManEntry*)b;
    /* if(ea->key.type != eb->key.type) return ea->key.type < eb->key.type ? -1 : 1;
    if(ea->key.index != eb->key.index) return ea->key.index < eb->key.index ? -1 : 1;
    return 0; */
    return _memcmp(&ea->key, &eb->key, sizeof(DevManKey));
}
uint64_t devm_hash(const void* item, uint64_t seed0, uint64_t seed1) {
    const DevManEntry* entry = (const DevManEntry*)item;
    return hashmap_sip(&entry->key, sizeof(DevManKey), seed0, seed1);
}
//Device type and it's index manager

int devtim_compare(const void *a,const void*b,void* udata){
    const DevManKey* ea = (const DevManKey*)a;
    const DevManKey* eb = (const DevManKey*)b;
    return ea->index < eb->index ? -1 : (ea->index > eb->index ? 1 : 0);
}
uint64_t devtim_hash(const void* item, uint64_t seed0, uint64_t seed1){
    const DevManKey* entry = (const DevManKey*)item;
    return hashmap_sip(&entry->type, sizeof(DevManKey), seed0, seed1);
}

int devstrs_compare(const void* a,const void* b,void* udata){
    const DevStrSearch* ea = (const DevStrSearch*)a;
    const DevStrSearch* eb = (const DevStrSearch*)b;
    return strcmp(ea->name, eb->name);
}

uint64_t devstrs_hash(const void* item, uint64_t seed0, uint64_t seed1){
    const DevStrSearch* entry = (const DevStrSearch*)item;
    return hashmap_sip(entry->name, strlen(entry->name), seed0, seed1);
}

static volatile hashmap* TIMap = nullptr;
static volatile hashmap* StrMap = nullptr;
static volatile hashmap* DevMan_Map = nullptr;
namespace Dev{
    VDL ThisDev = {0};
    uint32_t ThisDevType = Undefined;
    uint32_t ThisDevIDX = 0;
    //DevManEntry* ThisEntry = nullptr;

    void AddStorageDevice(VsDevType type, DevOPS ops, uint32_t SectorCount = 0, void* Class = nullptr) {
        if(type > MAX_TYPE_C) return;

        // 先分配好内存，减少在锁内部滞留的时间
        VDL *DeviceInfo = (VDL*)kmalloc(sizeof(VDL));
        if (!DeviceInfo) return; 

        spinlock_lock(&dev_manager_lock); // 开启大锁，保证 Index 和 Map 的原子同步

        // 1. 获取并更新索引
        DevManKey key_lookup = {.type = type};
        DevManKey *p = (DevManKey*)hashmap_get(TIMap, &key_lookup);
        uint32_t TypeIndex = p ? p->index : 0;
        
        DevManKey next_key = {.type = type, .index = TypeIndex + 1};
        hashmap_set(TIMap, &next_key);

        // 2. 初始化设备信息
        DeviceInfo->idx = TypeIndex;
        DeviceInfo->type = type;
        DeviceInfo->classp = Class;
        DeviceInfo->MaxSectorCount = SectorCount;
        DeviceInfo->Name = StrCombine(TypeToString(type), to_string((uint64_t)TypeIndex));
        
        // 使用内核级拷贝，确保 ops 完整
        __memcpy(&DeviceInfo->ops, &ops, sizeof(DevOPS));

        // 3. 录入全局查找表
        hashmap_set(StrMap, &(DevStrSearch){.name = DeviceInfo->Name, .dev = DeviceInfo});
        
        DevManEntry entry = {
            .key = {.type = type, .index = TypeIndex}, 
            .dev = DeviceInfo
        };
        hashmap_set(DevMan_Map, &entry);

        spinlock_unlock(&dev_manager_lock); // 释放锁

        debugpln("[AddStorageDevice] Registered: %s", DeviceInfo->Name);
    }



    void SetSDev(VsDevType type, u32 idx){
        ThisDevType = type;
        ThisDevIDX = idx;
        DevManKey key = {.type = type, .index = idx};
        spinlock_lock(&dev_manager_lock);
        DevManEntry* ThisEntry = (DevManEntry*)hashmap_get(DevMan_Map, &key);
        spinlock_unlock(&dev_manager_lock);
        ThisDev = *ThisEntry->dev;
    }


    VDL* GetSDEV(const char *Name){
        spinlock_lock(&dev_manager_lock);
        DevStrSearch* search = (DevStrSearch*)hashmap_get(StrMap, &(DevStrSearch){.name = Name});
        spinlock_unlock(&dev_manager_lock);
        if(search)
            return search->dev;
        return nullptr;
    }

    VDL* GetSDEV(VsDevType Type, uint32_t idx){
        DevManKey key = {.type = Type, .index = idx};
        spinlock_lock(&dev_manager_lock);
        DevManEntry* ThisEntry = (DevManEntry*)hashmap_get(DevMan_Map, &(DevManEntry){.key = key});
        spinlock_unlock(&dev_manager_lock);
        if(ThisEntry)
            return ThisEntry->dev;
        return nullptr;
    }


    u8 Read(uint64_t lba, uint32_t SectorCount, void* Buffer){
        if(ThisDev.type != VsDevType::Undefined)
            return ThisDev.ops.Read(ThisDev.classp,lba, SectorCount, Buffer);
        else
            return false;
    }

    u8 Write(uint64_t lba, uint32_t SectorCount, void* Buffer){
        if(ThisDev.type != VsDevType::Undefined)
            return ThisDev.ops.Write(ThisDev.classp,lba, SectorCount, Buffer);
        else
            return false;
    }

    u8 ReadBytes(uint64_t address, uint32_t Count, void* Buffer){
        if(ThisDev.type != VsDevType::Undefined){
            if(ThisDev.ops.ReadBytes != nullptr)
                return ThisDev.ops.ReadBytes(ThisDev.classp,address, Count, Buffer);
            else {
                if (Count == 0)
                    return true;
                if (address + Count > ThisDev.MaxSectorCount * 512)
                    return false;
                
                uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
                uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);//"Malloc for Read Buffer"
                _memset(buffer2, 0, tempSectorCount * 512);

                if (!ThisDev.ops.Read(ThisDev.classp,(address / 512), tempSectorCount, buffer2))
                {
                    uint16_t offset = address % 512;
                    for (uint64_t i = 0; i < Count; i++)
                        ((uint8_t*)Buffer)[i] = buffer2[i + offset];

                    kfree(buffer2);
                    
                    return false;
                }

                uint16_t offset = address % 512;
                for (uint64_t i = 0; i < Count; i++)
                    ((uint8_t*)Buffer)[i] = buffer2[i + offset];
                        
                kfree(buffer2);
                
                return true;
            }
        }else{
            return false;
        }
    }

    u8 WriteBytes(uint64_t address, uint32_t Count, void* Buffer){
        if(ThisDev.type != VsDevType::Undefined){
            if(ThisDev.ops.WriteBytes != nullptr)
                return ThisDev.ops.WriteBytes(ThisDev.classp,address, Count, Buffer);
            else{
                if (Count == 0)
                    return true;
                if (address + Count > ThisDev.MaxSectorCount * 512)
                    return false;
                
                uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
                uint8_t* buffer2 = (uint8_t*)kmalloc(512); //Malloc for Write Buffer
                
                if (tempSectorCount == 1)
                {
                    _memset(buffer2, 0, 512);
                    if (!ThisDev.ops.Read(ThisDev.classp,(address / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }

                    uint16_t offset = address % 512;
                    for (uint64_t i = 0; i < Count; i++)
                        buffer2[i + offset] = ((uint8_t*)Buffer)[i];

                    if (!ThisDev.ops.Write(ThisDev.classp,(address / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }
                    
                }
                else
                {
                    uint64_t newAddr = address;
                    uint64_t newCount = Count;
                    uint64_t addrOffset = 0;
                    {

                        _memset(buffer2, 0, 512);
                        if (!ThisDev.ops.Read(ThisDev.classp,(address / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }

                        uint16_t offset = address % 512;
                        uint16_t specialCount = 512 - offset;
                        addrOffset = specialCount;
                        newAddr = address + specialCount;
                        newCount = Count - specialCount;

                        for (uint64_t i = 0; i < specialCount; i++)
                            buffer2[i + offset] = ((uint8_t*)Buffer)[i];

                        if (!ThisDev.ops.Write(ThisDev.classp,(address / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }
                    }
                    {
                        _memset(buffer2, 0, 512);
                        if (!ThisDev.ops.Read(ThisDev.classp,((address + Count) / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }

                        uint16_t specialCount = ((address + Count) % 512);
                        newCount -= specialCount;

                        uint64_t blehus = (Count - specialCount);

                        for (int64_t i = 0; i < specialCount; i++)
                            buffer2[i] = ((uint8_t*)Buffer)[i + blehus];

                        if (!ThisDev.ops.Write(ThisDev.classp,((address + Count) / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }
                    }
                    {
                        uint64_t newSectorCount = newCount / 512;
                        if (newSectorCount != 0)
                        {
                            uint64_t newSectorStartId = newAddr / 512;

                            if (!ThisDev.ops.Write(ThisDev.classp,newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset)))
                            {
                                kfree(buffer2);
                                
                                return false;
                            }
                        }
                    }


                    
                }
                kfree(buffer2);
                
                return true;
            }
        }else{
            return false;
        }
    }

    const char* TypeToString(VsDevType type){
        switch(type){
            case SATA:return "sata";
            case IDE:return "ide";
            case NVME:return "nvme";
            case SAS:return "sas";
            case Undefined:return "UNDEF";
        }
    }

    void Init(){
        DevMan_Map = hashmap_new(sizeof(DevManEntry), 64, 0, 0, devm_hash, devm_compare, nullptr, nullptr);
        TIMap = hashmap_new(sizeof(DevManKey), 128,0, 0, devtim_hash, devtim_compare, nullptr, nullptr);
        StrMap = hashmap_new(sizeof(DevStrSearch), 128,0, 0, devstrs_hash, devstrs_compare, nullptr, nullptr);
    }
}
#endif