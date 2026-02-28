/*
* SPDX-License-Identifier: GPL-2.0-only
* File: init.cpp
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
#include <arch/aarch64/asm/mair.h>
#include <arch/aarch64/asm/regs.h>
#include <arch/aarch64/vmm/vmm.h>
#include <mem/pmm.h>
#include <klib/kprintf.h>
#include <pdef.h>
#include <klib/kio.h>
#include <klib/klib.h>
#include <conf.h>



void setup_mair() {
    // Device nGnRnE
    const uint64_t device_uncacheable_encoding = 0b00000000;
    // Device GRE
    const uint64_t device_write_combining_encoding = 0b00001100;
    // Normal memory, inner and outer non-cacheable
    const uint64_t memory_uncacheable_encoding = 0b01000100;
    // Normal memory inner and outer writethrough, non-transient
    const uint64_t memory_writethrough_encoding = 0b10111011;
    // Normal memory, inner and outer write-back, non-transient
    const uint64_t memory_write_back_encoding = 0b11111111;

    const uint64_t mair_value =
        memory_write_back_encoding
        | device_write_combining_encoding << 8
        | memory_writethrough_encoding << 16
        | device_uncacheable_encoding << 24
        | memory_uncacheable_encoding << 32;

    write_mair_el1(mair_value);
}

/*
* @param lowerattr How to R/W()
* @param gigherattr How to run()
*/
uint64_t MapMem4K(pagemap_t *pagemap,uint64_t vaddr, uint64_t paddr,uint64_t lowerattr,uint64_t higherattr){
    uint64_t ttbr;
    if(vaddr <= 0x0000FFFFFFFFFFFF)
        ttbr = (uint64_t)pagemap->top_level[0];
    else
        ttbr = (uint64_t)pagemap->top_level[1];
    Translate4K t;t.vaddr = vaddr;
    __tdesc_4K *lv0 = (__tdesc_4K*)
                    (ttbr + (uint64_t)t.addrdesc.LV0_TblIndex * sizeof(__tdesc_4K));
    TableDesc4KLV1 *lv1 = (TableDesc4KLV1*)
                    ((uint64_t)lv0->NextLvl + t.addrdesc.LV1_TblIndex * sizeof(__tdesc_4K));
    TableDesc4KLV2 *lv2 = (TableDesc4KLV2*)
                    ((uint64_t)lv1->desc.NextLvl + t.addrdesc.LV2_TblIndex * sizeof(__tdesc_4K));
    PageDescriptor4K *Page = (PageDescriptor4K*)
                    ((uint64_t)lv2->desc.NextLvl + t.addrdesc.LV3_TblIndex * sizeof(PageDescriptor4K));

    Page->fields.OutputAddr = paddr & 0xFFF; //Set lower 12 bits
    Page->fields.LowerAttrs = lowerattr;
    Page->fields.UpperAttrs = higherattr;
}

/*
* @param lowerattr How to R/W()
* @param gigherattr How to run()
*/
uint64_t MapMem16K(pagemap_t *pagemap,uint64_t vaddr, uint64_t paddr,uint64_t lowerattr,uint64_t higherattr){
    uint64_t ttbr;
    if(vaddr <= 0x0000FFFFFFFFFFFF)
        ttbr = (uint64_t)pagemap->top_level[0];
    else
        ttbr = (uint64_t)pagemap->top_level[1];
    Translate16K t;t.vaddr = vaddr;
    __tdesc_16K *lv0 = (__tdesc_16K*)
                        (ttbr + t.addrdesc.LV0_TblIndex * sizeof(__tdesc_16K));
    __tdesc_16K *lv1 = (__tdesc_16K*)
                        ((uint64_t)lv0->NextLvl + t.addrdesc.LV1_TblIndex * sizeof(__tdesc_16K));
    __tdesc_16K *lv2 = (__tdesc_16K*)
                        ((uint64_t)lv1->NextLvl + t.addrdesc.LV2_TblIndex * sizeof(__tdesc_16K));
    
    PageDescriptor64K *Page = (PageDescriptor64K*)
                    ((uint64_t)lv2->NextLvl + t.addrdesc.LV3_TblIndex * sizeof(PageDescriptor64K));
    Page->fields.OutputAddr = paddr & 0x3FFF; //Set lower 14 bits
    Page->fields.LowerAttrs = lowerattr;
    Page->fields.UpperAttrs = higherattr;
}

/*
* @param lowerattr How to R/W()
* @param gigherattr How to run()
*/
uint64_t MapMem64K(pagemap_t *pagemap,uint64_t vaddr, uint64_t paddr,uint64_t lowerattr,uint64_t higherattr){
    uint64_t ttbr;
    if(vaddr <= 0x0000FFFFFFFFFFFF)
        ttbr = (uint64_t)pagemap->top_level[0];
    else
        ttbr = (uint64_t)pagemap->top_level[1];
    Translate64K t;t.vaddr = vaddr;
    __tdesc_64K *lv1 = (__tdesc_64K*)
                        (ttbr + t.addrdesc.LV1_TblIndex * sizeof(__tdesc_64K));
    __tdesc_64K *lv2 = (__tdesc_64K*)
                        (lv1->NextLvl + t.addrdesc.LV2_TblIndex * sizeof(__tdesc_64K));
    PageDescriptor64K *Page = (PageDescriptor64K*)
                    (lv2->NextLvl + t.addrdesc.LV3_TblIndex * sizeof(PageDescriptor64K));
    return (Page->fields.OutputAddr << 16) | t.addrdesc.BlockOffset;
    //FFFF
    Page->fields.OutputAddr = paddr & 0xFFFF; //Set lower 16 bits
    Page->fields.LowerAttrs = lowerattr;
    Page->fields.UpperAttrs = higherattr;
}

volatile const uint64_t (*MamMemoryFunctionLists[3])
    (pagemap_t *pagemap,uint64_t vaddr, uint64_t paddr,uint64_t lowerattr,uint64_t higherattr) = {
    MapMem4K,MapMem16K,MapMem64K
};


namespace VMM
{
    volatile pagemap_t *KernelPageMap = nullptr;
    void Init(){
        setup_mair();
        kpokln("Setup Mair!");
        //TODO: VMM::INIT
        KernelPageMap = HIGHER_HALF(PMM::Request());
        KernelPageMap->top_level[0] = HIGHER_HALF(PMM::Request());
        KernelPageMap->top_level[1] = HIGHER_HALF(PMM::Request());
        _memset(KernelPageMap->top_level[0],0,PAGE_SIZE);
        _memset(KernelPageMap->top_level[1],0,PAGE_SIZE);

        hcf(); //I didn't know how to set tec_el1 val!!!

        VMM::SwitchPM(VMM::KernelPageMap);
    }

    void SwitchPM(pagemap_t *pagemap){
        asm volatile (
            "msr ttbr0_el1, %0\n"
            "msr ttbr1_el1, %1\n"
            "isb"
            : : "r"(pagemap->top_level[0]), "r"(pagemap->top_level[1]) : "memory"
        );
    }
} // namespace VMM
