/*
* SPDX-License-Identifier: GPL-2.0-only
* File: devl.cpp
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
// ---------------- 数据结构定义 ----------------

struct DevOPS_Entry {
    VsDevType type;
    DevOPS ops;
};

// 记录特定设备的注册数量，充当原本 vector 的 size 和 index 分配器
struct DevCount_Entry {
    VsDevType type;
    uint32_t count;
};

// 联合主键：设备类型 + 设备索引
struct DevInfoKey {
    VsDevType type;
    uint32_t index;
};

// 设备信息实体
struct DevInfo_Entry {
    DevInfoKey key;
    VDL dev;
};


// ---------------- 全局 Hashmap 指针 ----------------

static struct hashmap *Type2DeviceOPS_Map = nullptr;
static struct hashmap *DeviceCount_Map = nullptr;
static struct hashmap *DeviceInfos_Map = nullptr;


// ---------------- Hash 与比较函数 ----------------

// 针对单一 VsDevType 作为键的 Hash 和 Compare (用于 OPS 和 Count 表)
static uint64_t dev_type_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const VsDevType *type = (const VsDevType *)item;
    return hashmap_sip(type, sizeof(VsDevType), seed0, seed1);
}

static int dev_type_compare(const void *a, const void *b, void *udata) {
    const VsDevType *ta = (const VsDevType *)a;
    const VsDevType *tb = (const VsDevType *)b;
    return (*ta == *tb) ? 0 : (*ta < *tb ? -1 : 1);
}

// 针对联合主键 (DevInfoKey) 的 Hash 和 Compare (用于 VDL 信息表)
static uint64_t dev_info_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const DevInfo_Entry *entry = (const DevInfo_Entry *)item;
    // 分别对 type 和 index 进行哈希计算混合，避免内存对齐填充数据（Padding）干扰哈希值
    uint64_t h1 = hashmap_sip(&entry->key.type, sizeof(VsDevType), seed0, seed1);
    return hashmap_sip(&entry->key.index, sizeof(uint32_t), h1, seed1);
}

static int dev_info_compare(const void *a, const void *b, void *udata) {
    const DevInfo_Entry *ea = (const DevInfo_Entry *)a;
    const DevInfo_Entry *eb = (const DevInfo_Entry *)b;
    if (ea->key.type != eb->key.type) return ea->key.type < eb->key.type ? -1 : 1;
    if (ea->key.index != eb->key.index) return ea->key.index < eb->key.index ? -1 : 1;
    return 0;
}


namespace Dev {

    // 内部懒加载初始化函数
    static void EnsureInitialized() {
        if (!Type2DeviceOPS_Map) {
            Type2DeviceOPS_Map = hashmap_new(sizeof(DevOPS_Entry), 16, 0, 0, 
                                             dev_type_hash, dev_type_compare, nullptr, nullptr);
        }
        if (!DeviceCount_Map) {
            DeviceCount_Map = hashmap_new(sizeof(DevCount_Entry), 16, 0, 0, 
                                          dev_type_hash, dev_type_compare, nullptr, nullptr);
        }
        if (!DeviceInfos_Map) {
            DeviceInfos_Map = hashmap_new(sizeof(DevInfo_Entry), 32, 0, 0, 
                                          dev_info_hash, dev_info_compare, nullptr, nullptr);
        }
    }

    void AddDevice(VDL DeviceInfo, VsDevType DeviceType, DevOPS OPS) {
        // spinlock_lock(&DeviceInfo.lock);
        EnsureInitialized();

        // 1. 注册或更新设备操作集 OPS
        DevOPS_Entry lookup_ops = { DeviceType, {} };
        if (!hashmap_get(Type2DeviceOPS_Map, &lookup_ops)) {
            DevOPS_Entry new_ops_entry = { DeviceType, OPS };
            hashmap_set(Type2DeviceOPS_Map, &new_ops_entry);
        }

        // 2. 获取并分配当前设备的 Index (替代 vector::push_back)
        uint32_t current_index = 0;
        DevCount_Entry lookup_count = { DeviceType, 0 };
        DevCount_Entry *count_ptr = (DevCount_Entry*)hashmap_get(DeviceCount_Map, &lookup_count);
        
        if (count_ptr) {
            current_index = count_ptr->count;
            count_ptr->count++; // 指针更新，直接修改 Hashmap 内存储的计数值
        } else {
            DevCount_Entry new_count = { DeviceType, 1 };
            hashmap_set(DeviceCount_Map, &new_count);
            current_index = 0;
        }

        // 3. 将设备信息写入二维映射表
        DevInfo_Entry new_info_entry;
        new_info_entry.key.type = DeviceType;
        new_info_entry.key.index = current_index;
        new_info_entry.dev = DeviceInfo;
        hashmap_set(DeviceInfos_Map, &new_info_entry);

        // spinlock_unlock(&DeviceInfo.lock);
    }

    VDL FindDevice(VsDevType DeviceType, uint32_t DeviceIndex) {
        EnsureInitialized();
        
        // 构造联合主键直接查询
        DevInfo_Entry lookup_info;
        lookup_info.key.type = DeviceType;
        lookup_info.key.index = DeviceIndex;
        
        DevInfo_Entry *info_ptr = (DevInfo_Entry*)hashmap_get(DeviceInfos_Map, &lookup_info);
        
        if (info_ptr) {
            return info_ptr->dev;
        }
        
        // 未找到时返回空的 VDL
        VDL empty_dev;
        _memset(&empty_dev, 0, sizeof(VDL));
        return empty_dev;
    }

    uint8_t DeviceRead(VsDevType DeviceType, uint32_t DevIDX, size_t offset, void* Buffer, size_t nbytes) {
        EnsureInitialized();
        DevOPS_Entry lookup_ops = { DeviceType, {} };
        DevOPS_Entry *ops_ptr = (DevOPS_Entry*)hashmap_get(Type2DeviceOPS_Map, &lookup_ops);
        
        if (!ops_ptr) return false;
        DevOPS curr_operation = ops_ptr->ops;

        if (curr_operation.ReadBytes != nullptr) {
            return curr_operation.ReadBytes(
                FindDevice(DeviceType, DevIDX).classp,
                offset,
                nbytes,
                Buffer
            );
        } else if (curr_operation.ReadBytes_ != nullptr) {
            return curr_operation.ReadBytes_(
                offset,
                nbytes,
                Buffer
            );
        } else if (curr_operation.Read == nullptr) {
            if (nbytes == 0) return true;
            VDL dev = FindDevice(DeviceType, DevIDX);
            if (offset + nbytes > dev.MaxSectorCount * 512) return false;
            
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);
            _memset(buffer2, 0, tempSectorCount * 512);

            if (!curr_operation.Read_((offset / 512), tempSectorCount, buffer2)) {
                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    ((uint8_t*)Buffer)[i] = buffer2[i + offset_];

                kfree(buffer2);
                return false;
            }

            uint16_t offset_ = offset % 512;
            for (uint64_t i = 0; i < nbytes; i++)
                ((uint8_t*)Buffer)[i] = buffer2[i + offset_];
                    
            kfree(buffer2);
            return true;
        } else if (curr_operation.Read_ == nullptr) {
            VDL dev = FindDevice(DeviceType, DevIDX);
            if (nbytes == 0) return true;
            if (offset + nbytes > dev.MaxSectorCount * 512) return false;
            
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);
            _memset(buffer2, 0, tempSectorCount * 512);

            if (!curr_operation.Read(dev.classp, (offset / 512), tempSectorCount, buffer2)) {
                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    ((uint8_t*)Buffer)[i] = buffer2[i + offset_];

                kfree(buffer2);
                return false;
            }

            uint16_t offset_ = offset % 512;
            for (uint64_t i = 0; i < nbytes; i++)
                ((uint8_t*)Buffer)[i] = buffer2[i + offset_];
                    
            kfree(buffer2);
            return true;
        }
        return false;
    }

    uint8_t DeviceWrite(VsDevType DeviceType, uint32_t DevIDX, size_t offset, void* Buffer, size_t nbytes) {
        EnsureInitialized();
        DevOPS_Entry lookup_ops = { DeviceType, {} };
        DevOPS_Entry *ops_ptr = (DevOPS_Entry*)hashmap_get(Type2DeviceOPS_Map, &lookup_ops);
        
        if (!ops_ptr) return false;
        DevOPS curr_operation = ops_ptr->ops;

        if (curr_operation.WriteBytes != nullptr) {
            return curr_operation.WriteBytes(
                FindDevice(DeviceType, DevIDX).classp,
                offset,
                nbytes,
                Buffer
            );
        } else if (curr_operation.WriteBytes_ != nullptr) {
            return curr_operation.WriteBytes_(
                offset,
                nbytes,
                Buffer
            );
        } else if (curr_operation.Read_ == nullptr) {
            if (nbytes == 0) return true;
            VDL dev = FindDevice(DeviceType, DevIDX);
            if (offset + nbytes > dev.MaxSectorCount * 512) return false;
            
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(512);

            if (tempSectorCount == 1) {
                _memset(buffer2, 0, 512);
                if (!curr_operation.Read(dev.classp, (offset / 512), 1, buffer2)) {
                    kfree(buffer2);
                    return false;
                }

                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                if (!curr_operation.Write(dev.classp, (offset / 512), 1, buffer2)) {
                    kfree(buffer2);
                    return false;
                }
            } else {
                uint64_t newAddr = offset;
                uint64_t newCount = nbytes;
                uint64_t addrOffset = 0;
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read(dev.classp, (offset / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }

                    uint16_t offset_ = offset % 512;
                    uint16_t specialCount = 512 - offset_;
                    addrOffset = specialCount;
                    newAddr = offset + specialCount;
                    newCount = nbytes - specialCount;

                    for (uint64_t i = 0; i < specialCount; i++)
                        buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                    if (!curr_operation.Write(dev.classp, (offset / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }
                }
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read(dev.classp, ((offset + nbytes) / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }

                    uint16_t specialCount = ((offset + nbytes) % 512);
                    newCount -= specialCount;
                    uint64_t blehus = (nbytes - specialCount);

                    for (int64_t i = 0; i < specialCount; i++)
                        buffer2[i] = ((uint8_t*)Buffer)[i + blehus];

                    if (!curr_operation.Write(dev.classp, ((offset + nbytes) / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }
                }
                {
                    uint64_t newSectorCount = newCount / 512;
                    if (newSectorCount != 0) {
                        uint64_t newSectorStartId = newAddr / 512;
                        if (!curr_operation.Write(dev.classp, newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset))) {
                            kfree(buffer2);
                            return false;
                        }
                    }
                }
            }
            kfree(buffer2);
            return true;
        } else if (curr_operation.Read == nullptr) {
            if (nbytes == 0) return true;
            VDL dev = FindDevice(DeviceType, DevIDX);
            if (offset + nbytes > dev.MaxSectorCount * 512) return false;
            
            uint32_t tempSectorCount = ((((offset + nbytes) + 511) / 512) - (offset / 512));
            uint8_t* buffer2 = (uint8_t*)kmalloc(512);

            if (tempSectorCount == 1) {
                _memset(buffer2, 0, 512);
                if (!curr_operation.Read_((offset / 512), 1, buffer2)) {
                    kfree(buffer2);
                    return false;
                }

                uint16_t offset_ = offset % 512;
                for (uint64_t i = 0; i < nbytes; i++)
                    buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                if (!curr_operation.Write_((offset / 512), 1, buffer2)) {
                    kfree(buffer2);
                    return false;
                }
            } else {
                uint64_t newAddr = offset;
                uint64_t newCount = nbytes;
                uint64_t addrOffset = 0;
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read_((offset / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }

                    uint16_t offset_ = offset % 512;
                    uint16_t specialCount = 512 - offset_;
                    addrOffset = specialCount;
                    newAddr = offset + specialCount;
                    newCount = nbytes - specialCount;

                    for (uint64_t i = 0; i < specialCount; i++)
                        buffer2[i + offset_] = ((uint8_t*)Buffer)[i];

                    if (!curr_operation.Write_((offset / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }
                }
                {
                    _memset(buffer2, 0, 512);
                    if (!curr_operation.Read_(((offset + nbytes) / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }

                    uint16_t specialCount = ((offset + nbytes) % 512);
                    newCount -= specialCount;
                    uint64_t blehus = (nbytes - specialCount);

                    for (int64_t i = 0; i < specialCount; i++)
                        buffer2[i] = ((uint8_t*)Buffer)[i + blehus];

                    if (!curr_operation.Write_(((offset + nbytes) / 512), 1, buffer2)) {
                        kfree(buffer2);
                        return false;
                    }
                }
                {
                    uint64_t newSectorCount = newCount / 512;
                    if (newSectorCount != 0) {
                        uint64_t newSectorStartId = newAddr / 512;
                        if (!curr_operation.Write_(newSectorStartId, newSectorCount, (void*)((uint64_t)Buffer + addrOffset))) {
                            kfree(buffer2);
                            return false;
                        }
                    }
                }
            }
            kfree(buffer2);
            return true;
        }
        return false;
    }

    uint64_t DeviceMemoryMap(VsDevType DeviceType, uint32_t DevIDX, uint64_t length, uint64_t prot, uint64_t offset, uint64_t VADDR) {
        EnsureInitialized();
        DevOPS_Entry lookup_ops = { DeviceType, {} };
        DevOPS_Entry *ops_ptr = (DevOPS_Entry*)hashmap_get(Type2DeviceOPS_Map, &lookup_ops);
        
        if (ops_ptr && ops_ptr->ops.MemoryMap != nullptr) {
            return ops_ptr->ops.MemoryMap(
                length,
                prot,
                offset,
                VADDR
            );
        }
        return -1;
    }
}

#endif