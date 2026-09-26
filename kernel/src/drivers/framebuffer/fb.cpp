//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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
        /* 修复: 帧缓冲是全屏可写映射, 原实现任何进程都能重复映射。
           改为"首个请求者独占"(即启动时第一个映射它的合成器进程),
           之后一律拒绝 —— 不依赖 pid 编号, 对启动顺序变化健壮 */
        static volatile uint32_t fb_claimed = 0;
        if (__atomic_test_and_set(&fb_claimed, __ATOMIC_ACQUIRE)) return 0;
        uint64_t fb_siz = Fb->BufferSize;
        kinfoln("FB Base Address: 0x%X, Size: %lu bytes", Fb->BaseAddress, fb_siz);
        pagemap_t *pm = Schedule::this_proc()->pagemap;
        uint64_t pages = DIV_ROUND_UP(fb_siz, PAGE_SIZE);

        spinlock_lock(&pm->vma_lock);
        VADDR = VMM::Internal::InternalAlloc(pm, pages, 
            MM_USER | VMM_FLAGS_MMIO);
        spinlock_unlock(&pm->vma_lock);
        VMM::MapRange(pm,VADDR,PHYSICAL((uint64_t)Fb->BaseAddress),MM_USER | VMM_FLAGS_MMIO,pages);
        VMM::NewMapping(pm, VADDR, pages, 
            MM_USER | VMM_FLAGS_MMIO);
        uint64_t check_pte = VMM::Internal::GetPageInfo(pm, VADDR).flags;
        kinfoln("VADDR: 0x%X -> PTE Value: 0x%llX", VADDR, check_pte);
        kinfoln("Framebuffer mapped to VADDR: 0x%X", VADDR);
        return (VADDR); 
    }

    uint64_t ioctl(uint64_t cmd,uint64_t arg){
        switch (cmd)
        {
        case 0:return (uint64_t)Fb->BaseAddress;
        case 1:return (uint64_t)Fb->BufferSize;
        case 2:return (uint64_t)Fb->Height;
        case 3:return (uint64_t)Fb->PixelsPerScanLine;
        case 4:return (uint64_t)Fb->Width;
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