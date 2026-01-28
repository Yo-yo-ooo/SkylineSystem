#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
uint64_t sys_mmap_(void *addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,uint64_t offset) {
    pagemap_t *pagemap = Schedule::this_proc()->pagemap;
    
    if(Schedule::this_proc()->fd_table[fd]->path == NULL){
        return -EINVAL;
    }
    size_t pages = DIV_ROUND_UP(length, PAGE_SIZE);
    
    uint64_t flags_vm = 0;
    if(prot & PROT_READ) flags_vm |= MM_READ;
    if(prot & PROT_WRITE) flags_vm |= MM_WRITE;
    if(prot & PROT_EXEC) flags_vm |= MM_NX;
    flags_vm |= MM_USER;
    spinlock_lock(&pagemap->vma_lock);
    uint64_t Address = 0;
    if(!addr){
        Address = VMM::Useless::InternalAlloc(pagemap, pages, flags_vm);
    }else{Address = (uint64_t)addr;}
    for (int32_t i = 0; i < pages; i++)
        VMM::Map(pagemap, Address + (i * PAGE_SIZE), (uint64_t)PMM::Request(), flags_vm);
    VMM::NewMapping(pagemap, Address, pages, flags_vm);
    spinlock_unlock(&pagemap->vma_lock);
    if(flags & MAP_SHARED){
        
        _memset((void*)Address,0,length);
    }
    return Address;
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

uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);

    thread_t * tthread = Schedule::this_thread();
    size_t pages = DIV_ROUND_UP(len, PAGE_SIZE);
    uint64_t vm_flags = 0;
    if (prot & 2) 
        vm_flags |= MM_READ;
    if (prot & 4) 
        vm_flags |= MM_WRITE;
    if (!(prot & 1)) 
        vm_flags |= MM_NX;

    spinlock_lock(&tthread->pagemap->vma_lock);
    VMM::MapRange(tthread->pagemap, addr, 0, vm_flags, pages);
    spinlock_unlock(&tthread->pagemap->vma_lock);

    return 0;
}