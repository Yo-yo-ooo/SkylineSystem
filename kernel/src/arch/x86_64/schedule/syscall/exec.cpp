//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <atomic/atomic.h>

void execve_cleanup(int argc, int envc, char **argv, char **envp) {
    if (argv) {
        for (int i = 0; i < argc; i++)
            if (argv[i]) kfree(argv[i]);
        kfree(argv);
    }
    if (envp) {
        for (int j = 0; j < envc; j++)
            if (envp[j]) kfree(envp[j]);
        kfree(envp);
    }
}

extern uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2);

extern cpu_t *get_lw_cpu(cpu_t *ref_cpu = nullptr);

uint64_t elf_load(uint8_t *data, pagemap_t *pagemap, 
                  uint64_t *tls_offset = nullptr, 
                  uint64_t *tls_memsz = nullptr, 
                  uint64_t *tls_filesz = nullptr, 
                  uint64_t *tls_align = nullptr) {
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
        return 0;                    // ★ 4: 原来只打印不返回，32位 ELF 会继续往下跑
    }

    Elf64::Elf64_Phdr *phdrs = (Elf64::Elf64_Phdr*)(data + hdr->e_phoff);
    uint64_t max_vaddr = 0;

    // ★ 1: 保存进入时的地址空间，结尾恢复（原来结尾硬编码切 kernel_pagemap，
    //       sysret 回调用者时 CR3 是内核的 → 用户态取指 #PF，本次故障根因）
    pagemap_t *restore_pm = VMM::SwitchPageMap(pagemap);

    for (int i = 0; i < hdr->e_phnum; i++) {
        Elf64::Elf64_Phdr *phdr = &phdrs[i];
        if (phdr->p_type == 1) {
            uint64_t start = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
            uint64_t end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
            uint64_t flags = MM_READ | MM_WRITE | MM_USER;
            for (uint64_t i = start; i < end; i += PAGE_SIZE) {
                // ★ 5: 两个 PT_LOAD 共享边界页时（很常见），第二段会重新分配
                //       物理页覆盖第一段尾部数据并泄漏旧页 → 已映射则跳过
                if (VMM::GetPhysics(pagemap, i))
                    continue;
                uint64_t page = (uint64_t)PMM::Request();
                VMM::Map(pagemap, i, page, flags);
                kinfoln("  Mapping vaddr=0x%lx -> page=0x%lx", i, page);
            }
            VMM::NewMapping(pagemap, start, (end - start) / PAGE_SIZE, flags);
            // ★ 登记 VMA：否则 Fork 克隆不到镜像、进程退出时这些页无人释放
            //   （依赖幂等版 SetStart 的 vma.cpp；旧版 SetStart 会丢弃它，无害）
            VMM::VMA::AddRegion(pagemap, start, (end - start) / PAGE_SIZE, flags);
            __memcpy((void*)phdr->p_vaddr, (void*)(data + phdr->p_offset), phdr->p_filesz);
            if (phdr->p_memsz > phdr->p_filesz)
                _memset((void*)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
            if (end > max_vaddr)
                max_vaddr = end;
            kinfoln("ELF LOADER PT_LOAD: vaddr=[0x%lx~0x%lx], filesz=0x%lx, memsz=0x%lx, flags=0x%x", 
                    phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz, 
                    phdr->p_filesz, phdr->p_memsz, phdr->p_flags);
        } 
        else if (phdr->p_type == 7) { 
            if (tls_offset) *tls_offset = phdr->p_offset;
            if (tls_memsz)  *tls_memsz  = phdr->p_memsz;
            if (tls_filesz) *tls_filesz = phdr->p_filesz;
            if (tls_align)  *tls_align  = phdr->p_align;
            kinfoln("ELF LOADER PT_TLS: offset=0x%lx, filesz=0x%lx, memsz=0x%lx, align=0x%lx", 
                    phdr->p_offset, phdr->p_filesz, phdr->p_memsz, phdr->p_align);
        }
    }

    // ★ 1: 恢复进入时的地址空间（!smp_started 时 SwitchPageMap 返回 nullptr，兜底内核）
    VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
    max_vaddr += PAGE_SIZE;
    VMM::VMA::SetStart(pagemap, max_vaddr, 1);
    kpokln("LOAD ELF!");

    return hdr->e_entry;
}

extern uint64_t sched_tid;
extern uint32_t sched_prio_to_weight[16];

art_tree *NOT_RUNQ_P;
spinlock_t NOT_RUNQ_LOCK = 0;

uint64_t sys_load(uint64_t u_pathname, uint64_t u_argv, uint64_t u_envp, \
    GENERATE_IGN3()) {
    
    IGNV_3();

    if(!is_user_address(u_pathname) || !is_user_address(u_argv)
    || !is_user_address(u_envp) /* || !is_user_address(EXECVE_ARG) */)
        return -EFAULT;
    if((Schedule::this_proc()->IsTrusted == false)/*  && EXECVE_ARG */)
        return -EPERM;
    
    //User-Mode Trusted Process Mapping Control, UTPMC
    //SysExecveARG *pearg = (SysExecveARG*)EXECVE_ARG;

    // ★ 2: 记住 syscall 进入时的地址空间。“仅加载”意味着退出时必须原样归还，
    //       否则 sysret 会把调用者送进错误的 CR3
    cpu_t *my_cpu = this_cpu();
    pagemap_t *caller_pm = (smp_started && my_cpu && my_cpu->pagemap)
                               ? my_cpu->pagemap
                               : (pagemap_t*)kernel_pagemap;

    const char* pathname_ = (const char*)u_pathname;
    char **argv_ = (char**)u_argv;          // ★ 3: 原来声明成 const char*，
    char **envp_ = (char**)u_envp;          //       argc 计数和 strlen 全按单字节算
    char *pathname = (char*)kmalloc(strlen(pathname_)+1);
    __memcpy(pathname, pathname_, strlen(pathname_)+1);
    
    int argc = 0;
    if (argv_) { while (argv_[argc]) argc++; }
    int envc = 0;
    if (envp_) { while (envp_[envc]) envc++; }
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

    // Create New Process
    // PROCESS NOT TRUST PROCESS AS DEFAULT FOR SAFE
    proc_t *parent = Schedule::NewProcess(true, false);
    if (!parent) {
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        return -ENOMEM;
    }

    thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) {
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        kfree(parent);
        return -ENOMEM;
    }
    _memset(thread, 0, sizeof(thread_t));


    cpu_t *cpu = get_lw_cpu();
    if (!cpu) {
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        kfree(thread);
        kfree(parent);
        return -EFAULT;
    }
    uint32_t cpu_num = cpu->id;
    uint32_t priority = Schedule::this_thread()->priority;

    thread->timer_cpu = cpu_num;
    thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
    thread->cpu_num = cpu_num; 
    thread->parent = parent; 
    thread->pagemap = parent->pagemap;
    thread->priority = priority > 15 ? 15 : priority;
    thread->weight = sched_prio_to_weight[thread->priority];

    uint64_t base_vruntime = cpu->avg_vruntime;
    uint64_t half_slice = cpu->base_quantum / 2;
    thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;

    __hmap_s_mp *MP = GetMount(pathname);
    if(!MP) { 
        kerrorln("Cannot Find Mount Point!!!"); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        kfree(thread);
        kfree(parent);
        return -ENOENT; 
    }
    void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    if (!FileDesc) { 
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        kfree(thread);
        kfree(parent);
        return -ENOMEM; 
    }
    _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
    if(MP->FSOPS->open(FileDesc, pathname, O_RDONLY) != 0) { 
        kfree(FileDesc); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        kfree(thread);
        kfree(parent);
        return -EACCES; 
    }
    uint64_t FSize = MP->FSOPS->fsize(FileDesc);
    uint8_t *buffer = (uint8_t*)kmalloc(FSize);
    if (!buffer) { 
        MP->FSOPS->close(FileDesc); 
        kfree(FileDesc); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(pathname);
        kfree(thread);
        kfree(parent);
        return -ENOMEM; 
    }
    MP->FSOPS->read(FileDesc, buffer, FSize, 0);
    MP->FSOPS->close(FileDesc); 
    kfree(FileDesc);
    kfree(pathname); // Free PathName

    uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
    _memset(&thread->ctx, 0, sizeof(context_t));
    thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align);
    if (thread->ctx.rip == 0) { 
        kerrorln("ELF load failed!"); 
        kfree(buffer); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(thread);
        kfree(parent);
        return -EINVAL; 
    }

    // ★ 6: user=true 会把这段内核内存映射成 U/S=1 的页，用户程序可直接读写
    thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), false);
    if (!thread->fx_area) { 
        kfree(buffer); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(thread);
        kfree(parent);
        return -ENOMEM; 
    }
    _memset(thread->fx_area, 0, cpu->XsaveSize);
    cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
    
    uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
    if (!kernel_stack) { 
        VMM::Free(kernel_pagemap, thread->fx_area); 
        kfree(buffer); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(thread);
        kfree(parent);
        return -ENOMEM; 
    }
    _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
    thread->kernel_stack = kernel_stack; 
    thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
    
    uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
    if (!thread_stack) { 
        VMM::Free(kernel_pagemap, thread->fx_area); 
        VMM::Free(kernel_pagemap, (void*)kernel_stack); 
        kfree(buffer); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(thread);
        kfree(parent);
        return -ENOMEM; 
    }
    thread->stack = thread_stack; 
    thread->thread_stack = thread_stack + 8 * PAGE_SIZE;
    
    uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
    if (!sig_stack) { 
        VMM::Free(kernel_pagemap, thread->fx_area); 
        VMM::Free(kernel_pagemap, (void*)kernel_stack); 
        VMM::Free(thread->pagemap, (void*)thread_stack); 
        kfree(buffer); 
        execve_cleanup(argc, envc, argv, envp);
        kfree(thread);
        kfree(parent);
        return -ENOMEM; 
    }
    thread->sig_stack = sig_stack;

    thread->ctx.cs = 0x23; 
    thread->ctx.ss = 0x1b; 
    thread->ctx.rflags = 0x202;
    thread->ctx.rsp = thread->thread_stack;
    VMM::SwitchPageMap(thread->pagemap);        // ★ 2: 写用户栈需在目标地址空间下
    Schedule::PrepareUserStack(thread, argc, argv, envp);
    thread->thread_stack = thread->ctx.rsp;
    VMM::SwitchPageMap(caller_pm);              // ★ 2: 恢复（若 PrepareUserStack
                                                //       内部自己切过，这里也保证归位）

    if (tls_memsz > 0) {
        if (tls_align == 0) tls_align = 16;
        uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
        uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
        uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
        if (!tls_mem) { 
            kfree(buffer); 
            execve_cleanup(argc, envc, argv, envp);
            VMM::Free(kernel_pagemap, thread->fx_area); 
            VMM::Free(kernel_pagemap, (void*)kernel_stack); 
            VMM::Free(thread->pagemap, (void*)thread_stack);
            VMM::Free(thread->pagemap, (void*)sig_stack);
            kfree(thread);
            kfree(parent);
            return -ENOMEM; 
        }
        uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
        VMM::SwitchPageMap(thread->pagemap);
        __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)(buffer + tls_offset), tls_filesz);
        *(uint64_t*)tcb_base = tcb_base;
        VMM::SwitchPageMap(caller_pm);          // ★ 2: 原来是 kernel_pagemap —— 根因
        thread->fs = tcb_base; 
        thread->tls_base = tls_mem; 
        thread->tls_pages = tls_pages;
    }

    kfree(buffer);
    execve_cleanup(argc, envc, argv, envp);

    spinlock_lock(&NOT_RUNQ_LOCK);
    art_insert(NOT_RUNQ_P, (const uint8_t*)&parent->id, 8, parent);
    spinlock_unlock(&NOT_RUNQ_LOCK);

    kpokln("LOADED PID:%d",parent->id);

    return parent->id;                          // 此时 CR3 == caller_pm，sysret 安全
}

uint64_t sys_launch(uint64_t pid,GENERATE_IGN5()){
    IGNV_5();
    cpu_t *cpu = get_lw_cpu();
    if (!cpu) return -EFAULT;

    spinlock_lock(&NOT_RUNQ_LOCK);              // ★ 7: 搜索也要持锁
    proc_t* proc = (proc_t*)art_search(NOT_RUNQ_P,(const uint8_t*)&pid,8);
    spinlock_unlock(&NOT_RUNQ_LOCK);
    if (!proc){     // ★ 7: 原来直接解引用，进程不存在即崩
        kwarnln("sys_launch: PROC NOT FOUND");
        return -ESRCH;
    }                   
    kinfoln("PROC->ID %d",proc->id);

    thread_t *thread = proc->threads;
    if (!thread) return -ESRCH;                 // ★ 7

    thread->state = THREAD_RUNNING;
    thread->timer_cpu = cpu->id;
    thread->cpu_num = cpu->id;
    Schedule::Internal::ProcessAddThread(proc, thread);
    uint64_t rflags;
    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
    spinlock_lock(&cpu->sched_lock);
    cpu->has_runnable_thread = true;
    Schedule::Internal::InsertToQueue(cpu, thread);
    spinlock_unlock(&cpu->sched_lock);
    asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

    return 0;                                   // ★ 7: 原来缺 return，RAX 是垃圾
}