#pragma once
#ifndef PARTITION_MGR_H
#define PARTITION_MGR_H

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>

namespace PartitionManager
{
    extern uint32_t CurDriver;
    extern uint8_t CurPartition;
    extern uint64_t CurPartitionStart;
    extern uint64_t CurPartitionEnd;

    void SetCurPartition(uint32_t driverID,uint8_t partitionID);

    //These function must do SetCurPartition First!
    uint8_t Read(uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t Write(uint64_t lba, uint32_t SectorCount, void* Buffer);
    uint8_t ReadBytes(uint64_t address, uint32_t Count, void* Buffer);
    uint8_t WriteBytes(uint64_t address, uint32_t Count, void* Buffer);
    uint64_t GetMaxSectorCount();
} // namespace PartitionOPS


#endif
