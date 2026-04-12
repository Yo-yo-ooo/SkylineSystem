/*
* SPDX-License-Identifier: GPL-2.0-only
* File: mgr.cpp
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
#include <partition/mgr.h>

#include <partition/identfstype.h>
#include <partition/mbrgpt.h>

namespace PartitionManager
{
    VsDevType CurDriverType;
    uint32_t CurDriver = 0;
    uint8_t CurPartition = 0;
    uint64_t CurPartitionStart = 0;
    uint64_t CurPartitionEnd = 0;
    VDL* CurDevice;

    void SetCurPartition(VsDevType DriverType,uint32_t DriverID, uint8_t partitionID){
        CurDriverType = DriverType;
        CurDriver = DriverID;
        CurPartition = partitionID;
        CurDevice = Dev::GetSDEV(DriverType, DriverID);
#ifdef USE_VIRT_IMAGE
        return;
#else
        if(GetPartitionStart(PartitionManager::CurDriverType,PartitionManager::CurDriver,
            PartitionManager::CurPartition,PartitionManager::CurPartitionStart) != 0)
            kerrorln("Cannot change to partition %d",partitionID);
        else if(GetPartitionEnd(PartitionManager::CurDriverType,PartitionManager::CurDriver,
            PartitionManager::CurPartition,PartitionManager::CurPartitionEnd) != 0)
            kerrorln("Cannot change to partition %d",partitionID);
        else return;
#endif
    }

    uint8_t Read(uint64_t lba, uint32_t SectorCount, void* Buffer){
        
#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.Read(
                CurDevice->classp,
                lba,SectorCount,Buffer
        );
#else
        if(((PartitionManager::CurPartitionStart) % 512 == 0 ? 
        (PartitionManager::CurPartitionStart) / 512 :
        (PartitionManager::CurPartitionStart) / 512 + 1) + lba > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            if(CurDevice->ops.Read(
                CurDevice->classp,
                (PartitionManager::CurPartitionStart) / 512 + lba,Count,Buffer
            ) != Dev::RW_OK)
                return true;
            else return false;
        }
#endif
    }
    uint8_t Write(uint64_t lba, uint32_t SectorCount, void* Buffer){
        
#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.Write(
                CurDevice->classp,
                lba,SectorCount,Buffer
        );
#else
        if(((PartitionManager::CurPartitionStart) % 512 == 0 ? 
        (PartitionManager::CurPartitionStart) / 512 :
        (PartitionManager::CurPartitionStart) / 512 + 1) + lba > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            return CurDevice->ops.Write(
                CurDevice->classp,
                (PartitionManager::CurPartitionStart) / 512 + lba,Count,Buffer
            );
        }
#endif
    }

    uint8_t ReadBytes(uint64_t address, uint32_t Count, void* Buffer){
        
#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.ReadBytes(
                CurDevice->classp,
                address,Count,Buffer);
        
#else
        if(PartitionManager::CurPartitionStart + address > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            if(CurDevice->ops.ReadBytes(
                CurDevice->classp,
                PartitionManager::CurPartitionStart + address,Count,Buffer
            ) != Dev::RW_OK)
                return true;
            else return false;
        }
#endif
    }
    uint8_t WriteBytes(uint64_t address, uint32_t Count, void* Buffer){
        
#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.WriteBytes(
                CurDevice->classp,
                address,Count,Buffer
        );
#else
        if(PartitionManager::CurPartitionStart + address > CurPartitionEnd){
            kerrorln("Address > CurPartitionEnd");
            return false;
        }else{
            if(CurDevice->ops.WriteBytes(
                CurDevice->classp,
                PartitionManager::CurPartitionStart + address,Count,Buffer
            ) != Dev::RW_OK)
                return true;
            else return false;
        }
#endif
    }

    uint64_t GetMaxSectorCount(){
        
#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.GetMaxSectorCount(CurDevice->classp);
#else
        return GetPartitionSize(CurDriver,CurPartition) / 512 + 1;
#endif
    }


} // namespace PartitionManager
