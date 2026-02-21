/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pci.cpp
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
#include <arch/x86_64/dev/pci/pci.h>
#include <stddef.h>
#include <stdint.h>
#include <klib/klib.h>
#include <klib/kio.h>
#include <klib/kprintf.h>
#include <klib/cstr.h>
#include <acpi/acpi.h>
#include <arch/x86_64/vmm/vmm.h>
#include <conf.h>
#include <klib/klib.h>
#include <klib/algorithm/stl/vector.h>
#include <klib/algorithm/hmap.h>
#include <klib/rnd.h>
/* PCI::PCIDeviceHeader* pciDevices[PCI_DEVICE_MAX] = {0};
uint32_t pciDeviceidx = 0; */
volatile static struct hashmap *PCIDevMap = nullptr;

PACK(typedef struct _MapIdent_32b{
    uint8_t RSVD = 0;
    uint8_t Class;
    uint8_t SubClass;
    uint8_t ProgIF;
})_MapIdent_32b;

typedef struct PCIDevMapS{
    uint64_t PCIDevBaseAddr;
    _MapIdent_32b mi;
}PCIDevMapS;

int32_t pdev_compare(const void *a, const void *b, void *udata) {
    uint32_t va = *(volatile uint32_t*)&((const PCIDevMapS*)a)->mi;
    uint32_t vb = *(volatile uint32_t*)&((const PCIDevMapS*)b)->mi;
    
    return (va < vb) ? -1 : (va > vb);
}

bool pdev_iter(const void *item, void *udata) {
    return true;
}

uint64_t pdev_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const PCIDevMapS *A = (PCIDevMapS*)item;
    return hashmap_sip(&A->mi,4,seed0,seed1);
}


namespace PCI
{

    uint32_t read_pci0(uint32_t bus, uint32_t dev, uint32_t function,uint8_t registeroffset){
        uint32_t id = 1U << 31 | ((bus & 0xff) << 16) | ((dev & 0x1f) << 11) |
                      ((function & 0x07) << 8) | (registeroffset & 0xfc);
        io_out32(PCI_COMMAND_PORT, id);
        uint32_t result = io_in32(PCI_DATA_PORT);
        return result >> (8 * (registeroffset % 4));
    }

    void DoPCIWithoutMCFG(){
        uint32_t BUS, Equipment, F,PCI_NUM = 0;
        for (BUS = 0; BUS < 256; BUS++) {
            for (Equipment = 0; Equipment < 32; Equipment++) {
                for (F = 0; F < 8; F++) {
                    //pci_config0(BUS,F,Equipment,0);
                    uint32_t cmd = 0;
                    cmd = 0x80000000 + (uint32_t) 0 + ((uint32_t) F << 8) +
                        ((uint32_t) Equipment << 11) + ((uint32_t) BUS << 16);
                    // cmd = cmd | 0x01;
                    __asm__ volatile("outl %0, %w1" : : "a"(PCI_COMMAND_PORT), "Nd"(cmd));
                    if(io_in32(PCI_DATA_PORT) != 0xFFFFFFFF){
                        PCI_NUM++;
                        //load_pci_device(BUS,Equipment,F);
                        uint32_t value_c = PCI::read_pci0(BUS, Equipment, F, PCI_CONF_REVISION);
                        uint32_t class_code = value_c >> 8;

                        uint16_t value_v = PCI::read_pci0(BUS, Equipment, F, PCI_CONF_VENDOR);
                        uint16_t value_d = PCI::read_pci0(BUS, Equipment, F, PCI_CONF_DEVICE);
                        uint16_t vendor_id = value_v & 0xffff;
                        uint16_t device_id = value_d & 0xffff;

                        PCIDeviceHeader* device = (PCIDeviceHeader*)kmalloc(sizeof(PCIDeviceHeader));
                        device->Vendor_ID = vendor_id;
                        device->Device_ID = device_id;
                        device->Class = class_code;
                        device->Prog_IF = io_in8(PCI_PROG_IF);
                        device->SubClass = io_in8(PCI_SUBCLASS);

                        /* if (pciDeviceidx > PCI_DEVICE_MAX) {
                            kwarn("PCI:add device full %d", pciDeviceidx);
                            return;
                        } */
                        //pciDevices[pciDeviceidx++] = device;

                        if(device->Class == 0x060400 || (device->Class & 0xFFFF00) == 0x060400){
                            return;
                        }
                    }
                }
            }
        }
        kinfo("PCI device loaded: %d", PCI_NUM);
    }

    void EnumeratePCI(ACPI::MCFGHeader* mcfg)
    {
        
        uint32_t entries = (ACPI::mcfg->Header.Length - sizeof(ACPI::MCFGHeader)) / sizeof(ACPI::DeviceConfig);

        //kinfoln("PCI HIT! 2");

        //_memset(pciDevices, 0, sizeof(PCI::PCIDeviceHeader) * 128);
        /* for(uint8_t i = 0;i < PCI_LIST_MAX;i++)
            pciDevices[i] = {0}; 
        pciDeviceidx = 0; */

        PCIDevMap = hashmap_new(sizeof(PCIDevMapS),256,RND::RandomInt(),RND::RandomInt(),
            pdev_hash,pdev_compare,NULL,NULL);
        

        //kinfoln("PCI HIT! 3");
        //kinfoln("ENTRIES: %d",entries);

        for (uint32_t t = 0; t < entries; t++)
        {
            //kinfoln("PCI HIT %d",t);
            ACPI::DeviceConfig* newDeviceConfig = (ACPI::DeviceConfig*)((uint64_t)ACPI::mcfg + sizeof(ACPI::MCFGHeader) + sizeof(ACPI::DeviceConfig) * t);
            //kinfoln("PCI HIT! 4");
            for (uint8_t bus = newDeviceConfig->StartBus; bus < newDeviceConfig->EndBus; bus++)
                EnumerateBus((newDeviceConfig->BaseAddress), bus);
        }
    }

    void EnumerateBus(uint64_t baseAddress, uint64_t bus)
    {
        
        uint64_t offset = bus << 20;
        uint64_t busAddress = baseAddress + offset;

        
        if (busAddress == 0){return;}

        VMM::Map((void*)(busAddress), (void*)(busAddress));
        //kinfoln("PCI HIT! 5");
        
        PCIDeviceHeader* pciDeviceHeader  = (PCIDeviceHeader*)HIGHER_HALF(busAddress);

        if (pciDeviceHeader ->Device_ID == 0x0000) { return;}
        if (pciDeviceHeader ->Device_ID == 0xFFFF) { return;}

        //kinfoln("PCI HIT! 5-1");

        for (uint8_t device = 0; device < 32; device++)
        {
            EnumerateDevice(busAddress, device);
        }
        
    }

    void EnumerateDevice(uint64_t busAddress, uint64_t device) // Slot
    {
        
        uint64_t offset = device << 15;

        uint64_t deviceAddress = busAddress + offset;

        if (deviceAddress == 0)
        {
            
            return;
        }

        VMM::Map((void*)(deviceAddress), (void*)(deviceAddress));
        //kinfoln("PCI HIT! 6");
        
        PCIDeviceHeader* pciDeviceHeader  = (PCIDeviceHeader*)HIGHER_HALF(deviceAddress);

        if (pciDeviceHeader ->Device_ID == 0x0000) { return;}
        if (pciDeviceHeader ->Device_ID == 0xFFFF) { return;}

        for (uint8_t function = 0; function < 8; function++)
        {
            EnumerateFunction(deviceAddress, function);
        }
        
    }

    void EnumerateFunction(uint64_t deviceAddress, uint64_t function)
    {
        
        uint64_t offset = function << 12;

        uint64_t functionAddress = deviceAddress + offset;

        if (functionAddress == 0)
        {
            
            return;
        }

        VMM::Map((void*)(functionAddress), (void*)(functionAddress));
        //kinfoln("PCI HIT! 7");
        
        PCIDeviceHeader* pciDeviceHeader  = (PCIDeviceHeader*)HIGHER_HALF(functionAddress);

        if (pciDeviceHeader ->Device_ID == 0x0000) { return;}
        if (pciDeviceHeader ->Device_ID == 0xFFFF) { return;}

        //BasicRenderer* renderer = osData.debugTerminalWindow->renderer;
        kprintf("%s"," > ");

        {
            const char* vendorName = GetVendorName(pciDeviceHeader->Vendor_ID);
            if (vendorName != unknownString)
                kprintf("%s",vendorName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Vendor_ID));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* deviceName = GetDeviceName(pciDeviceHeader->Vendor_ID, pciDeviceHeader->Device_ID);
            if (deviceName != unknownString)
                kprintf("%s",deviceName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Device_ID));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* className = GetClassName(pciDeviceHeader->Class);
            if (className != unknownString)
                kprintf("%s",className);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Class));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* subclassName = GetSubclassName(pciDeviceHeader->Class, pciDeviceHeader->SubClass);
            if (subclassName != unknownString)
                kprintf("%s",subclassName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->SubClass));
                kprintf("%s",">");
            }
            kprintf("%s"," / ");
        }

        {
            const char* progIFName = GetProgIFName(pciDeviceHeader->Class, pciDeviceHeader->SubClass, pciDeviceHeader->Prog_IF);
            if (progIFName != unknownString)
                kprintf("%s",progIFName);
            else
            {
                kprintf("%s","<");
                kprintf("%s",ConvertHexToString(pciDeviceHeader->Prog_IF));
                kprintf("%s",">");
            }
            //renderer->Print(" / ");
        }
        kprintf("\n");
        //pciDevices[pciDeviceidx] = pciDeviceHeader;
        PCIDevMapS ms = {0};
        ms.mi.Class = pciDeviceHeader->Class;
        ms.mi.SubClass = pciDeviceHeader->SubClass;
        ms.mi.ProgIF = pciDeviceHeader->Prog_IF;
        ms.mi.RSVD = 0;
        //kinfoln("%X",(uint64_t)pciDeviceHeader);
        ms.PCIDevBaseAddr = (uint64_t)pciDeviceHeader;
        hashmap_set(PCIDevMap,&ms);

        return;
    }

    PCI::PCIDeviceHeader* FindPCIDev(u8 Class,u8 SubClass,u8 ProgIF){
        /* for (u8 i = 0; i < PCI_LIST_MAX; i++){
            if(pciDevices[i]->Class == Class 
                && pciDevices[i]->SubClass == SubClass 
                && pciDevices[i]->Prog_IF == ProgIF){
                return pciDevices[i];
            }
        } */
        PCIDevMapS ps;
        ps.mi.Class = Class;
        ps.mi.SubClass = SubClass;
        ps.mi.ProgIF = ProgIF;
        ps.mi.RSVD = 0;
        PCIDevMapS *p = (PCIDevMapS*)hashmap_get(PCIDevMap,&ps);
        return (PCI::PCIDeviceHeader*)p->PCIDevBaseAddr;
    }

    /* PCI::PCIDeviceHeader* FindPCIDev(u8 Class,u8 SubClass){
        for (u8 i = 0; i < PCI_LIST_MAX; i++){
            if(pciDevices[i]->Class == Class 
            && pciDevices[i]->SubClass == SubClass)
                return pciDevices[i];
        }
    } */


    IOAddress get_address(PCIDeviceHeader* hdr, uint8_t field)
    {
        uint64_t addr = (uint64_t)hdr;
        return get_address(addr, field);
    }

    IOAddress get_address(uint64_t addr, uint8_t field)
    {
        IOAddress address;
        address.attrs.field = field;
        address.attrs.function = (addr >> 12) & 0b111; // 3 bit
        address.attrs.slot = (addr >> 15) & 0b11111; // 5 bit
        address.attrs.bus = (addr >> 20) & 0b11111111; // 8 bit
        address.attrs.enable = true;
        //0x80000000 
        return address;
    }

	uint8_t io_read_byte(uint64_t address, uint8_t field) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		return inb(PCI_DATA_PORT + (field & 3));
	}

	uint16_t io_read_word(uint64_t address, uint8_t field){
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		return inw(PCI_DATA_PORT + (field & 2));
	}

	uint32_t io_read_dword(uint64_t address, uint8_t field) {
	    outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		return inl(PCI_DATA_PORT);
	}

	void io_write_byte(uint64_t address, uint8_t field, uint8_t value) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		outb(PCI_DATA_PORT + (field & 3), value);
	}

	void io_write_word(uint64_t address, uint8_t field, uint16_t value) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		outw(PCI_DATA_PORT + (field & 2), value);
	}

	void io_write_dword(uint64_t address, uint8_t field, uint32_t value) {
		outl(PCI_ADDRESS_PORT, get_address(address, field).value);
		outl(PCI_DATA_PORT, value);
	}

	void enable_interrupt(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.interrupt_disable = false;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

	void disable_interrupt(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.interrupt_disable = true;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

	void enable_bus_mastering(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.bus_master = true;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

	void disable_bus_mastering(uint64_t address) {
		Command comm = {.value = io_read_word(address, PCI_COMMAND)};
		comm.attrs.bus_master = false;
		io_write_word(address, PCI_COMMAND, comm.value);
	}

    void enable_io_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.io_space = true;
        io_write_word(address, PCI_COMMAND, comm.value);
    }
	void disable_io_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.io_space = false;
        io_write_word(address, PCI_COMMAND, comm.value);
    }
    void enable_mem_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.mem_space = true;
        io_write_word(address, PCI_COMMAND, comm.value);
    }
	void disable_mem_space(uint64_t address)
    {
        Command comm = {.value = io_read_word(address, PCI_COMMAND)};
        comm.attrs.mem_space = false;
        io_write_word(address, PCI_COMMAND, comm.value);
    }

    void pci_read_bar(uint32_t* mask, uint64_t addr, uint32_t offset)
    {
        uint32_t data = io_read_dword(addr, offset);
        io_write_dword(addr, offset, 0xffffffff);
        *mask = io_read_dword(addr, offset);
        io_write_dword(addr, offset, data);
    }

    PCI_BAR_TYPE pci_get_bar(uint32_t* bar0, int32_t bar_num, uint64_t addr)
    {
        PCI_BAR_TYPE bar;
        bar.io_address = 0;
        bar.size = 0;
        bar.mem_address = 0;
        bar.type = NONE;

        uint32_t* bar_ptr = bar0 + bar_num;

        if (*bar_ptr == 0) 
        {
            bar.type = NONE;
            return bar;
        }

        uint32_t mask;
        pci_read_bar(&mask, addr, bar_num * sizeof(uint32_t));

        if (*bar_ptr & 0x04) 
        { //64-bit mmio
            bar.type = MMIO64;

            uint32_t* bar_ptr_high = bar0 + bar_num + 4;
            uint32_t mask_high;
            pci_read_bar(&mask_high, addr, (bar_num * sizeof(uint32_t)) + 0x4);

            bar.mem_address = ((uint64_t) (*bar_ptr_high & ~0xf) << 32) | (*bar_ptr & ~0xf);
            bar.size = (((uint64_t) mask_high << 32) | (mask & ~0xf)) + 1;
        } 
        else if (*bar_ptr & 0x01) 
        { //IO
            bar.type = IO;

            bar.io_address = (uint16_t)(*bar_ptr & ~0x3);
            bar.size = (uint16_t)(~(mask & ~0x3) + 1);
        } 
        else 
        { //32-bit mmio
            bar.type = MMIO32;

            bar.mem_address = (uint64_t) *bar_ptr & ~0xf;
            bar.size = ~(mask & ~0xf) + 1;
        }
        

        return bar;
    }

    PCI_BAR_TYPE pci_get_bar(PCIHeader0* addr, int32_t bar_num)
    {
        return pci_get_bar(&(addr->BAR0), bar_num, (uint64_t)addr);
    }

    uint8_t read_byte(uint64_t address, PCI_BAR_TYPE type, uint8_t field)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            return *(uint8_t*)(type.mem_address + field);
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            return inb(type.io_address + field);
        return 0;
    }

	uint16_t read_word(uint64_t address, PCI_BAR_TYPE type, uint8_t field)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            return *(uint16_t*)(type.mem_address + field);
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            return inw(type.io_address + field);
        return 0;
    }

	uint32_t read_dword(uint64_t address, PCI_BAR_TYPE type, uint8_t field)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            return *(uint32_t*)(type.mem_address + field);
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            return inl(type.io_address + field);
        return 0;
    }

	void write_byte(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint8_t value)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            *(uint8_t*)(type.mem_address + field) = value;
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            outb(type.io_address + field, value);
    }

	void write_word(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint16_t value)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            *(uint16_t*)(type.mem_address + field) = value;
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            outw(type.io_address + field, value);
    }

	void write_dword(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint32_t value)
    {
        if (type.type == PCI_BAR_TYPE_ENUM::MMIO32 || type.type == PCI_BAR_TYPE_ENUM::MMIO64)
            *(uint32_t*)(type.mem_address + field) = value;
        else if (type.type == PCI_BAR_TYPE_ENUM::IO)
            outl(type.io_address + field, value);
    }

    PCI::PCICapHdr *GetNxtCap(PCIHeader0 *cfg, PCI::PCICapHdr *cur) {
        if (cur) return cur->NextPTR ? (PCI::PCICapHdr *)((u64)cfg + cur->NextPTR) : NULL;
        return (PCI::PCICapHdr *)((u64)cfg + cfg->CapabilitiesPtr);
    }

    PCI::PCI_MSIX_TABLE *GetMSIXTbl(PCI::PCI_MSIX_CAP *cap, PCIHeader0 *cfg) {
        return (PCI::PCI_MSIX_TABLE*)PCI::GetMSIXTblBaseAddr(cfg,cap);
    }

    void MSI_CAP_SetVecNum(PCI::PCI_MSI_CAP *cap, u64 vecNum) {
        // set log2(vecNum) to msgCtrl
        u32 n = 1;
        __asm__ volatile (
            "bsfl %1, %%eax				\n\t"
            "movl $0xffffffff, %%edx	\n\t"
            "cmove %%edx, %%eax			\n\t"
            "addl $1, %%eax				\n\t"
            "movl %%eax, %0				\n\t"
            : "=m"(n)
            : "m"(vecNum)
            : "memory", "rax", "rdx"
        );
        //return n;
        cap->Cap32.MsgCtrl = (cap->Cap32.MsgCtrl & ~(0x7u << 4)) | ((n - 1) << 4);
    }

    PCI::MSI_CAP32* GetMSICap(PCI::PCIHeader0 *Header){
        uint64_t BaseAddr = (uint64_t)Header;
        uint8_t *Ptr = (uint8_t*)(Header->CapabilitiesPtr + BaseAddr);
        for(uint8_t i = 0;Ptr[1] != 0;i++){
            if(Ptr[0] == 0x5){ //PCI MSI Capability ID
                return (PCI::MSI_CAP32*)((uint64_t)Ptr);
            }
            Ptr = (uint8_t*)(BaseAddr + Ptr[1]);
        }
    }

    PCI::PCI_MSIX_CAP* GetMSIXCap(PCI::PCIHeader0 *Header){
        uint64_t BaseAddr = (uint64_t)Header;
        uint8_t *Ptr = (uint8_t*)(Header->CapabilitiesPtr + BaseAddr);
        for(uint8_t i = 0;Ptr[1] != 0;i++){
            if(Ptr[0] == 0x11){ //PCI MSIX Capability ID
                return (PCI::PCI_MSIX_CAP*)((uint64_t)Ptr);
            }
            Ptr = (uint8_t*)(BaseAddr + Ptr[1]);
        }
    }

    uint64_t GetMSIXTblBaseAddr(PCI::PCIHeader0 *Header,PCI::PCI_MSIX_CAP *CapPtr){
        uint8_t TblBIR = (uint8_t)(CapPtr->DW1 & 0x07);
        uint32_t TblOffset = CapPtr->DW1 >> 3;
        uint32_t BAR_LOW = Header->BAR[TblBIR];
        uint64_t PhyAddr = BAR_LOW & ~0xFULL;
        uint64_t HdrAddr = (uint64_t)Header;
        if(((BAR_LOW >> 1) & 0x3) == 0x2){
            uint32_t BARH = *(uint32_t*)(HdrAddr + 20 + TblBIR * 4);
            PhyAddr |= ((uint64_t)BARH << 32);
        }

        return PhyAddr + hhdm_offset + TblOffset;
    }

    uint64_t GetPBABaseAddr(PCI::PCIHeader0 *Header,PCI::PCI_MSIX_CAP *CapPtr){
        /* uint8_t PBABIR = (uint8_t)(CapPtr->DW2 & 0x07);
        uint32_t PBAOffset = CapPtr->DW2 >> 3;
        return (uint64_t)(((uint64_t)Header->BAR[PBABIR] + hhdm_offset) + (uint64_t)PBAOffset); */
        uint8_t PBABIR = (uint8_t)(CapPtr->DW2 & 0x07);
        uint32_t PBAOffset = CapPtr->DW2 >> 3;
        uint32_t BAR_LOW = Header->BAR[PBABIR];
        uint64_t PhyAddr = BAR_LOW & ~0xFULL;
        uint64_t HdrAddr = (uint64_t)Header;
        if(((BAR_LOW >> 1) & 0x3) == 0x2){
            uint32_t BARH = *(uint32_t*)(HdrAddr + 20 + PBABIR * 4);
            PhyAddr |= ((uint64_t)BARH << 32);
        }

        return PhyAddr + hhdm_offset + PBAOffset;
    }

    uint8_t *FindCapability(PCI::PCIHeader0 *Hdr,uint8_t CapID){
        uint64_t BaseAddr = (uint64_t)Hdr;
        uint8_t *Ptr = (uint8_t*)(Hdr->CapabilitiesPtr + BaseAddr);
        for(uint8_t i = 0;Ptr[1] != 0;i++){
            if(Ptr[0] == CapID){ //PCI XXX Capability ID
                return (uint8_t*)((uint64_t)Ptr);
            }
            Ptr = (uint8_t*)(BaseAddr + Ptr[1]);
        }
    }

}
