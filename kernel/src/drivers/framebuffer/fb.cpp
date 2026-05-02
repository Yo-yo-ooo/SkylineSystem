/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fb.cpp
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

#include <klib/klib.h>
#include <klib/renderer/fb.h>
#include <arch/x86_64/schedule/sched.h>
extern "C" void mmu_invlpg(uint64_t vaddr);
namespace FrameBufferDevice{

    uint64_t MemoryMap(
        uint64_t length,uint64_t prot,
        uint64_t offset,uint64_t VADDR
    ){
        //kinfoln("Called dev.ops.MMAP");
        uint64_t fb_siz = Fb->BufferSize; // Assuming 32 bits per pixel
        kinfoln("FB Base Address: 0x%X, Size: %lu bytes", Fb->BaseAddress, fb_siz);
        pagemap_t *pm = Schedule::this_proc()->pagemap;
        uint64_t pages = DIV_ROUND_UP(fb_siz, PAGE_SIZE);
        //VADDR = VMM::EAlloc(pm, pages, VMM_FLAG_USER | VMM_FLAG_READWRITE | VMM_FLAG_CACHE_DISABLE);
        spinlock_lock(&pm->vma_lock);
        VADDR = VMM::Useless::InternalAlloc(pm, pages, VMM_FLAG_USER | VMM_FLAG_READWRITE | VMM_FLAG_CACHE_DISABLE);
        for(uint64_t i = 0; i < pages; i++){
            uint64_t paddr = ((uint64_t)Fb->BaseAddress + i * PAGE_SIZE);
            uint64_t cur_vaddr = VADDR + i * PAGE_SIZE;
            //kinfoln("Mapping FB page: VADDR=0x%X PADDR=0x%X", cur_vaddr, paddr);
            VMM::Map(pm, cur_vaddr, paddr, 
                    VMM_FLAG_USER | 
                    VMM_FLAG_READWRITE | 
                    VMM_FLAG_CACHE_DISABLE);
        }
        spinlock_unlock(&pm->vma_lock);
        kinfoln("Framebuffer mapped to VADDR: 0x%X, length: %lu bytes", VADDR, fb_siz);
        return VADDR; 
    }

    uint64_t ioctl(uint64_t cmd,uint64_t arg){
        switch (cmd)
        {
        case 0:return Fb->BaseAddress;
        case 1:return Fb->BufferSize;
        case 2:return Fb->Height;
        case 3:return Fb->PixelsPerScanLine;
        case 4:return Fb->Width;
        default:
            break;
        }
    }

    void Init(){
        VDL FrameBuffer;
        //FrameBuffer.Name = "fb";
        FrameBuffer.type = VsDevType::FrameBuffer;
        FrameBuffer.DescBaseAddr = (uint64_t)Fb;
        FrameBuffer.DescLength = sizeof(Framebuffer);
        DevOPS ops = {0};
        ops.MemoryMap = FrameBufferDevice::MemoryMap;
        ops.ioctl = FrameBufferDevice::ioctl;
        kinfoln("FBBADDR %X",Fb->BaseAddress);
        Dev::AddDevice(FrameBuffer,VsDevType::FrameBuffer,ops);
    }
}
#endif