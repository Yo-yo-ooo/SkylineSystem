#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>

uint64_t sys_mmap_(void *addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd) {
    uint64_t vm_flags = MM_USER;
    if (prot & 2) vm_flags |= MM_READ;
    if (prot & 4) vm_flags |= MM_WRITE;
    if (!(prot & 1)) vm_flags |= MM_NX;

    size_t pages = DIV_ROUND_UP(length, PAGE_SIZE);
    void *virt_addr = nullptr;

    if (!addr) {
        // Allocate new address
        virt_addr = VMM::Alloc(Schedule::this_thread()->pagemap, pages, true);
    } else {
        // TODO: Check if a mapping already exists in this address
        // if it does, pick another address.
        uint64_t phys = 0;
        for (size_t i = 0; i < pages; i++) {
            phys = (uint64_t)PMM::Request();
            VMM::Map(Schedule::this_thread()->pagemap, (uint64_t)addr + i * PAGE_SIZE, phys, vm_flags);
        }
        virt_addr = addr;
    }

    if (flags & 1)
        _memset(virt_addr, 0, length);

    return (uint64_t)virt_addr;
}

uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t prot, \
    uint64_t flags, uint64_t fd,uint64_t ign_0){
    IGNORE_VALUE(ign_0);
    return sys_mmap_((void*)addr_,length,prot,flags,fd);
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
        SLAB::Alloc(*((uint64_t*)addr));
        t->heap_size += *((int64_t*)addr);
        return 0;
    }
    uint64_t old_size = t->heap_size;
    t->heap_size += *((int64_t*)addr);
    void *new_ptr = SLAB::Alloc(t->heap_size);
    __memcpy(new_ptr, t->heap_size, (old_size > t->heap_size ? t->heap_size : old_size));
    SLAB::Free(t->heap);
    t->heap = new_ptr;
    return 0;
}