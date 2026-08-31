//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#ifdef __x86_64__
#include <drivers/ahci/ahci.h>
#include <drivers/Disk_Interfaces/sata/sataDiskInterface.h>
#include <klib/kio.h>
#include <mem/pmm.h>
#include <arch/x86_64/pit/pit.h>

/*  修复: #pragma GCC optimize("O0") 对 Clang 无效 —
   改用 optnone attribute (Clang/GCC 双兼容), 逐函数标注 */
#if defined(__clang__)
  #define AHCI_NOOPT __attribute__((optnone))
#else
  #define AHCI_NOOPT __attribute__((optimize("O0")))
#endif

namespace AHCI 
{
    #define HBA_PORT_DEV_PRESENT 0x3
    #define HBA_PORT_IPM_ACTIVE  0x1
    #define HBA_PxCMD_CR  0x8000
    #define HBA_PxCMD_FRE 0x0010
    #define HBA_PxCMD_ST  0x0001
    #define HBA_PxCMD_FR  0x4000
    #define HBA_PxIS_TFES (1 << 30)

    AHCI_NOOPT void Port::Configure()
    {
        StopCMD();

        // 1. Command List
        uint64_t cmd_list_phys = (uint64_t)PMM::Request();
        hbaPort->commandListBase = (uint32_t)cmd_list_phys;
        hbaPort->commandListBaseUpper = (uint32_t)(cmd_list_phys >> 32);
        _memset(HIGHER_HALF((void*)cmd_list_phys), 0, 1024);

        // 2. FIS Base
        uint64_t fis_phys = (uint64_t)PMM::Request();
        hbaPort->fisBaseAddress = (uint32_t)fis_phys;
        hbaPort->fisBaseAddressUpper = (uint32_t)(fis_phys >> 32);
        _memset(HIGHER_HALF((void*)fis_phys), 0, 256);
        
        HBACommandHeader* cmdHeader = (HBACommandHeader*)HIGHER_HALF((void*)cmd_list_phys);

        for (int32_t i = 0; i < 32; i++)
        {
            cmdHeader[i].prdtLength = 8;

            uint64_t cmd_tbl_phys = (uint64_t)PMM::Request();
            cmdHeader[i].commandTableBaseAddress = (uint32_t)cmd_tbl_phys;
            cmdHeader[i].commandTableBaseAddressUpper = (uint32_t)(cmd_tbl_phys >> 32);
            _memset(HIGHER_HALF((void*)cmd_tbl_phys), 0, 256);
        }

        StartCMD();
    }

    AHCI_NOOPT void Port::StopCMD()
    {
        hbaPort->cmdStatus &= ~HBA_PxCMD_ST;
        hbaPort->cmdStatus &= ~HBA_PxCMD_FRE;

        uint64_t deadline = PIT::TimeSinceBootMS() + 500;
        while (PIT::TimeSinceBootMS() < deadline)
        {
            if (hbaPort->cmdStatus & (HBA_PxCMD_FR | HBA_PxCMD_CR))
                continue;
            break;
        }
    }

    AHCI_NOOPT void Port::StartCMD()
    {
        uint64_t deadline = PIT::TimeSinceBootMS() + 500;
        while (PIT::TimeSinceBootMS() < deadline)
        {
            if (!(hbaPort->cmdStatus & HBA_PxCMD_CR))
                break;
        }

        hbaPort->cmdStatus |= HBA_PxCMD_FRE;
        hbaPort->cmdStatus |= HBA_PxCMD_ST;
    }

    AHCI_NOOPT SATA_Ident Port::Identifydrive()
    {
        /***Make the Command Header***/
        uint64_t cmd_list_phys = (uint64_t)hbaPort->commandListBase | ((uint64_t)hbaPort->commandListBaseUpper << 32);
        HBACommandHeader* cmdhead = (HBACommandHeader*)HIGHER_HALF((void*)cmd_list_phys);
        
        cmdhead->write = 0;
        cmdhead->prdtLength = 1;
        cmdhead->clearBusy = 1;
        cmdhead->commandFISLenght = sizeof(FIS_REG_H2D) / sizeof(uint32_t); 

        /***Make the Command Table***/
        uint64_t cmd_tbl_phys = (uint64_t)cmdhead->commandTableBaseAddress | ((uint64_t)cmdhead->commandTableBaseAddressUpper << 32);
        HBACommandTable* cmdtbl = (HBACommandTable*)HIGHER_HALF((void*)cmd_tbl_phys);
        
        static const SATA_Ident zero_ident = {};

        if (cmdtbl == nullptr)
            return zero_ident;

        uint64_t ident_phys = (uint64_t)PMM::Request();
        cmdtbl->prdtEntry[0].dataBaseAddress = (uint32_t)ident_phys;
        cmdtbl->prdtEntry[0].dataBaseAddressUpper = (uint32_t)(ident_phys >> 32);
        cmdtbl->prdtEntry[0].byteCount = 0x200 - 1;
        cmdtbl->prdtEntry[0].interruptOnCompletion = 1;

        /***Make the IDENTIFY DEVICE h2d FIS***/
        FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->commandFIS);
        _memset((void*)cmdfis, 0, sizeof(FIS_REG_H2D));
        cmdfis->fisType = FIS_TYPE_REG_H2D;
        cmdfis->commandControl = 1;
        cmdfis->command = ATA_CMD_IDENTIFY;
        
        /***Send the Command***/
        hbaPort->commandIssue |= 1;

        /***Wait for a reply***/
        uint64_t s = PIT::TimeSinceBootMS() + 3000;
        bool timeout = true;
        /* The syscall path runs with IRQs masked (syscall_entry does cli).
           Spinning on device completion with IRQs off monopolises this core
           for up to the whole timeout and starves peer threads pinned to it.
           Keep IRQs enabled while polling so the scheduler can preempt us, and
           restore the masked state expected by the syscall ABI on the way out. */
        asm volatile("sti");
        while(PIT::TimeSinceBootMS() < s)
        {
            if((hbaPort->commandIssue & 1) == 0)
            {
                timeout = false;
                break;
            }
            asm volatile("pause");
        }
        asm volatile("cli");
        
        if (timeout)
        {
            PMM::Free((void*)ident_phys);
            return zero_ident;
        }

        SATA_Ident test = *((SATA_Ident*)HIGHER_HALF((void*)ident_phys));

        PMM::Free((void*)ident_phys);

        return test;
    }

    AHCI_NOOPT int32_t Port::FindCommandSlot()
    {
        uint32_t cmdSlots = 32;
        uint32_t slots = (hbaPort->sataActive | hbaPort->commandIssue);
        for (int32_t i = 0; i < cmdSlots; i++)
        {
            if ((slots & 1) == 0)
                return i;
            slots >>= 1;
        }
        kerror("Could not find free Command Slot!\n");
        return -1;
    }
    
    AHCI_NOOPT bool Port::Read(uint64_t sector, uint32_t sectorCount, void* buffer)
    {
        uint32_t sectorL = (uint32_t)sector;
        uint32_t sectorH = (uint32_t)(sector >> 32);
        uint32_t sectorCountCopy = sectorCount;
        
        hbaPort->interruptStatus = (uint32_t)-1;
        int32_t slot = FindCommandSlot();
        if (slot == -1)
            return false;

        uint64_t cmd_list_phys = (uint64_t)hbaPort->commandListBase | ((uint64_t)hbaPort->commandListBaseUpper << 32);
        HBACommandHeader* cmdHeader = (HBACommandHeader*)HIGHER_HALF((void*)cmd_list_phys);
        cmdHeader += slot;
        cmdHeader->commandFISLenght = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
        cmdHeader->write = 0;
        cmdHeader->prdtLength = ((sectorCount) / 16) + 1;

        uint64_t cmd_tbl_phys = (uint64_t)cmdHeader->commandTableBaseAddress | ((uint64_t)cmdHeader->commandTableBaseAddressUpper << 32);
        HBACommandTable* commandTable = (HBACommandTable*)HIGHER_HALF((void*)cmd_tbl_phys);
        _memset(commandTable, 0, sizeof(HBACommandTable) + (cmdHeader->prdtLength - 1) * sizeof(HBAPRDTEntry));

        int32_t i = 0;
        for (i = 0; i < cmdHeader->prdtLength - 1; i++)
        {
            commandTable->prdtEntry[i].dataBaseAddress = (uint32_t)(uint64_t)buffer;
            commandTable->prdtEntry[i].dataBaseAddressUpper = (uint32_t)((uint64_t)buffer >> 32);
            commandTable->prdtEntry[i].byteCount = 0x2000 - 1;
            commandTable->prdtEntry[i].interruptOnCompletion = 1;
            buffer = (uint8_t*)buffer + 0x2000;
            sectorCount -= 16;
        }

        commandTable->prdtEntry[i].dataBaseAddress = (uint32_t)(uint64_t)buffer;
        commandTable->prdtEntry[i].dataBaseAddressUpper = (uint32_t)((uint64_t)buffer >> 32);
        commandTable->prdtEntry[i].byteCount = (sectorCount << 9) - 1;
        commandTable->prdtEntry[i].interruptOnCompletion = 1;
        
        FIS_REG_H2D* cmdFIS = (FIS_REG_H2D*)(&commandTable->commandFIS);
        cmdFIS->fisType = FIS_TYPE_REG_H2D;
        cmdFIS->commandControl = 1;
        cmdFIS->command = ATA_CMD_READ_DMA_EX;

        cmdFIS->lba0 = (uint8_t)sectorL;
        cmdFIS->lba1 = (uint8_t)(sectorL >> 8);
        cmdFIS->lba2 = (uint8_t)(sectorL >> 16);
        cmdFIS->lba3 = (uint8_t)(sectorL >> 24);
        cmdFIS->lba4 = (uint8_t)sectorH;
        cmdFIS->lba5 = (uint8_t)(sectorH >> 8);

        cmdFIS->deviceRegister = 1<<6; // LBA Mode

        cmdFIS->countLow = sectorCountCopy & 0xFF;
        cmdFIS->countHigh = (sectorCountCopy >> 8) & 0xFF;
        
        uint64_t spin = 0;
        while((hbaPort->taskFileData & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000)
            spin++;
        if (spin == 1000000)
            return false;

        hbaPort->commandIssue = 1 << slot;
        
        uint64_t deadline = PIT::TimeSinceBootMS() + 5000;
        /* The syscall path runs with IRQs masked (syscall_entry does cli).
           Spinning on device completion with IRQs off monopolises this core
           for up to the whole timeout and starves peer threads pinned to it.
           Keep IRQs enabled while polling so the scheduler can preempt us, and
           restore the masked state expected by the syscall ABI on the way out. */
        asm volatile("sti");
        while (PIT::TimeSinceBootMS() < deadline)
        {
            if ((hbaPort->commandIssue & (1<<slot)) == 0)
                break;
            if (hbaPort->interruptStatus & HBA_PxIS_TFES) {
                asm volatile("cli");
                return false;
            }
            asm volatile("pause");
        }
        asm volatile("cli");

        if (hbaPort->interruptStatus & HBA_PxIS_TFES) {
            return false;
        }

        return true;
    }

    AHCI_NOOPT bool Port::Write(uint64_t sector, uint32_t sectorCount, void* buffer)
    {
        uint32_t sectorL = (uint32_t)sector;
        uint32_t sectorH = (uint32_t)(sector >> 32);
        uint32_t sectorCountCopy = sectorCount;
        
        hbaPort->interruptStatus = (uint32_t)-1;
        int32_t slot = FindCommandSlot();
        if (slot == -1)
            return false;

        uint64_t cmd_list_phys = (uint64_t)hbaPort->commandListBase | ((uint64_t)hbaPort->commandListBaseUpper << 32);
        HBACommandHeader* cmdHeader = (HBACommandHeader*)HIGHER_HALF((void*)cmd_list_phys);
        cmdHeader += slot;
        cmdHeader->commandFISLenght = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
        cmdHeader->write = 1;
        cmdHeader->prdtLength = ((sectorCount) / 16) + 1;

        uint64_t cmd_tbl_phys = (uint64_t)cmdHeader->commandTableBaseAddress | ((uint64_t)cmdHeader->commandTableBaseAddressUpper << 32);
        HBACommandTable* commandTable = (HBACommandTable*)HIGHER_HALF((void*)cmd_tbl_phys);
        _memset(commandTable, 0, sizeof(HBACommandTable) + (cmdHeader->prdtLength - 1) * sizeof(HBAPRDTEntry));

        int32_t i = 0;
        for (i = 0; i < cmdHeader->prdtLength - 1; i++)
        {
            commandTable->prdtEntry[i].dataBaseAddress = (uint32_t)(uint64_t)buffer;
            commandTable->prdtEntry[i].dataBaseAddressUpper = (uint32_t)((uint64_t)buffer >> 32);
            commandTable->prdtEntry[i].byteCount = 0x2000 - 1;
            commandTable->prdtEntry[i].interruptOnCompletion = 1;
            buffer = (uint8_t*)buffer + 0x2000;
            sectorCount -= 16;
        }

        commandTable->prdtEntry[i].dataBaseAddress = (uint32_t)(uint64_t)buffer;
        commandTable->prdtEntry[i].dataBaseAddressUpper = (uint32_t)((uint64_t)buffer >> 32);
        commandTable->prdtEntry[i].byteCount = (sectorCount << 9) - 1;
        commandTable->prdtEntry[i].interruptOnCompletion = 1;
        
        FIS_REG_H2D* cmdFIS = (FIS_REG_H2D*)(&commandTable->commandFIS);
        cmdFIS->fisType = FIS_TYPE_REG_H2D;
        cmdFIS->commandControl = 1;
        cmdFIS->command = ATA_CMD_WRITE_DMA_EX;

        cmdFIS->lba0 = (uint8_t)sectorL;
        cmdFIS->lba1 = (uint8_t)(sectorL >> 8);
        cmdFIS->lba2 = (uint8_t)(sectorL >> 16);
        cmdFIS->lba3 = (uint8_t)(sectorL >> 24);
        cmdFIS->lba4 = (uint8_t)sectorH;
        cmdFIS->lba5 = (uint8_t)(sectorH >> 8);

        cmdFIS->deviceRegister = 1<<6; // LBA Mode

        cmdFIS->countLow = sectorCountCopy & 0xFF;
        cmdFIS->countHigh = (sectorCountCopy >> 8) & 0xFF;
        
        uint64_t spin = 0;
        while((hbaPort->taskFileData & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000)
            spin++;
        if (spin == 1000000)
            return false;

        hbaPort->commandIssue = 1<<slot;

        uint64_t deadline = PIT::TimeSinceBootMS() + 5000;
        /* The syscall path runs with IRQs masked (syscall_entry does cli).
           Spinning on device completion with IRQs off monopolises this core
           for up to the whole timeout and starves peer threads pinned to it.
           Keep IRQs enabled while polling so the scheduler can preempt us, and
           restore the masked state expected by the syscall ABI on the way out. */
        asm volatile("sti");
        while (PIT::TimeSinceBootMS() < deadline)
        {
            if ((hbaPort->commandIssue & (1<<slot)) == 0)
                break;
            if (hbaPort->interruptStatus & HBA_PxIS_TFES)
            { asm volatile("cli"); return false; }
            asm volatile("pause");
        }
        asm volatile("cli");

        if (hbaPort->interruptStatus & HBA_PxIS_TFES) 
                return false;

        return true;
    }

    AHCI_NOOPT uint64_t Port::GetMaxSectorCount()
    {
        if (!ident_cached)
        {
            SATA_Ident test = Identifydrive();
            cached_max_sector = test.lba_capacity;
            cached_sector_size = test.sector_bytes;
            ident_cached = true;
        }
        return cached_max_sector;
    }

    AHCI_NOOPT uint16_t Port::GetSectorSize()
    {
        if (!ident_cached)
            GetMaxSectorCount();
        return cached_sector_size;
    }

    AHCI_NOOPT AHCIDriver::AHCIDriver (PCI::PCIDeviceHeader* PCIBaseAddr){
        PCI::PCIDeviceHeader* pciBaseAddress = PCIBaseAddr;
        this->PCIBaseAddress = pciBaseAddress;
        kinfoln("> AHCIDriver has been created! (PCI: 0x%p)", (uint64_t)pciBaseAddress);

        PCI::PCIHeader0* hdr0 = (PCI::PCIHeader0*)(uint64_t)pciBaseAddress;

        /*  修复 1: 使能 Memory Space + Bus Master (经 Header.Command, offset 0x04) */
        uint16_t cmd = hdr0->Header.Command;
        cmd |= 0x6;                       // bit1=MSE, bit2=BusMaster
        hdr0->Header.Command = cmd;

        /*  修复 2: BAR5 解码 — 只保留一份正确实现:
           - 64 位判据: low 的 bit2 (PCI 规范)
           - upper 32 位: 配置空间 offset 0x28。
             PCIHeader0 结构里 0x28 是 CardbusCISPtr (AHCI 无 CIS,
             64 位 BAR 时固件把 upper 写在此处), 按偏移直接读最稳 */
        uint32_t low = hdr0->BAR5;
        kinfoln("AHCI: BAR5 raw=0x%x cmd=0x%x", low, (uint32_t)hdr0->Header.Command);

        uint64_t full_phys;
        if (low == 0 || low == 0xFFFFFFFF) {
            kerrorln("AHCI: BAR5 invalid (0x%x)!", low);
            this->ABAR = nullptr;
            this->PortCount = 0;
            return;
        }

        if (low & 0x4) {                  // bit2 = 64-bit BAR
            uint32_t high = *(volatile uint32_t*)((uint64_t)pciBaseAddress + 0x28);
            full_phys = ((uint64_t)high << 32) | (low & ~0xFULL);
        } else {
            full_phys = low & ~0xFULL;
        }

        kinfoln("AHCI: full_phys=0x%lx", (unsigned long)full_phys);

        this->ABAR = (HBAMemory*)(full_phys + hhdm_offset);

        /*  修复 3: ABAR 有效性验证 — version 必须在 0x1.0~0x2.x 范围,
           否则是映射错 (上次 PI==GHC==ver 全等就是这种) */
        uint32_t ver = ABAR->version;
        kinfoln("AHCI: ABAR=%p PI=%x GHC=%x ver=%x",
                (uint64_t)this->ABAR, ABAR->portsImplemented,
                ABAR->globalHostControl, ver);

        if (!(ver >= 0x00010000 && ver <= 0x0002FFFF)) {
            kerrorln("AHCI: version 0x%x invalid — ABAR mapping wrong (phys=0x%lx hhdm=0x%lx)",
                     ver, (unsigned long)full_phys, (unsigned long)hhdm_offset);
            this->ABAR = nullptr;
            this->PortCount = 0;
            return;
        }

        ABAR->globalHostControl |= 0x80000000;

        ProbePorts();

        kinfoln("Checking %d Ports:", PortCount);

        for (int32_t i = 0; i < PortCount; i++)
        {
            Port* port = Ports[i];
            PortType portType = port->portType;

            port->Configure();

            if (portType == PortType::SATA){
                kprintf("\033[38;2;255;165;0m* SATA drive\033[0m\n");
                SataDiskInterface* sataDiskInterface = new SataDiskInterface(port);
            }
        }
    }

    AHCIDriver::~AHCIDriver() {}

    #define SATA_SIG_ATAAPI 0xEB140101
    #define SATA_SIG_ATA    0x00000101
    #define SATA_SIG_SEMB   0xC33C0101
    #define SATA_SIG_PM     0x96690101

    AHCI_NOOPT void AHCIDriver::ProbePorts()
    {
        uint32_t portsImplemented = ABAR->portsImplemented;

        PortCount = 0;
        if (portsImplemented == 0) {
            kinfo("AHCI: PI == 0, no ports\n");
            return;
        }
        kinfo("AHCI: Probing ports, PI=0x%x\n", portsImplemented);

        for (int32_t i = 0; i < 32; i++)
        {
            if (portsImplemented & (1 << i))
            {
                PortType portType = CheckPortType(&ABAR->ports[i]);

                if (portType == PortType::SATA)
                    kprintf("\033[38;2;255;165;0m* SATA drive\033[0m\n");
                else if (portType == PortType::SATAPI)
                    kprintf("\033[38;2;255;165;0m* SATAPI drive\033[0m\n");
                else
                    kprintf("\033[38;2;255;165;0m* Not interested (det=%u ipm=%u sig=0x%x)\033[0m\n",
                            (unsigned)(ABAR->ports[i].sataStatus & 0b111),
                            (unsigned)((ABAR->ports[i].sataStatus >> 8) & 0b111),
                            (unsigned)ABAR->ports[i].signature);

                if (portType == PortType::SATA || portType == PortType::SATAPI)
                {
                    Ports[PortCount] = new Port();
                    Ports[PortCount]->portType = portType;
                    Ports[PortCount]->hbaPort = &ABAR->ports[i];
                    Ports[PortCount]->portNumber = PortCount;
                    PortCount++;
                }
            }
        }
    }

    AHCI_NOOPT PortType AHCIDriver::CheckPortType(HBAPort* port)
    {
        uint32_t sataStatus = port->sataStatus;

        uint8_t interfacePowerManagement = (sataStatus >> 8) & 0b111;
        uint8_t deviceDetection = sataStatus & 0b111;

        if (deviceDetection != HBA_PORT_DEV_PRESENT)
            return PortType::None;
        if (interfacePowerManagement != HBA_PORT_IPM_ACTIVE)
            return PortType::None;

        switch (port->signature)
        {
            case SATA_SIG_ATA:
                return PortType::SATA;
            case SATA_SIG_ATAAPI:
                return PortType::SATAPI;
            case SATA_SIG_PM:
                return PortType::PM;
            case SATA_SIG_SEMB:
                return PortType::SEMB;
            default:
                return PortType::None;
        }
    }
}

#endif