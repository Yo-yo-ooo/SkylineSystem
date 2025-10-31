#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>

void execve_cleanup(int argc, int envc, char **argv, char **envp) {
    for (int i = 0; i < argc; i++)
        kfree(argv[i]);
    kfree(argv);
    for (int j = 0; j < envc; j++)
        kfree(envp[j]);
    kfree(envp);
}

extern uint64_t sys_read(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);

uint64_t elf_load(uint8_t *data, pagemap_t *pagemap) {
    Elf64::Elf64_Ehdr *hdr = (Elf64::Elf64_Ehdr*)data;

    if (hdr->e_ident.c[0] != 0x7f || 
        hdr->e_ident.c[1] != 'E' || 
        hdr->e_ident.c[2] != 'L' || 
        hdr->e_ident.c[3] != 'F'){
        kerrorln("LOAD FILE NOT ELF!");
        return 0;
    }

    if (hdr->e_type != 2){
        kinfoln("ELF TYPE %d",hdr->e_type);
        kerrorln("ELF> LOAD ELF FILE TYPE NOT SUPPORT!");
        return 0;
    }

    if(hdr->e_ident.c[Elf64::EI_CLASS] != Elf64::ELFCLASS64){
        kerrorln("ELF> FILE NOT 64 BIT!");
    }

    Elf64::Elf64_Phdr *phdrs = (Elf64::Elf64_Phdr*)(data + hdr->e_phoff);
    uint64_t max_vaddr = 0;

    VMM::SwitchPageMap(pagemap);


    for (int i = 0; i < hdr->e_phnum; i++) {
        Elf64::Elf64_Phdr *phdr = &phdrs[i];
        if (phdr->p_type == 1) {
            // Load this segment into memory at the correct virtual address
            uint64_t start = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
            uint64_t end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
            uint64_t flags = MM_READ | MM_WRITE | MM_USER;
            for (uint64_t i = start; i < end; i += PAGE_SIZE) {
                uint64_t page = (uint64_t)PMM::Request();
                VMM::Map(pagemap, i, page, flags);
            }
            VMM::NewMapping(pagemap, start, (end - start) / PAGE_SIZE, flags);
            __memcpy((void*)phdr->p_vaddr, (void*)(data + phdr->p_offset), phdr->p_filesz);
            if (phdr->p_memsz > phdr->p_filesz)
                _memset((void*)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz); // Zero out BSS
            if (end > max_vaddr)
                max_vaddr = end;
        }
    }

    VMM::SwitchPageMap(kernel_pagemap);
    max_vaddr += PAGE_SIZE;

    // Sets the start of the VMA, the first free region to start allocating.
    VMM::VMA::SetStart(pagemap, max_vaddr, 1);
    kpokln("LOAD ELF!");

    return hdr->e_entry;
}


uint64_t sys_execve(uint64_t u_pathname, uint64_t u_argv, uint64_t u_envp, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
        IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    // Copy pathname to kernel
    const char* pathname_ = (const char*)u_pathname;
    const char* argv_ = (const char*)u_argv;
    const char* envp_ = (const char*)u_envp;
    char *pathname = (char*)kmalloc(strlen(pathname_)+1);
    __memcpy(pathname, pathname_, strlen(pathname_)+1);
    // Copy argv, envp to kernel
    int argc = 0;
    if (argv_) {
        while (argv_[argc++]);
        argc -= 1;
    }
    int envc = 0;
    if (envp_) {
        while (envp_[envc++]);
        envc -= 1;
    }
    char **argv = (char**)kmalloc((argc + 1) * 8);
    argv[argc] = nullptr;
    for (int i = 0; i < argc; i++) {
        int size = strlen(argv_[i]) + 1;
        char *arg = (char*)kmalloc(size);
        argv[i] = arg;
        __memcpy(arg, argv_[i], size);
    }
    char **envp = (char**)kmalloc((envc + 1) * 8);
    envp[envc] = nullptr;
    for (int i = 0; i < envc; i++) {
        int size = strlen(envp_[i]) + 1;
        char *env = (char*)kmalloc(size);
        envp[i] = env;
        __memcpy(env, envp_[i], size);
    }
    proc_t *proc = Schedule::this_proc();
    thread_t *thread = Schedule::this_thread();
    //vnode_t *node = vfs_open(proc->cwd, pathname);
    if(sys_open(pathname_,O_RDONLY,NULL,NULL,NULL,NULL) == -1){
        execve_cleanup(argc, envc, argv, envp);
        return (uint64_t)((int64_t)-1);
    }
    Schedule::PAUSE();
    //proc->cwd = node->parent;
    VMM::SwitchPageMap(kernel_pagemap);
    // Destroy old page map
    VMM::DestroyPM(thread->pagemap);
    pagemap_t *new_pagemap = VMM::NewPM();
    thread->pagemap = proc->pagemap = new_pagemap;
    thread->sig_deliver = 0;
    thread->sig_mask = 0;
    _memset(thread->fx_area, 0, 512);
    *(uint16_t *)(thread->fx_area + 0x00) = 0x037F;
    *(uint32_t *)(thread->fx_area + 0x18) = 0x1F80;

    // Load ELF
    static ext4_file f;
    ext4_fopen(&f,pathname_,"r");
    uint8_t *buffer = (uint8_t*)kmalloc(f.fsize);
    ext4_fread(&f,buffer,f.fsize,NULL);
    //vfs_read(node, buffer, 0, node->size);
    thread->ctx.rip = elf_load(buffer, thread->pagemap);

    thread->ctx.cs = 0x23;
    thread->ctx.ss = 0x1b;
    thread->ctx.rflags = 0x202;

    // Thread stack (32 KB)
    uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
    uint64_t thread_stack_top = thread_stack + 8 * PAGE_SIZE;
    thread->stack = thread_stack;

    // Sig stack (4 KB)
    uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
    thread->sig_stack = sig_stack;

    // Set up stack (argc, argv, env)
    thread->ctx.rsp = thread_stack_top;
    Schedule::PrepareUserStack(thread, argc, argv, envp);
    thread->thread_stack = thread->ctx.rsp;

    execve_cleanup(argc, envc, argv, envp);

    thread->fs = 0;
    VMM::SwitchPageMap(new_pagemap);
    __asm__ volatile ("swapgs");
    this_cpu()->current_thread = nullptr;
    Schedule::Resume();
    return 0;
}

