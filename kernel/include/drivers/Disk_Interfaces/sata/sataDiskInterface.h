#pragma once
#include <drivers/ahci/ahci.h>
#include <stdint.h>
#include <stddef.h>

namespace SataDiskInterface
{
    u8 FRegVsDEV_R(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 FRegVsDEV_W(uint64_t lba, uint32_t SectorCount, void* Buffer);

    u8 FRegVsDEV_Wb(uint64_t address, uint64_t count, void* buffer);
    u8 FRegVsDEV_Rb(uint64_t address, uint64_t count, void* buffer);
    
    void Init(AHCI::Port* port);
    bool Read(uint64_t sector, uint32_t sectorCount, void* buffer);
    bool Write(uint64_t sector, uint32_t sectorCount, void* buffer);

    bool ReadBytes(uint64_t address, uint64_t count, void* buffer);
    bool WriteBytes(uint64_t address, uint64_t count, void* buffer);
    uint32_t GetMaxSectorCount();
}