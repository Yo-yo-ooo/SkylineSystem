//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/Disk_Interfaces/ram/ramDiskInterface.h>
#include <mem/heap.h>

namespace RamDiskInterface
{
    uint8_t* Buffer;
    uint32_t SectorCount;
    uint32_t GetMaxSectorCount()
    {
        return SectorCount;
    }

    void Init(uint64_t sectorCount)
    {
        Buffer = (uint8_t*)kmalloc(sectorCount * 512); //Malloc For Ram Disk
        /* 修复: 分配失败时不得继续使用空指针 */
        if (!Buffer) { SectorCount = 0; return; }
        SectorCount = sectorCount;
        _memset(Buffer, 0, sectorCount * 512);
    }

    bool Read(uint64_t sector, uint32_t sectorCount, void* buffer)
    {
        uint8_t* buf = (uint8_t*)buffer;
        for (uint64_t s = sector; s < sector + sectorCount; s++)
        {
            if (s >= SectorCount)
                return false;
            /* 修复: 原用绝对扇区号 s 索引调用者缓冲区 -> sector!=0 时越界读写,
               应使用相对偏移 (s - sector) */
            _memcpy(Buffer + (s * 512), buf + ((s - sector) * 512), 512);
        }
        return true;
    }

    bool Write(uint64_t sector, uint32_t sectorCount, void* buffer)
    {
        uint8_t* buf = (uint8_t*)buffer;
        for (uint64_t s = sector; s < sector + sectorCount; s++)
        {
            if (s >= SectorCount)
                return false;
            /* 修复: 同上 —— 调用者缓冲区用相对偏移索引 */
            _memcpy(buf + ((s - sector) * 512), Buffer + (s * 512), 512);
        }
        return true;
    }

    bool ReadBytes(uint64_t address, uint64_t count, void* buffer)
    {
        if (address + count > SectorCount * 512)
            return false;

        for (uint64_t i = 0; i < count; i++)
            ((uint8_t*)buffer)[i] = Buffer[address + i];
        
        return true;
    }

    bool WriteBytes(uint64_t address, uint64_t count, void* buffer)
    {
        if (address + count > SectorCount * 512)
            return false;

        for (uint64_t i = 0; i < count; i++)
            Buffer[address + i] = ((uint8_t*)buffer)[i];

        return true;
    }
}
#endif