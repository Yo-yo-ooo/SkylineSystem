/*
* SPDX-License-Identifier: GPL-2.0-only
* File: intr.cpp
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
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/interrupt/idt.h>
#include <klib/kio.h>

static u16 *MSICAP_MSGDATA(PCI::PCI_MSI_CAP *cap) {
    return PCI_MSI_CAP_IS64(cap) ? &cap->Cap64.MsgData : &cap->Cap32.MsgData;
}
namespace PCI
{
    void SetMsgData16(uint16_t *msgData, uint32_t vec, 
        uint32_t deliverMode, uint32_t level, uint32_t triggerMode) {
        *msgData = vec | (deliverMode << 8) | (level << 14) | (triggerMode << 15);
    }

    void SetMsgData32(uint32_t *msgData, uint32_t vec, 
        uint32_t deliverMode, uint32_t level, uint32_t triggerMode) {
        *msgData = vec | (deliverMode << 8) | (level << 14) | (triggerMode << 15);
    }
    namespace MSIX
    {
        //redirect = 0 : 直接投递给指定的 CPU。
        //redirect = 1 : 允许中断控制器根据负载均衡等策略重新分发。
        void SetMsgAddr(uint64_t *msgAddr, uint32_t cpuId, uint32_t redirect, uint32_t destMode) {
            *msgAddr = 0xfee00000u
                | (((cpuId) & 0xFF) << 12)
                | (redirect << 3) | (destMode << 2);
        }

        void ConfigMSIX(
        PCI::PCIHeader0 *Hdr,uint32_t CpuId, uint32_t Redirect, 
        uint32_t DestMode,uint16_t TblIdx,uint16_t INTRNUM,
        void (*Handler)(context_t*)){
            PCI::PCI_MSIX_CAP *Cap = PCI::GetMSIXCap(Hdr);
            uint32_t MsgAddr = 0xfee00000u
                | ((CpuId) << 12)
                | (Redirect << 3) | (DestMode << 2);
            PCI::PCI_MSIX_TABLE* Tbl = PCI::GetMSIXTblBaseAddr(Hdr,Cap);
            
            Tbl[TblIdx].msgAddr = MsgAddr;
            Tbl[TblIdx].msgData = INTRNUM;
            __asm__ volatile ("mfence" ::: "memory"); // 确保硬件看到地址和数据
            Tbl[TblIdx].vecCtrl &= ~1u; // 解除ENTRY[INTRNUM]'s Mask
            
            idt_install_irq_cpu(CpuId,INTRNUM,Handler);

            PCI::enable_bus_mastering((uint64_t)Hdr);
            Cap->MsgCtrl |= (1 << 15); // Enable
            Cap->MsgCtrl &= ~(1 << 14); // Unmask all
        }
    } // namespace MSIX
    
    namespace MSI{
        void SetMsgAddr(PCI::PCI_MSI_CAP *cap, uint32_t cpuId, uint32_t redirect, uint32_t destMode) {
            uint32_t val = 0xfee00000u
                | ((cpuId) << 12)
                | (redirect << 3) | (destMode << 2);
            if (PCI_MSI_CAP_IS64(cap)) cap->Cap64.MsgAddr = val;
            else cap->Cap32.MsgAddr = val;
        }    
    }
}
