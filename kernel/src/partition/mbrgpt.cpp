/*
* SPDX-License-Identifier: GPL-2.0-only
* File: mbrgpt.cpp
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
#include <partition/mbrgpt.h>
#include <drivers/dev/dev.h>
#include <mem/heap.h>

// 辅助宏：判断是否为 GPT 保护分区
#define IS_GPT(dpt) ((dpt).PartitionTypeIndicator == 0xEE && (dpt).BootIndicator == 0x00)

uint8_t IdentifyMBR(VsDevType DriverType, uint32_t DriverID) {
    MBR_DPT dpt; 
    Dev::SetSDev(DriverType, DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR)
        return 2;
    
    if(IS_GPT(dpt)) return 3; // GPT
    return 0; // 传统 MBR
}

uint8_t GetPartitionSize(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionSize) {
    Dev::SetSDev(DriverType, DriverID);
    MBR_DPT dpt;
    if (Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 2;

    if (IS_GPT(dpt)) {
        uint32_t entry_count;
        // GPT Header 在 LBA 1 (512字节处)
        Dev::ReadBytes(512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &entry_count);
        if (PartitionID >= entry_count) return 4;

        GPT_PTE gptpte;
        if (Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;

        if (gptpte.PartitionStart == 0) { PartitionSize = 0; return 0; }
        PartitionSize = (gptpte.PartitionEnd - gptpte.PartitionStart) + 1;
        return 0;
    } else {
        if (PartitionID >= MBR_PARTITION_MAX) return 6;
        MBR_DPT entry;
        if (Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &entry) == Dev::RW_ERROR)
            return 7;
        PartitionSize = entry.SectorsInPartition;
        return 0;
    }
}

// 注意这里加了 & 引用符号
uint8_t GetPartitionStart(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionStart) {
    MBR_DPT dpt; 
    Dev::SetSDev(DriverType, DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 2;

    if(IS_GPT(dpt)) {
        uint32_t buffer;
        Dev::ReadBytes(512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &buffer);
        if(PartitionID >= buffer) return 4;

        GPT_PTE gptpte;
        if(Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;
        PartitionStart = gptpte.PartitionStart;
    } else {
        if(PartitionID >= MBR_PARTITION_MAX) return 6;
        MBR_DPT buffer2;
        if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &buffer2) == Dev::RW_ERROR)
            return 7;
        PartitionStart = buffer2.StartLBA;
    }
    return 0;
}

// 注意这里加了 & 引用符号
uint8_t GetPartitionEnd(VsDevType DriverType, uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionEnd) {
    MBR_DPT dpt; 
    Dev::SetSDev(DriverType, DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 2;

    if(IS_GPT(dpt)) {
        uint32_t buffer;
        Dev::ReadBytes(512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &buffer);
        if(PartitionID >= buffer) return 4;

        GPT_PTE gptpte;
        if(Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;
        PartitionEnd = gptpte.PartitionEnd;
    } else {
        if(PartitionID >= MBR_PARTITION_MAX) return 6;
        MBR_DPT entry;
        if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &entry) == Dev::RW_ERROR)
            return 7;
        
        // End = Start + Count - 1
        if (entry.SectorsInPartition == 0) PartitionEnd = 0;
        else PartitionEnd = entry.StartLBA + entry.SectorsInPartition - 1;
    }
    return 0;
}

uint8_t GetPartitionCount(VsDevType DriverType, uint32_t DriverID) {
    MBR_DPT dpt; 
    Dev::SetSDev(DriverType, DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) return 0;

    if(IS_GPT(dpt)) {
        uint32_t buffer;
        // 简单返回分区表项总数（通常是128）
        if(Dev::ReadBytes(512 + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &buffer) == Dev::RW_ERROR)
            return 0;
        return (uint8_t)buffer;
    } else {
        uint8_t count = 0;
        for(uint8_t i = 0; i < MBR_PARTITION_MAX; i++) {
            MBR_DPT buffer2;
            if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + i * 16, 16, &buffer2) == Dev::RW_ERROR)
                break;
            if(buffer2.SectorsInPartition != 0) {
                count++;
            }
        }
        return count;
    }
}