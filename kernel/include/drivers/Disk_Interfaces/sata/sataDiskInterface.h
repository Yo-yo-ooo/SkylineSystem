/*
* SPDX-License-Identifier: GPL-2.0-only
* File: sataDiskInterface.h
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
#ifdef __x86_64__
#include <drivers/ahci/ahci.h>
#include <stdint.h>
#include <stddef.h>

class SataDiskInterface
{
private:
    AHCI::Port* Port;
    uint32_t SectorCount;

    u8 FRegVsDEV_R(uint64_t lba, uint32_t SectorCount, void* Buffer);
    u8 FRegVsDEV_W(uint64_t lba, uint32_t SectorCount, void* Buffer);

    u8 FRegVsDEV_Wb(uint64_t address, uint64_t count, void* buffer);
    u8 FRegVsDEV_Rb(uint64_t address, uint64_t count, void* buffer);
public:
    SataDiskInterface(AHCI::Port* port);
    bool Read(uint64_t sector, uint32_t sectorCount, void* buffer);
    bool Write(uint64_t sector, uint32_t sectorCount, void* buffer);

    bool ReadBytes(uint64_t address, uint64_t count, void* buffer);
    bool WriteBytes(uint64_t address, uint64_t count, void* buffer);
    uint32_t GetMaxSectorCount();
};
#endif