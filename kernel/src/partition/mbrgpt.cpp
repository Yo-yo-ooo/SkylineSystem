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

uint8_t IdentifyMBR(uint32_t DriverID){
    if(DriverID > Dev::vsdev_list_idx)
        return 1; //DriverID ERR
    
    DevList ThisInfo = Dev::DevList_[DriverID];
    MBR_DPT dpt; 
    Dev::SetSDev(DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET,16,&dpt) == Dev::RW_ERROR)
        return 2;
    if(dpt.PartitionTypeIndicator == 0xEE && dpt.BootIndicator == 0x00){//GPT
        return 3;
    }else{
        return 0;
    }
    return 4;
}

uint8_t GetPartitionSize(uint32_t DriverID, uint32_t PartitionID, uint64_t &PartitionSize) {
    if (DriverID > Dev::vsdev_list_idx) return 1;

    Dev::SetSDev(DriverID);
    MBR_DPT dpt;
    
    // 读取 MBR 的第一个分区表项来检查是否为 GPT 保护分区
    if (Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET, 16, &dpt) == Dev::RW_ERROR) 
        return 2;

    if (dpt.PartitionTypeIndicator == 0xEE) { // GPT 模式
        uint32_t entry_count;
        // 从 GPT Header 读取分区表项总数
        Dev::ReadBytes(MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET, 4, &entry_count);
        
        if (PartitionID >= entry_count) return 4;

        GPT_PTE gptpte;
        if (Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + (PartitionID * 128), 128, &gptpte) == Dev::RW_ERROR)
            return 5;

        // GPT 范围通常是闭区间 [Start, End]
        PartitionSize = (gptpte.PartitionEnd - gptpte.PartitionStart) + 1;
        return 0;
    } else { // 传统 MBR 模式
        if (PartitionID >= MBR_PARTITION_MAX) return 6;

        MBR_DPT entry;
        if (Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + (PartitionID * 16), 16, &entry) == Dev::RW_ERROR)
            return 7;

        PartitionSize = entry.SectorsInPartition;
        return 0;
    }
}


uint8_t GetPartitionStart(uint32_t DriverID,uint32_t PartitionID,uint64_t PartitionStart){
    if(DriverID > Dev::vsdev_list_idx)
        return 1; //DriverID ERR
    
    DevList ThisInfo = Dev::DevList_[DriverID];
    MBR_DPT dpt; 
    uint32_t buffer;
    Dev::SetSDev(DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET,16,&dpt) == false)
        return 2;
        //80

    if(dpt.PartitionTypeIndicator == 0xEE && dpt.BootIndicator == 0x00){//GPT
        if(Dev::ReadBytes(MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET,4,&buffer) == false)
            return 3;
        if(PartitionID > buffer)
            return 4;
        GPT_PTE gptpte;
        if(Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + PartitionID * 128,
                128,&gptpte) == false)
                return 5;
        PartitionStart = gptpte.PartitionStart;
        return 0;
    }else{

        if(PartitionID > MBR_PARTITION_MAX)
            return 6; //PartitionID ERR
        
        MBR_DPT buffer2;
        //Dev::SetSDev(DriverID);
        if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + PartitionID * 16,
            16,&buffer2) == false)
            return 7;
        
        //CHS To LBA : easy to R/W 
        PartitionStart = buffer2.StartLBA;
        return 0;
    }
    return 0;
}

uint8_t GetPartitionEnd(uint32_t DriverID,uint32_t PartitionID,uint64_t PartitionEnd){
    if(DriverID > Dev::vsdev_list_idx)
        return 1; //DriverID ERR
    
    DevList ThisInfo = Dev::DevList_[DriverID];
    MBR_DPT dpt; 
    uint32_t buffer;
    Dev::SetSDev(DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET,16,&dpt) == false)
        return 2;
        //80

    if(dpt.PartitionTypeIndicator == 0xEE && dpt.BootIndicator == 0x00){//GPT
        if(Dev::ReadBytes(MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET,4,&buffer) == false)
            return 3;
        if(PartitionID > buffer)
            return 4;
        GPT_PTE gptpte;
        if(Dev::ReadBytes(GPT_PARTITION_TABLE_OFFSET + PartitionID * 128,
                128,&gptpte) == false)
                return 5;
        PartitionEnd = gptpte.PartitionEnd;
        return 0;
    }else{

        if(PartitionID > MBR_PARTITION_MAX)
            return 6; //PartitionID ERR
            
        MBR_DPT buffer2;
        MBR_DPT buffer3;
        //Dev::SetSDev(DriverID);
        if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + PartitionID * 16,
            16,&buffer2) == false)
            return 7;

        if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + PartitionID * 16,
            16,&buffer3) == false)
            return 7;
        
        //CHS To LBA : easy to R/W 
        PartitionEnd = buffer3.StartLBA - buffer2.StartLBA;
        return 0;
    }
    return 0;
}

uint8_t GetPartitionCount(uint32_t DriverID){
    if(DriverID > Dev::vsdev_list_idx)
        return 1; //DriverID ERR
    
    DevList ThisInfo = Dev::DevList_[DriverID];
    MBR_DPT dpt; 
    uint32_t buffer;
    Dev::SetSDev(DriverID);
    if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET,16,&dpt) == false)
        return 2;
    

    if(dpt.PartitionTypeIndicator == 0xEE && dpt.BootIndicator == 0x00){//GPT
        if(Dev::ReadBytes(MBR_TABLE_SIZE + GPT_HEADER_NUMBER_OF_PTE_OFFSET,4,&buffer) == false)
            return 3;
        else
            return buffer;
    }else{
        for(uint8_t i = 0;i < MBR_PARTITION_MAX;i++){
            MBR_DPT buffer2;
            if(Dev::ReadBytes(MBR_PARTITION_TABLE_OFFSET + i * 16,
                16,&buffer2) == false)
                return 7;
            if(buffer2.SectorsInPartition = NULL){
                return i;
            }
        }
    }
    return 0;
}