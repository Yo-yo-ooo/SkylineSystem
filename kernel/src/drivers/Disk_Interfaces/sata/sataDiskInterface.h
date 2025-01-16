#pragma once
#include "../../ahci/ahci.h"
#include <stdint.h>
#include <stddef.h>

namespace SataDiskInterface
{
    
    
    void Init(AHCI::Port* port);
    bool Read(uint64_t sector, uint32_t sectorCount, void* buffer);
    bool Write(uint64_t sector, uint32_t sectorCount, void* buffer);

    bool ReadBytes(uint64_t address, uint64_t count, void* buffer);
    bool WriteBytes(uint64_t address, uint64_t count, void* buffer);
    uint32_t GetMaxSectorCount();
}