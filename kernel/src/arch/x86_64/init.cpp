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
#include <arch/x86_64/allin.h>
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>

#include <arch/x86_64/simd/simd.h>
#include <mem/heap.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <drivers/framebuffer/fb.h>
#include <klib/renderer/rnd.h>
#include <desktop/basicdraw/basicdraw.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1


void __init x86_64_init(void){
    InitFunc("Serial(Simulater)",Serial::Init());
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");
    InitFunc("SSE",sse_enable());
    kinfoln("HHDM OFFSET:0x%X",hhdm_offset);

    InitFunc("GDT",GDT::Init(0));
    InitFunc("IDT",idt_init());
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());
    InitFunc("SLAB",SLAB::Init());
    InitFunc("ACPI",ACPI::Init((void*)RSDP_ADDR));
    InitFunc("MADT",MADT_Init());
    //DISABLE PIC
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
    kinfoln("DISABLED PIC!");
    //ENABLE ICMR
    outb(0x22,0x70);
    outb(0x23,0x01);
    kinfoln("ENABLED ICMR!");
    //simd_cpu_init(get_cpu(0));
    InitFunc("LAPIC",LAPIC::Init());
    InitFunc("IOAPIC",IOAPIC::Init());
    InitFunc("PIT & RTC",PIT::InitPIT());
    InitFunc("HPET",HPET::InitHPET());
    InitFunc("SMP",smp_init());
    KernelXsaveSpace = (int8_t*)VMM::Alloc(kernel_pagemap,DIV_ROUND_UP(MaxXsaveSize,PAGE_SIZE),false);
    _memset(KernelXsaveSpace,0,MaxXsaveSize);
    *(uint16_t *)(KernelXsaveSpace + 0x00) = 0x037F;
    *(uint32_t *)(KernelXsaveSpace + 0x18) = 0x1F80;
    InitFunc("RTC",RTC::InitRTC());
    InitFunc("SIMD Core 0",simd_cpu_init(this_cpu()));
    
    
    
    
    InitFunc("Schedule",Schedule::Init());
    InitFunc("Syscall",syscall_init());
    

    InitFunc("VsDev",Dev::Init());
    //InitFunc("ATA",ATA::Init());
    if(ACPI::mcfg == NULL){PCI::DoPCIWithoutMCFG();}
    else{InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));}
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    InitFunc("KEYBOARD(x86)",keyboard_init());

    if(!ext4_kernel_init("sata0","/mp/")){hcf();}

    Schedule::Install();

    proc_t *proc = Schedule::NewProcess(true);

    FrameBufferDevice::Init();

    KernelInited = true;
    if(this_cpu()->SupportXSAVE)
        asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
    else
        asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");

    thread_t *syscalltest = Schedule::NewThread(proc, 0, 0, 
        "/mp/syscalltest.elf", 1, (char*[]){"Test Main Thread"}, (char*[]){nullptr}); 
    kinfoln("syscalltest PROCESS: %d",proc->id);


    LAPIC::IPIOthers(0, SCHED_VEC);
}