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
    uint64_t CurPartitionStart = 0; // 单位：扇区 LBA
    uint64_t CurPartitionEnd = 0;   // 单位：扇区 LBA
    VDL* CurDevice = nullptr;

    void SetCurPartition(VsDevType DriverType, uint32_t DriverID, uint8_t partitionID) {
        CurDriverType = DriverType;
        CurDriver = DriverID;
        CurPartition = partitionID;
        CurDevice = Dev::GetSDEV(DriverType, DriverID);

#ifdef USE_VIRT_IMAGE
        return;
#else
        if (GetPartitionStart(CurDriverType, CurDriver, CurPartition, CurPartitionStart) != 0)
            kerrorln("Cannot get start of partition %d", partitionID);
        else if (GetPartitionEnd(CurDriverType, CurDriver, CurPartition, CurPartitionEnd) != 0)
            kerrorln("Cannot get end of partition %d", partitionID);
#endif
    }

    uint8_t Read(VsDevType DriverType, uint32_t DriverID, uint8_t partitionID,uint64_t lba, uint32_t SectorCount, void* Buffer) {
        //kinfoln("PartitionManager::Read: LBA=%llu, SectorCount=%u", lba, SectorCount);
        CurDevice = Dev::GetSDEV(DriverType, DriverID);
        if (!CurDevice) return Dev::RW_ERROR;

#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.Read(CurDevice->classp, lba, SectorCount, Buffer);
#else
        // 边界检查：lba 是相对于分区的偏移
        if (lba + SectorCount > (CurPartitionEnd - CurPartitionStart + 1)) {
            kerrorln("Read: LBA out of partition range");
            return true; // 保持你失败返回 true 的风格
        }

        // 实际物理 LBA = 分区起始 LBA + 偏移 LBA
        if (CurDevice->ops.Read(
                CurDevice->classp,
                CurPartitionStart + lba, 
                SectorCount, // 修正了之前错误的 Count 变量名
                Buffer
            ) != Dev::RW_OK)
            return true;
        else
            return false;
#endif
    }

    uint8_t Write(VsDevType DriverType, uint32_t DriverID, uint8_t partitionID,uint64_t lba, uint32_t SectorCount, void* Buffer) {
        CurDevice = Dev::GetSDEV(DriverType, DriverID);
        if (!CurDevice) return Dev::RW_ERROR;

#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.Write(CurDevice->classp, lba, SectorCount, Buffer);
#else
        if (lba + SectorCount > (CurPartitionEnd - CurPartitionStart + 1)) {
            kerrorln("Write: LBA out of partition range");
            return true;
        }

        if (CurDevice->ops.Write(
                CurDevice->classp,
                CurPartitionStart + lba,
                SectorCount, // 修正了之前错误的 Count 变量名
                Buffer
            ) != Dev::RW_OK)
            return true;
        else
            return false;
#endif
    }

    uint8_t ReadBytes(uint64_t address, uint32_t Count, void* Buffer) {
        if (!CurDevice) return Dev::RW_ERROR;

#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.ReadBytes(CurDevice->classp, address, Count, Buffer);
#else
        // 1. 统一单位为字节进行判断
        uint64_t start_byte = CurPartitionStart * 512;
        uint64_t partition_len_byte = (CurPartitionEnd - CurPartitionStart + 1) * 512;

        if (address + Count > partition_len_byte) {
            kerrorln("ReadBytes: Access out of partition range!");
            return true; 
        }

        // 2. 传入正确的物理字节地址 (分区起始字节 + 字节偏移)
        if (CurDevice->ops.ReadBytes(
                CurDevice->classp,
                start_byte + address,
                Count,
                Buffer
            ) != Dev::RW_OK)
            return true; 
        else 
            return false;
#endif
    }

    uint8_t WriteBytes(uint64_t address, uint32_t Count, void* Buffer) {
        if (!CurDevice) return Dev::RW_ERROR;

#ifdef USE_VIRT_IMAGE
        return CurDevice->ops.WriteBytes(CurDevice->classp, address, Count, Buffer);
#else
        uint64_t start_byte = CurPartitionStart * 512;
        uint64_t partition_len_byte = (CurPartitionEnd - CurPartitionStart + 1) * 512;

        if (address + Count > partition_len_byte) {
            kerrorln("WriteBytes: Access out of partition range!");
            return true;
        }

        if (CurDevice->ops.WriteBytes(
                CurDevice->classp,
                start_byte + address,
                Count,
                Buffer
            ) != Dev::RW_OK)
            return true;
        else 
            return false;
#endif
    }

    uint64_t GetMaxSectorCount(VsDevType DriverType, uint32_t DriverID, uint8_t partitionID) {
        CurDevice = Dev::GetSDEV(DriverType, DriverID);
#ifdef USE_VIRT_IMAGE
        //uint64_t x = CurDevice->ops.GetMaxSectorCount(CurDevice->classp);
        //kinfoln("Max Sector Count:L: %d", x);
        return CurDevice->ops.GetMaxSectorCount(CurDevice->classp);;
#else
        // 分区总扇区数 = 结束LBA - 起始LBA + 1
        return (CurPartitionEnd - CurPartitionStart + 1);
#endif
    }

} // namespace PartitionManager