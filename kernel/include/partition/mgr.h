/*
* SPDX-License-Identifier: GPL-2.0-only
* File: mgr.h
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
