/*
* SPDX-License-Identifier: GPL-2.0-only
* File: acpi.cpp
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
#include <acpi/acpi.h>
#include <limine.h>

namespace ACPI{
    ACPI::MCFGHeader* mcfg = nullptr;
    int32_t ACPI_DIV = 0;
    ACPI::SDTHeader* rootThing = nullptr;
    volatile bool UseXSDT = false;

    void* FindTable(const char* signature){
        return ACPI::FindTable(ACPI::rootThing,signature,ACPI::ACPI_DIV);
    }

    void* FindTable(SDTHeader* sdtHeader, const char* signature, int32_t div)
    {
        
        /* ACPI::SDT *sdt = (uint64_t)rootThing;
        //uint32_t entry_size = 4;
        uint32_t entries = (sdtHeader->Length - sizeof(SDTHeader)) / div;
        for (uint32_t i = 0; i < entries; i++) {
            uint64_t address = 0;
            if (UseXSDT) address = *((uint64_t*)sdt->table + i);
            else address = *((uint32_t*)sdt->table + i);
            SDTHeader *header = HIGHER_HALF((SDTHeader*)address);
            if (!_memcmp(header->Signature, signature, 4))
                return (void*)header;
        } */

        SDTHeader* xsdt = sdtHeader;
        int32_t entries = (xsdt->Length - sizeof(ACPI::SDTHeader)) / div;
        //osData.debugTerminalWindow->Log("Entry count: {}", to_string(entries));

        //osData.debugTerminalWindow->renderer->Print("> ");
        for (int32_t t = 0; t < entries; t++)
        {
            uint64_t bleh1 = *(uint64_t*)((uint64_t)xsdt + sizeof(ACPI::SDTHeader) + (t * div));
            if (div == 4)
                bleh1 &= 0x00000000FFFFFFFF;
            ACPI::SDTHeader* newSDTHeader = (ACPI::SDTHeader*)(bleh1);
            
            for (int32_t i = 0; i < 4; i++)
            {
                if (newSDTHeader->Signature[i] != signature[i])
                {
                    break;
                }
                
                if (i == 3)
                {
                    
                    kinfo("FIND TABLE!\n");
                    return (void*)newSDTHeader;
                }
            }

            //osData.debugTerminalWindow->renderer->Print(" ");
        }
        //osData.debugTerminalWindow->renderer->Println(); 

        
        return nullptr;
    }

    u64 Init(void *addr) {
        

        
        kinfoln("Preparing ACPI...");
        kinfoln("RSDP Addr: %X", (uint64_t)addr);
        

        ACPI::RSDP2 *rsdp = (ACPI::RSDP2*)(addr);

           
        
        int32_t div = 1;

        if (rsdp->firstPart.Revision == 0)
        {
            kinfoln("ACPI Version: 1");
            rootThing = (ACPI::SDTHeader*)(uint64_t)HIGHER_HALF(rsdp->firstPart.RSDTAddress);
            kinfoln("RSDT Header Addr: %X", (uint64_t)rootThing);
            div = 4;

            if (rootThing == NULL){
                Panic("RSDT Header is at NULL!", true);
            }
            else{
                //GlobalRenderer->Clear(Colors.black);
                kinfoln("> Testing ACPI Loader...");

                //InitAcpiShutdownThing(rootThing);
                //while (true);
            }
        }
        else{
            kinfoln("ACPI Version: 2");
            rootThing = (ACPI::SDTHeader*)HIGHER_HALF(rsdp->XSDTAddress);

            kinfoln("XSDT Header Addr: %X", (uint64_t)rootThing);
            div = 8;
            UseXSDT = true;

            if (rootThing == NULL)
            {
                Panic("XSDT Header is at NULL!", true);
            }
        }
        

        
        
        int32_t entries = (rootThing->Length - sizeof(ACPI::SDTHeader)) / div;
        debugpln("Entry count: %d", entries);
        

        
        debugpln("> ");
        for (int32_t t = 0; t < entries; t++)
        {
            uint64_t bleh1 = *(uint64_t*)((uint64_t)rootThing + sizeof(ACPI::SDTHeader) + (t * div));
            if (div == 4)
                bleh1 &= 0x00000000FFFFFFFF;
            ACPI::SDTHeader* newSDTHeader = (ACPI::SDTHeader*)bleh1;
            
            char bruh[2];
            bruh[1] = 0;

            for (int32_t i = 0; i < 4; i++)
            {
                bruh[0] = newSDTHeader->Signature[i];
                debugpln("%d",bruh);
            }

            debugpln(" ");
        }
        debugpln("");
        

        
        ACPI::mcfg = (ACPI::MCFGHeader*)ACPI::FindTable(rootThing, (char*)"MCFG", div);
        //ASSERT(!PHYSICAL(ACPI::mcfg));
        ACPI_DIV = div;
        kinfoln("ACPI_DIV: %d",div);

        kinfoln("MCFG Header Addr: %X", (uint64_t)ACPI::mcfg);
    
        
        return 1;
    }
}