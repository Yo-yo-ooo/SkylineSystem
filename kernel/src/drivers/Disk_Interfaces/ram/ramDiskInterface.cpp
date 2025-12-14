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
        SectorCount = sectorCount;
        Buffer = (uint8_t*)kmalloc(sectorCount * 512); //Malloc For Ram Disk
        _memset(Buffer, 0, sectorCount * 512);
    }

    bool Read(uint64_t sector, uint32_t sectorCount, void* buffer)
    {
        uint8_t* buf = (uint8_t*)buffer;
        for (uint64_t s = sector; s < sector + sectorCount; s++)
        {
            if (s >= SectorCount)
                return false;
            _memcpy(Buffer + (s * 512), buf + (s * 512), 512);
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
            _memcpy(buf + (s * 512), Buffer + (s * 512), 512);
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