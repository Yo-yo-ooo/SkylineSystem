#ifdef __x86_64__

#include <klib/klib.h>
#include <klib/renderer/fb.h>
#include <arch/x86_64/schedule/sched.h>
namespace FrameBufferDevice{

    uint64_t MemoryMap(
        uint64_t length,uint64_t prot,
        uint64_t offset,uint64_t VADDR
    ){
        uint64_t flags_vm = 0;
        if(prot & PROT_READ) flags_vm |= MM_READ;
        if(prot & PROT_WRITE) flags_vm |= MM_WRITE;
        if(prot & PROT_EXEC) flags_vm |= MM_NX;
        flags_vm |= MM_USER;
        flags_vm |= VMM_FLAG_PRESENT;
        proc_t *Process = Schedule::this_proc();
        pagemap_t *pagemap = Process->pagemap;
        uint64_t FramBufferPAddr = Fb->BaseAddress;
        if(length > Fb->BufferSize)
            return -EINVAL;
        if(VADDR == 0){
            /* uint64_t VADDR_ = VMM::Alloc(Process->pagemap,DIV_ROUND_UP(length,PAGE_SIZE),true);
            VMM::MapRange(Process->pagemap,VADDR_,FramBufferPAddr,flags_vm,DIV_ROUND_UP(length,PAGE_SIZE));
            return VADDR_; */
            uint64_t page_count;// = DIV_ROUND_UP(length, PAGE_SIZE);
            if(length == NULL)
                page_count = DIV_ROUND_UP(Fb->BufferSize, PAGE_SIZE);
            else
                page_count = DIV_ROUND_UP(length, PAGE_SIZE);
            if (!page_count) return -EINVAL;
            spinlock_lock(&pagemap->vma_lock);
            uint64_t addr = VMM::Useless::InternalAlloc(pagemap, page_count, flags_vm);
            for (int32_t i = 0; i < page_count; i++)
                VMM::Map(pagemap, addr + (i * PAGE_SIZE), FramBufferPAddr + i * page_count, flags_vm);
            VMM::NewMapping(pagemap, addr, page_count, flags_vm);
            spinlock_unlock(&pagemap->vma_lock);
            return (void*)addr;
        }else{
            //TODO: Map to VADDR
        }
    }

    void Init(){
        VDL FrameBuffer;
        FrameBuffer.Name = "fb";
        FrameBuffer.type = VsDevType::FrameBuffer;
        FrameBuffer.DescBaseAddr = (uint64_t)Fb;
        FrameBuffer.DescLength = sizeof(Framebuffer);
        DevOPS ops = {0};
        ops.MemoryMap = FrameBufferDevice::MemoryMap;
        Dev::AddDevice(FrameBuffer,VsDevType::FrameBuffer,ops);
    }
}
#endif