//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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
    /* P1-39: 与 devtim_hash 同字段 (整个 key) —— 原实现只比 index,
       不同 type 同 index 的条目在桶内被判相等 → 多盘重名覆盖首盘 */
    return _memcmp(ea, eb, sizeof(DevManKey));
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

static hashmap* TIMap = nullptr;
static hashmap* StrMap = nullptr;
static hashmap* DevMan_Map = nullptr;
namespace Dev{

    void AddStorageDevice(VsDevType type, DevOPS ops, uint64_t SectorCount, void* Class) {
        if(type > MAX_TYPE_C) return;

        // 先分配好内存，减少在锁内部滞留的时间
        VDL *DeviceInfo = (VDL*)kmalloc(sizeof(VDL));
        if (!DeviceInfo) return; 

        spinlock_lock(&dev_manager_lock); // 开启大锁，保证 Index 和 Map 的原子同步

        // 1. 获取并更新索引 (P1-39: 原实现查 {type,0} 恒得首条 → 之后
        //    全部撞 index 1 覆盖; 现遍历取该 type 的最大 index + 1)
        uint32_t TypeIndex = 0;
        {
            size_t it = 0; void *item;
            while (hashmap_iter(TIMap, &it, &item)) {
                const DevManKey *k = (const DevManKey*)item;
                if (k->type == type && k->index >= TypeIndex) TypeIndex = k->index + 1;
            }
        }
        
        DevManKey next_key = {.type = type, .index = TypeIndex};
        hashmap_set(TIMap, &next_key);

        // 2. 初始化设备信息 (idx = 本设备序号 = TypeIndex)
        DeviceInfo->idx = TypeIndex;
        DeviceInfo->type = type;
        DeviceInfo->classp = Class;
        DeviceInfo->MaxSectorCount = SectorCount;
        DeviceInfo->Name = StrCombine(TypeToString(type), to_string((uint64_t)TypeIndex));
        
        // 使用内核级拷贝，确保 ops 完整
        __memcpy(&DeviceInfo->ops, &ops, sizeof(DevOPS));

        // 3. 录入全局查找表
        DevStrSearch str_key = {}; str_key.name = DeviceInfo->Name; str_key.dev = DeviceInfo;
        hashmap_set(StrMap, &str_key); /* 修复: 复合字面量改命名局部变量 */
        
        DevManEntry entry = {
            .key = {.type = type, .index = TypeIndex}, 
            .dev = DeviceInfo
        };
        hashmap_set(DevMan_Map, &entry);

        spinlock_unlock(&dev_manager_lock); // 释放锁

        kinfo("[dev] registered %s (%u sectors)", DeviceInfo->Name, SectorCount);
    }



    VDL* GetSDEV(const char *Name){
        /* 修复: 原 &(DevStrSearch){...} 是 C 复合字面量取地址,
           C++ 下为临时对象, 生命周期不保(clang 报错) */
        DevStrSearch key = {}; key.name = (char*)Name;
        spinlock_lock(&dev_manager_lock);
        DevStrSearch* search = (DevStrSearch*)hashmap_get(StrMap, &key);
        spinlock_unlock(&dev_manager_lock);
        if(search)
            return search->dev;
        return nullptr;
    }

    VDL* GetSDEV(VsDevType Type, uint32_t idx){
        DevManKey key = {.type = Type, .index = idx};
        DevManEntry lookup = {}; lookup.key = key; /* 修复: 复合字面量改命名局部变量 */
        spinlock_lock(&dev_manager_lock);
        DevManEntry* ThisEntry = (DevManEntry*)hashmap_get(DevMan_Map, &lookup);
        spinlock_unlock(&dev_manager_lock);
        if(ThisEntry)
            return ThisEntry->dev;
        return nullptr;
    }


    u8 Read(VDL* dev, uint64_t lba, uint32_t SectorCount, void* Buffer){
        if(dev && dev->type != VsDevType::Undefined && dev->ops.Read)
            return dev->ops.Read(dev->classp,lba, SectorCount, Buffer);
        else
            return false;
    }

    u8 Write(VDL* dev, uint64_t lba, uint32_t SectorCount, void* Buffer){
        if(dev && dev->type != VsDevType::Undefined && dev->ops.Write)
            return dev->ops.Write(dev->classp,lba, SectorCount, Buffer);
        else
            return false;
    }

    u8 ReadBytes(VDL* dev, uint64_t address, uint32_t Count, void* Buffer){
        if(dev && dev->type != VsDevType::Undefined){
            if(dev->ops.ReadBytes != nullptr)
                return dev->ops.ReadBytes(dev->classp,address, Count, Buffer);
            else {
                if (Count == 0)
                    return true;
                /* P1-37: 边界收紧 —— 最后扇区必须 < 容量 */
                if (address + Count - 1 >= dev->MaxSectorCount * 512)
                    return false;
                
                uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
                uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);//"Malloc for Read Buffer"
                if (!buffer2) return false;   /* OOM 显式失败 */
                _memset(buffer2, 0, tempSectorCount * 512);

                /* P1-38: 读失败不得拷贝未填充缓冲 (原实现污染调用者) */
                if (!dev->ops.Read(dev->classp,(address / 512), tempSectorCount, buffer2))
                {
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

    u8 WriteBytes(VDL* dev, uint64_t address, uint32_t Count, void* Buffer){
        if(dev && dev->type != VsDevType::Undefined){
            if(dev->ops.WriteBytes != nullptr)
                return dev->ops.WriteBytes(dev->classp,address, Count, Buffer);
            else{
                if (Count == 0)
                    return true;
                /* P1-37: 边界收紧 —— 最后扇区必须 < 容量 */
                if (address + Count - 1 >= dev->MaxSectorCount * 512)
                    return false;
                
                uint32_t tempSectorCount = ((((address + Count) + 511) / 512) - (address / 512));
                uint8_t* buffer2 = (uint8_t*)kmalloc(512); //Malloc for Write Buffer
                
                if (tempSectorCount == 1)
                {
                    _memset(buffer2, 0, 512);
                    if (!dev->ops.Read(dev->classp,(address / 512), 1, buffer2))
                    {
                        kfree(buffer2);
                        
                        return false;
                    }

                    uint16_t offset = address % 512;
                    for (uint64_t i = 0; i < Count; i++)
                        buffer2[i + offset] = ((uint8_t*)Buffer)[i];

                    if (!dev->ops.Write(dev->classp,(address / 512), 1, buffer2))
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
                        if (!dev->ops.Read(dev->classp,(address / 512), 1, buffer2))
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

                        if (!dev->ops.Write(dev->classp,(address / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }
                    }
                    {
                        _memset(buffer2, 0, 512);
                        if (!dev->ops.Read(dev->classp,((address + Count) / 512), 1, buffer2))
                        {
                            kfree(buffer2);
                            
                            return false;
                        }

                        uint16_t specialCount = ((address + Count) % 512);
                        newCount -= specialCount;

                        uint64_t blehus = (Count - specialCount);

                        for (int64_t i = 0; i < specialCount; i++)
                            buffer2[i] = ((uint8_t*)Buffer)[i + blehus];

                        if (!dev->ops.Write(dev->classp,((address + Count) / 512), 1, buffer2))
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

                            if (!dev->ops.Write(dev->classp,newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset)))
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
            case FrameBuffer:return "fb";
            case USBSTORAGE:return "usb";
            default:return "UNDEF";
        }
    }

    void Init(){
        DevMan_Map = hashmap_new(sizeof(DevManEntry), 64, 0, 0, devm_hash, devm_compare, nullptr, nullptr);
        TIMap = hashmap_new(sizeof(DevManKey), 128,0, 0, devtim_hash, devtim_compare, nullptr, nullptr);
        StrMap = hashmap_new(sizeof(DevStrSearch), 128,0, 0, devstrs_hash, devstrs_compare, nullptr, nullptr);
    }
}
#endif