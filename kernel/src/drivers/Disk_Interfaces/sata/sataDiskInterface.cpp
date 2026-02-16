/*
* SPDX-License-Identifier: GPL-2.0-only
* File: sataDiskInterface.cpp
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

#ifdef __x86_64__
#include <drivers/Disk_Interfaces/sata/sataDiskInterface.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/allin.h>

#include <drivers/ahci/ahci.h>
#include <drivers/dev/dev.h>


uint32_t SataDiskInterface::GetMaxSectorCount()
{
    return Port->GetMaxSectorCount();
}

SataDiskInterface::SataDiskInterface(AHCI::Port* port)
{
    
    if(port == nullptr)
        return;
    this->Port = port;
    this->Port->buffer = (uint8_t*)(PMM::Request()); // 4096 Bytes
    //VMM::Map(this->Port->buffer, this->Port->buffer);
    

    
    SectorCount = GetMaxSectorCount();
    

    
    DevOPS ops;
    ops.GetMaxSectorCount = nullptr;
    ops.Read              = nullptr;ops.ReadBytes  = nullptr;
    ops.Write             = nullptr;ops.WriteBytes = nullptr;
    //_memset(&ops, 0, sizeof(DevOPS));
    ops.Read = CFCast<decltype(ops.Read)>(&SataDiskInterface::FRegVsDEV_R);
    ops.Write = CFCast<decltype(ops.Write)>(&SataDiskInterface::FRegVsDEV_W);
    ops.ReadBytes = CFCast<decltype(ops.ReadBytes)>(&SataDiskInterface::FRegVsDEV_Rb);
    ops.WriteBytes = CFCast<decltype(ops.WriteBytes)>(&SataDiskInterface::FRegVsDEV_Wb);
    ops.GetMaxSectorCount = CFCast<decltype(ops.GetMaxSectorCount)>(&SataDiskInterface::GetMaxSectorCount);
    Dev::AddStorageDevice(VsDevType::SATA, ops,SectorCount,this);
    

}

u8 SataDiskInterface::FRegVsDEV_R(uint64_t lba, uint32_t SectorCount, void* Buffer){
    if(Read(lba, SectorCount, Buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

u8 SataDiskInterface::FRegVsDEV_W(uint64_t lba, uint32_t SectorCount, void* Buffer){
    if(Write(lba, SectorCount, Buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

u8 SataDiskInterface::FRegVsDEV_Wb(uint64_t address, uint64_t count, void* buffer){
    if(WriteBytes(address, count, buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

u8 SataDiskInterface::FRegVsDEV_Rb(uint64_t address, uint64_t count, void* buffer){
    if(ReadBytes(address, count, buffer) == true)
        return Dev::RW_OK;
    else 
        return Dev::RW_ERROR;
}

bool SataDiskInterface::Read(uint64_t sector, uint32_t sectorCount, void* buffer)
{
    
    debugpln("(SataDiskInterface::Read)HIT");
    //osData.mainTerminalWindow->Log("This Interface: 0x{}", ConvertHexToString((uint64_t)this), Colors.yellow);
    uint8_t* buf = (uint8_t*)buffer;
    int32_t sectorCountDiv8 = ((sectorCount) / 8);
    debugpln("(SataDiskInterface::Read)HIT 2");
    for (int32_t sect = 0; sect < sectorCountDiv8; sect++)
    {
        _memset(Port->buffer, 0, 0x1000);
        debugpln("(SataDiskInterface::Read)HIT 3");
        if (!Port->Read(sector, 8, Port->buffer))
        {
            _memcpy(Port->buffer, buf, 0x1000);
            
            debugpln("(SataDiskInterface::Read)HIT ERROR 1");
            return false;
        }
        _memcpy(Port->buffer, buf, 0x1000);
        buf += 0x1000;
        sector += 8;
    }

    _memset(Port->buffer, 0, ((sectorCount % 8) << 9));
    if (!Port->Read(sector, sectorCount % 8, Port->buffer))
    {
        
        _memcpy(Port->buffer, buf, ((sectorCount % 8) << 9));
        debugpln("(SataDiskInterface::Read)HIT ERROR 2");
        return false;
    }
    _memcpy(Port->buffer, buf, ((sectorCount % 8) << 9));

    
    debugpln("(SataDiskInterface::Read)HIT OK!");
    return true;
}

bool SataDiskInterface::Write(uint64_t sector, uint32_t sectorCount, void* buffer)
{
    
    uint8_t* buf = (uint8_t*)buffer;
    int32_t sectorCountDiv8 = ((sectorCount) / 8);
    for (int32_t sect = 0; sect < sectorCountDiv8; sect++)
    {
        _memset(Port->buffer, 0, 0x1000);
        _memcpy(buf, Port->buffer, 0x1000);
        if (!Port->Write(sector, 8, Port->buffer))
        {
            
            return false;
        }
        buf += 0x1000;
        sector += 8;
    }

    _memset(Port->buffer, 0, ((sectorCount % 8) << 9));
    _memcpy(buf, Port->buffer, ((sectorCount % 8) << 9));
    if (!Port->Write(sector, sectorCount % 8, Port->buffer))
    {
        
        return false;
    }
    
    
    return true;
}


bool SataDiskInterface::ReadBytes(uint64_t address, uint64_t count, void* buffer)
{
    if (count == 0)
        return true;
    if (address + count > SectorCount * 512)
        return false;
    
    uint32_t tempSectorCount = ((((address + count) + 511) / 512) - (address / 512));
    uint8_t* buffer2 = (uint8_t*)kmalloc(tempSectorCount * 512);//"Malloc for Read Buffer"
    _memset(buffer2, 0, tempSectorCount * 512);

    if (!Read((address / 512), tempSectorCount, buffer2))
    {
        uint16_t offset = address % 512;
        for (uint64_t i = 0; i < count; i++)
            ((uint8_t*)buffer)[i] = buffer2[i + offset];

        kfree(buffer2);
        
        return false;
    }

    uint16_t offset = address % 512;
    for (uint64_t i = 0; i < count; i++)
        ((uint8_t*)buffer)[i] = buffer2[i + offset];
            
    kfree(buffer2);
    
    return true;
}

bool SataDiskInterface::WriteBytes(uint64_t address, uint64_t count, void* buffer)
{
    //Window* window = osData.mainTerminalWindow;
    
    if (count == 0)
        return true;
    if (address + count > SectorCount * 512)
        return false;
    
    uint32_t tempSectorCount = ((((address + count) + 511) / 512) - (address / 512));
    uint8_t* buffer2 = (uint8_t*)kmalloc(512); //Malloc for Write Buffer
    //window->Log("Writing Bytes...");
    
    if (tempSectorCount == 1)
    {
        //window->Log("Writing Bytes (1/1)");
        //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/1)");
        _memset(buffer2, 0, 512);
        if (!Read((address / 512), 1, buffer2))
        {
            kfree(buffer2);
            
            return false;
        }

        uint16_t offset = address % 512;
        for (uint64_t i = 0; i < count; i++)
            buffer2[i + offset] = ((uint8_t*)buffer)[i];

        if (!Write((address / 512), 1, buffer2))
        {
            kfree(buffer2);
            
            return false;
        }
        
        //free(buffer2);
    }
    else
    {
        //window->Log("Writing Bytes (1/3)");
        uint64_t newAddr = address;
        uint64_t newCount = count;
        uint64_t addrOffset = 0;
        {
            // GlobalRenderer->Clear(Colors.dgray);
            // while (true);


            //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/2)");
            _memset(buffer2, 0, 512);
            //window->Log("Writing to Sector: {}", to_string((address / 512)), Colors.yellow);
            if (!Read((address / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }

            uint16_t offset = address % 512;
            uint16_t specialCount = 512 - offset;
            addrOffset = specialCount;
            newAddr = address + specialCount;
            newCount = count - specialCount;

            // window->Log("Address:        {}", to_string(address), Colors.yellow);
            // window->Log("Count:          {}", to_string(count), Colors.yellow);
            // window->Log("New Address:    {}", to_string(newAddr), Colors.yellow);
            // window->Log("New Count:      {}", to_string(newCount), Colors.yellow);
            // window->Log("Address Offset: {}", to_string(addrOffset), Colors.yellow);

            for (uint64_t i = 0; i < specialCount; i++)
                buffer2[i + offset] = ((uint8_t*)buffer)[i];

            if (!Write((address / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }
            
            //free(buffer2);
        }

        //window->Log("Writing Bytes (2/3)");
        {
            //uint8_t* buffer2 = (uint8_t*)malloc(512, "Malloc for Read Buffer (1/2)");
            _memset(buffer2, 0, 512);
            //window->Log("Writing to Sector: {}", to_string((address + count - 1) / 512), Colors.yellow);
            if (!Read(((address + count) / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }

            uint16_t specialCount = ((address + count) % 512);
            //uint64_t offset = (address + count - specialCount);
            newCount -= specialCount;
            //newAddr = address + specialCount;

            // GlobalRenderer->Clear(Colors.black);

            // GlobalRenderer->Println("Address:        {}", to_string(address), Colors.yellow);
            // GlobalRenderer->Println("Count:          {}", to_string(count), Colors.yellow);
            // GlobalRenderer->Println("New Address:    {}", to_string(newAddr), Colors.yellow);
            // GlobalRenderer->Println("New Count:      {}", to_string(newCount), Colors.yellow);
            // //window->Log("Offset:         {}", to_string(offset), Colors.yellow);
            // GlobalRenderer->Println("Address Offset: {}", to_string(addrOffset), Colors.yellow);
            // GlobalRenderer->Println("Special Count:  {}", to_string(specialCount), Colors.yellow);

            uint64_t blehus = (count - specialCount);
            //GlobalRenderer->Println("blehus:  {}", to_string(blehus), Colors.yellow);

            // GlobalRenderer->Println("BUFF (1):");
            // for (int32_t i = 0; i < 512; i++)
            //     GlobalRenderer->Print("{} ", to_string((int32_t)buffer2[i]), Colors.yellow);
            // GlobalRenderer->Println("");

            for (int64_t i = 0; i < specialCount; i++)
                buffer2[i] = ((uint8_t*)buffer)[i + blehus];

            // GlobalRenderer->Println("BUFF (2):");
            // for (int32_t i = 0; i < 512; i++)
            //     GlobalRenderer->Print("{} ", to_string((int32_t)buffer2[i]), Colors.yellow);
            // GlobalRenderer->Println("");

            //while (true);

            if (!Write(((address + count) / 512), 1, buffer2))
            {
                kfree(buffer2);
                
                return false;
            }
            
            //free(buffer2);
        }
        
        //window->Log("Writing Bytes (3/3)");
        {
            uint64_t newSectorCount = newCount / 512;
            if (newSectorCount != 0)
            {
                uint64_t newSectorStartId = newAddr / 512;

                // window->Log("newSectorCount:    {}", to_string(newSectorCount), Colors.yellow);
                // window->Log("newSectorStartID:  {}", to_string(newSectorStartId), Colors.yellow);
                // window->Log("Address Offset:    {}", to_string(addrOffset), Colors.yellow);

                if (!Write(newSectorStartId, newSectorCount, (void*)((uint64_t)buffer + addrOffset)))
                {
                    kfree(buffer2);
                    
                    return false;
                }
            }
        }


        
    }
    kfree(buffer2);


    // free(buffer2);
    
    return true;
}

#endif