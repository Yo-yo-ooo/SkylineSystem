#include <acpi/acpi.h>
#include "../limine.h"



namespace ACPI{
    ACPI::MCFGHeader* mcfg = NULL;
    int ACPI_DIV = 0;
    ACPI::SDTHeader* rootThing = NULL;

    void* FindTable(SDTHeader* sdtHeader, const char* signature, int div)
    {
        AddToStack();
        SDTHeader* xsdt = sdtHeader;
        int entries = (xsdt->Length - sizeof(ACPI::SDTHeader)) / div;
        //osData.debugTerminalWindow->Log("Entry count: {}", to_string(entries));

        //osData.debugTerminalWindow->renderer->Print("> ");
        for (int t = 0; t < entries; t++)
        {
            uint64_t bleh1 = *(uint64_t*)((uint64_t)xsdt + sizeof(ACPI::SDTHeader) + (t * div));
            if (div == 4)
                bleh1 &= 0x00000000FFFFFFFF;
            ACPI::SDTHeader* newSDTHeader = (ACPI::SDTHeader*)(bleh1);
            
            for (int i = 0; i < 4; i++)
            {
                if (newSDTHeader->Signature[i] != signature[i])
                {
                    break;
                }
                
                if (i == 3)
                {
                    RemoveFromStack();
                    return newSDTHeader;
                }
            }

            //osData.debugTerminalWindow->renderer->Print(" ");
        }
        //osData.debugTerminalWindow->renderer->Println();
        RemoveFromStack();
        return NULL;
    }

    u64 Init(void *addr) {
        AddToStack();

        AddToStack();
        kinfoln("Preparing ACPI...");
        kinfoln("RSDP Addr: %X", (uint64_t)addr);
        RemoveFromStack();

        ACPI::RSDP2 *rsdp = (ACPI::RSDP2*)(addr);

        AddToStack();   
        
        int div = 1;

        if (rsdp->firstPart.Revision == 0)
        {
            kinfoln("ACPI Version: 1");
            rootThing = (ACPI::SDTHeader*)(uint64_t)HIGHER_HALF(rsdp->firstPart.RSDTAddress);
            kinfoln("RSDT Header Addr: %X", (uint64_t)rootThing);
            div = 4;

            if (rootThing == NULL)
            {
                Panic("RSDT Header is at NULL!", true);
            }
            else
            {
                //GlobalRenderer->Clear(Colors.black);
                kinfoln("> Testing ACPI Loader...");

                //InitAcpiShutdownThing(rootThing);
                //while (true);
            }
        }
        else
        {
            kinfoln("ACPI Version: 2");
            rootThing = (ACPI::SDTHeader*)HIGHER_HALF(rsdp->XSDTAddress);

            kinfoln("XSDT Header Addr: %X", (uint64_t)rootThing);
            div = 8;

            if (rootThing == NULL)
            {
                Panic("XSDT Header is at NULL!", true);
            }
        }
        RemoveFromStack();

        
        AddToStack();
        int entries = (rootThing->Length - sizeof(ACPI::SDTHeader)) / div;
        kinfoln("Entry count: %d", entries);
        RemoveFromStack();

        AddToStack();
        kinfoln("> ");
        for (int t = 0; t < entries; t++)
        {
            uint64_t bleh1 = *(uint64_t*)((uint64_t)rootThing + sizeof(ACPI::SDTHeader) + (t * div));
            if (div == 4)
                bleh1 &= 0x00000000FFFFFFFF;
            ACPI::SDTHeader* newSDTHeader = (ACPI::SDTHeader*)bleh1;
            
            char bruh[2];
            bruh[1] = 0;

            for (int i = 0; i < 4; i++)
            {
                bruh[0] = newSDTHeader->Signature[i];
                kinfoln("%d",bruh);
            }

            kinfoln(" ");
        }
        kinfoln("");
        RemoveFromStack();

        AddToStack();
        ACPI::mcfg = (ACPI::MCFGHeader*)ACPI::FindTable(rootThing, (char*)"MCFG", div);
        ACPI_DIV = div;

        kinfoln("MCFG Header Addr: %X", (uint64_t)ACPI::mcfg);
        RemoveFromStack();

        if (ACPI::mcfg == NULL)
        {
            RemoveFromStack();
            return;
        }
    
        RemoveFromStack();

        //PrintMsgEndLayer("ACPI");
        return 1;
    }
}