#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>

uint64_t sys_mmap_(void *addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,uint64_t offset) {
    MapedFileInfo info;
    uint64_t vm_flags;
    if (prot & 2) 
        vm_flags |= MM_READ;
    if (prot & 4) 
        vm_flags |= MM_WRITE;
    if (!(prot & 1)) 
        vm_flags |= MM_NX;
    info.FakeVMFlag = vm_flags;
    info.RealVMFlag = vm_flags | MM_USER;
    thread_t * tthread = Schedule::this_thread();
    //cpu_t* tcpu = this_cpu();

    size_t pages = DIV_ROUND_UP(length, PAGE_SIZE);
    void *virt_addr = nullptr;

    void *buffer = kmalloc(pages * PAGE_SIZE);
    _memset(buffer, 0, pages * PAGE_SIZE - length);
    if(Schedule::this_proc()->fd_table[fd]->IsSpecial == false){
        ext4_fopen(&Schedule::this_proc()->fd_table[fd]->f,
            Schedule::this_proc()->fd_table[fd]->path,"r");
        ext4_fseek(&Schedule::this_proc()->fd_table[fd]->f,offset,SEEK_SET);
        ext4_fread(&Schedule::this_proc()->fd_table[fd]->f,buffer,length,NULL);
        if (!addr) {
            // Allocate new address
            // Look for the first fit on the list.
            spinlock_lock(&tthread->pagemap->vma_lock);
            uint64_t flags = vm_flags;
            virt_addr = VMM::Useless::InternalAlloc(
                tthread->pagemap, pages, flags);
            for (int32_t i = 0; i < pages; i++)
                VMM::Map(
                    tthread->pagemap, 
                    virt_addr + (i * PAGE_SIZE), 
                    VMM::GetPhysics(kernel_pagemap,buffer) + (i * PAGE_SIZE), 
                    flags
                );
            VMM::NewMapping(tthread->pagemap, virt_addr, pages, flags);
            spinlock_unlock(&tthread->pagemap->vma_lock);
            
            info.length = length;
            info.fs_desc = (void*)&Schedule::this_proc()->fd_table[fd]->f;
            info.DescSize = sizeof(ext4_file);
            info.fd = fd;
            info.BufferBaseAddr = (uint64_t)buffer;
            if(tthread->maped_file_list.UsedCount + 1 > tthread->maped_file_list.MaxCount){
                tthread->maped_file_list.Info = krealloc(tthread->maped_file_list.Info, \
                    sizeof(MapedFileInfo) * (tthread->maped_file_list.MaxCount + 16));
                tthread->maped_file_list.MaxCount += 16;
            }
            tthread->maped_file_list.Info[tthread->maped_file_list.UsedCount + 1] = info;
            tthread->maped_file_list.UsedCount++;
            //virt_addr = (void*)addr;
        } else {
            // TODO: Check if a mapping already exists in this address
            // if it does, pick another address.
            for (size_t i = 0; i < pages; i++) {
                VMM::Map(
                    tthread->pagemap, 
                    (uint64_t)addr + i * PAGE_SIZE, 
                    VMM::GetPhysics(kernel_pagemap,buffer) + (i * PAGE_SIZE), 
                    vm_flags
                );
            }
            virt_addr = addr;
        }
    }else{
        if (!addr) {
            // Allocate new address
            virt_addr = VMM::Alloc(tthread->pagemap, pages, true);
        } else {
            // TODO: Check if a mapping already exists in this address
            // if it does, pick another address.
            uint64_t phys = 0;
            for (size_t i = 0; i < pages; i++) {
                phys = (uint64_t)PMM::Request();
                VMM::Map(
                    tthread->pagemap, 
                    (uint64_t)addr + i * PAGE_SIZE, 
                    phys, 
                    vm_flags
                );
            }
            virt_addr = addr;
        }
    }

    return (uint64_t)virt_addr;
}

uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t prot, \
    uint64_t flags, uint64_t fd,uint64_t offset){
    return sys_mmap_((void*)addr_,length,prot,flags,fd,offset);
}

uint64_t sys_munmap(uint64_t addr, uint64_t length, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,uint64_t ign_3){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);
    
    size_t pages = DIV_ROUND_UP(length, PAGE_SIZE);
    for (size_t i = 0; i < pages; i++) {
        VMM::Unmap(Schedule::this_thread()->pagemap, (uint64_t)addr - i * PAGE_SIZE);
    }
    return 0;
}

uint64_t sys_brk(uint64_t addr, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,uint64_t ign_3,uint64_t ign_4){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    thread_t *t = Schedule::this_thread();
    if(*((int64_t*)addr) < 0 && !t->heap)
        return -1;
    if((*((int64_t*)addr) < 0 ? -*((int64_t*)addr) : *((int64_t*)addr)) > t->heap_size
        && *((int64_t*)addr) < 0)
        return -1;
    if(!t->heap && t->heap_size < *((int64_t*)addr)){
        SLAB::UserAlloc(*((uint64_t*)addr));
        t->heap_size += *((int64_t*)addr);
        return 0;
    }
    uint64_t old_size = t->heap_size;
    t->heap_size += *((int64_t*)addr);
    void *new_ptr = SLAB::UserAlloc(t->heap_size);
    __memcpy(new_ptr, t->heap_size, (old_size > t->heap_size ? t->heap_size : old_size));
    SLAB::Free(t->heap);
    t->heap = new_ptr;
    return 0;
}