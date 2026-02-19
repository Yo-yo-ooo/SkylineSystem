/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pci.h
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
#include <acpi/acpi.h>
#include <stdint.h>
#include <stddef.h>
#include <arch/x86_64/dev/pci/pcidef.h>

#define CAPHDR_CAPID_MSI     0x05
#define CAPHDR_CAPID_MSIX    0x11

#define PCI_MSI_CAP_IS64(cap) (((cap)->Cap32.MsgCtrl >> 7) & 0x1)

#define PCI_MSIX_CAP_VecNum(cap) (((cap)->MsgCtrl & ((1u << 11) - 1)) + 1)
#define PCI_MSIX_CAP_BIR(cap) ((cap)->DW1 & 0x7)
#define PCI_MSIX_CAP_TableOff(cap) ((cap)->DW1 & ~0x7u)
#define PCI_MSIX_CAP_PendingBir(cap) ((cap)->DW2 & 0x7)
#define PCI_MSIX_CAP_PendingTblOff(cap) ((cap)->DW2 & ~0x7u)

namespace PCI
{
    void EnumeratePCI(ACPI::MCFGHeader* mcfg);

    void EnumerateBus(uint64_t baseAddress, uint64_t bus);

    void EnumerateDevice(uint64_t busAddress, uint64_t device);

    void EnumerateFunction(uint64_t deviceAddress, uint64_t function);

    void DoPCIWithoutMCFG();

    uint32_t read_pci0(uint32_t bus, uint32_t dev, uint32_t function,uint8_t registeroffset);

    extern const char* unknownString;

    const char* GetVendorName(uint16_t vendorID);

    const char* GetDeviceName(uint16_t vendorID, uint16_t deviceID);

    const char* GetClassName(uint8_t classCode);

    const char* GetSubclassName(uint8_t classCode, uint8_t subclassCode);

    const char* GetProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIFCode);

    PCI::PCIDeviceHeader* FindPCIDev(u8 Class,u8 SubClass,u8 ProgIF);
    //PCI::PCIDeviceHeader* FindPCIDev(u8 Class,u8 SubClass);

    uint8_t io_read_byte(uint64_t address, uint8_t field);
	uint16_t io_read_word(uint64_t address, uint8_t field);
	uint32_t io_read_dword(uint64_t address, uint8_t field);

	void io_write_byte(uint64_t address, uint8_t field, uint8_t value);
	void io_write_word(uint64_t address, uint8_t field, uint16_t value);
	void io_write_dword(uint64_t address, uint8_t field, uint32_t value);

	void enable_interrupt(uint64_t address);
	void disable_interrupt(uint64_t address);
	void enable_bus_mastering(uint64_t address);
	void disable_bus_mastering(uint64_t address);
    void enable_io_space(uint64_t address);
	void disable_io_space(uint64_t address);
    void enable_mem_space(uint64_t address);
	void disable_mem_space(uint64_t address);

    IOAddress get_address(PCIDeviceHeader* hdr, uint8_t field);
    IOAddress get_address(uint64_t addr, uint8_t field);

    void pci_read_bar(uint32_t* mask, uint64_t addr, uint32_t offset);
    PCI_BAR_TYPE pci_get_bar(uint32_t* bar0, int32_t bar_num, uint64_t addr);
    PCI_BAR_TYPE pci_get_bar(PCIHeader0* addr, int32_t bar_num);

    uint8_t read_byte(uint64_t address, PCI_BAR_TYPE type, uint8_t field);
	uint16_t read_word(uint64_t address, PCI_BAR_TYPE type, uint8_t field);
	uint32_t read_dword(uint64_t address, PCI_BAR_TYPE type, uint8_t field);

	void write_byte(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint8_t value);
	void write_word(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint16_t value);
	void write_dword(uint64_t address, PCI_BAR_TYPE type, uint16_t field, uint32_t value);

    PCI::PCICapHdr *GetNxtCap(PCIHeader0 *cfg, PCI::PCICapHdr *cur);
    PCI::PCI_MSIX_TABLE *GetMSIXTbl(PCI::PCI_MSIX_CAP *cap, PCIHeader0 *cfg);
    void MSI_CAP_SetVecNum(PCI::PCI_MSI_CAP *cap, u64 vecNum);
    namespace MSIX{
        void SetMsgAddr(uint64_t *msgAddr, uint32_t cpuId, uint32_t redirect, uint32_t destMode); 
    } // namespace MSIX
    
    namespace MSI{
        void SetMsgAddr(PCI::PCI_MSI_CAP *cap, uint32_t cpuId, uint32_t redirect, uint32_t destMode);
    } // namespace MSI
    bool SetMsi(PCI::PCI_MSI_CAP *cap, uint8_t vecID, uint32_t cpuID,uint64_t intrNum);
}