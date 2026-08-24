//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <arch/x86_64/lapic/lapic.h>
#include <atomic/atomic.h>
#include <fs/fc.h>          // fd_manager_destroy


#define SYS_MAX_PATH     4096
#define SYS_MAX_ARGLEN   32768
#define SYS_MAX_ARGC     256
#define SYS_MAX_ENVC     256
#define USER_ADDR_LIMIT  0x0000800000000000ULL

extern art_tree *pid2proc_tree;
extern spinlock_t PID2PROC_TREE_LOCK;

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

/* ============ 用户字符串带上界拷贝 ============
 * 消除对用户内存 strlen/无界遍历的失控读。
 * 注: 若字符串中途跨入未映射页仍会 #PF —— 彻底解决需要
 * copy_from_user(#PF 修复表)基建, 此为过渡方案。 */
static int64_t copy_user_str(const char *src, char **out, uint64_t cap) {
    *out = nullptr;
    if (!src) return 0;
    char *dst = (char*)kmalloc(cap);
    if (unlikely(!dst)) return -ENOMEM;
    for (uint64_t i = 0; i < cap - 1; i++) {
        char c = src[i];
        dst[i] = c;
        if (c == '\0') { *out = dst; return (int64_t)(i + 1); }
    }
    kfree(dst);
    return -ENAMETOOLONG;
}

/* argv/envp 指针数组整体拷贝; 失败时自清理, 调用方无需善后 */
static int64_t copy_user_strarray(char **src, char ***out_arr, int *out_cnt, int max_n) {
    *out_arr = nullptr; *out_cnt = 0;
    if (!src) return 0;                       // POSIX 允许 NULL (空参数表)

    int n = 0;
    while (n < max_n && src[n]) n++;
    if (unlikely(n == max_n)) return -E2BIG;  // 无结尾或超限, 保守拒绝

    char **arr = (char**)kmalloc((uint64_t)(n + 1) * sizeof(char*));
    if (unlikely(!arr)) return -ENOMEM;
    arr[n] = nullptr;
    for (int i = 0; i < n; i++) {
        int64_t r = copy_user_str(src[i], &arr[i], SYS_MAX_ARGLEN);
        if (unlikely(r < 0)) {
            for (int j = 0; j < i; j++) kfree(arr[j]);
            kfree(arr);
            return r;
        }
    }
    *out_arr = arr; *out_cnt = n;
    return 0;
}

/* ============ ELF 加载(带边界校验的实现 + 兼容旧签名的包装) ============ */
static uint64_t elf_load_impl(uint8_t *data, pagemap_t *pagemap,
                              uint64_t *tls_offset, uint64_t *tls_memsz,
                              uint64_t *tls_filesz, uint64_t *tls_align,
                              uint64_t data_len) {
    Elf64::Elf64_Ehdr *hdr = (Elf64::Elf64_Ehdr*)data;

    if (hdr->e_ident.c[0] != 0x7f ||
        hdr->e_ident.c[1] != 'E' ||
        hdr->e_ident.c[2] != 'L' ||
        hdr->e_ident.c[3] != 'F') {
        kerrorln("LOAD FILE NOT ELF!");
        return 0;
    }
    if (hdr->e_type != 2) {
        kinfoln("ELF TYPE %d", hdr->e_type);
        kerrorln("ELF> LOAD ELF FILE TYPE NOT SUPPORT!");
        return 0;
    }
    if (hdr->e_ident.c[Elf64::EI_CLASS] != Elf64::ELFCLASS64) {
        kerrorln("ELF> FILE NOT 64 BIT!");
        return 0;
    }
    if (hdr->e_machine != 62) {               // 修复: 只认 EM_X86_64
        kerrorln("ELF> WRONG MACHINE TYPE %d!", hdr->e_machine);
        return 0;
    }
    /* 修复: 程序头表边界校验 (data_len==0 表示旧调用方, 跳过) */
    if (data_len != 0) {
        if (unlikely(data_len < sizeof(Elf64::Elf64_Ehdr))) {
            kerrorln("ELF> FILE TOO SMALL!");
            return 0;
        }
        if (unlikely(hdr->e_phnum == 0 || hdr->e_phnum == 0xFFFF)) {
            kerrorln("ELF> BAD PHNUM!");
            return 0;
        }
        uint64_t ph_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * sizeof(Elf64::Elf64_Phdr);
        if (unlikely(hdr->e_phoff >= data_len || ph_end > data_len)) {
            kerrorln("ELF> PHDR TABLE OUT OF BOUNDS!");
            return 0;
        }
    }

    Elf64::Elf64_Phdr *phdrs = (Elf64::Elf64_Phdr*)(data + hdr->e_phoff);
    uint64_t max_vaddr = 0;

    // 保存进入时的地址空间,结尾恢复(否则 sysret 回调用者时 CR3 是错的 → 用户态取指 #PF)
    pagemap_t *restore_pm = VMM::SwitchPageMap(pagemap);

    for (int i = 0; i < hdr->e_phnum; i++) {
        Elf64::Elf64_Phdr *phdr = &phdrs[i];
        if (phdr->p_type == 1) {
            uint64_t filesz = phdr->p_filesz;
            /* 修复: 畸形段防御 —— filesz>memsz 会写穿映射区, 钳到 memsz */
            if (unlikely(filesz > phdr->p_memsz)) filesz = phdr->p_memsz;
            /* 修复: 段数据不得越过文件缓冲区 */
            if (unlikely(data_len != 0 && phdr->p_offset + filesz > data_len)) {
                kerrorln("ELF> SEGMENT DATA OUT OF BOUNDS!");
                VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
                return 0;
            }
            /* 修复: 目标地址必须在用户半区, 防止把内核地址映射进用户页表 */
            if (unlikely(phdr->p_vaddr >= USER_ADDR_LIMIT ||
                         phdr->p_vaddr + phdr->p_memsz >= USER_ADDR_LIMIT ||
                         phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)) {
                kerrorln("ELF> SEGMENT VADDR ILLEGAL: 0x%lx", phdr->p_vaddr);
                VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
                return 0;
            }

            uint64_t start = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
            uint64_t end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
            uint64_t flags = MM_READ | MM_WRITE | MM_USER;
            for (uint64_t p = start; p < end; p += PAGE_SIZE) {
                // 两个 PT_LOAD 共享边界页时,第二段会重新分配物理页
                // 覆盖第一段尾部数据并泄漏旧页 → 已映射则跳过
                if (VMM::GetPhysics(pagemap, p))
                    continue;
                uint64_t page = (uint64_t)PMM::Request();
                VMM::Map(pagemap, p, page, flags);
                kinfoln("  Mapping vaddr=0x%lx -> page=0x%lx", p, page);
            }
            VMM::NewMapping(pagemap, start, (end - start) / PAGE_SIZE, flags);
            // 登记 VMA:否则 Fork 克隆不到镜像、进程退出时这些页无人释放
            VMM::VMA::AddRegion(pagemap, start, (end - start) / PAGE_SIZE, flags);
            __memcpy((void*)phdr->p_vaddr, (void*)(data + phdr->p_offset), filesz);
            if (phdr->p_memsz > filesz)
                _memset((void*)(phdr->p_vaddr + filesz), 0, phdr->p_memsz - filesz);
            if (end > max_vaddr)
                max_vaddr = end;
            kinfoln("ELF LOADER PT_LOAD: vaddr=[0x%lx~0x%lx], filesz=0x%lx, memsz=0x%lx, flags=0x%x",
                    phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz,
                    filesz, phdr->p_memsz, phdr->p_flags);
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

    // 恢复进入时的地址空间(!smp_started 时 SwitchPageMap 返回 nullptr,兜底内核)
    VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
    max_vaddr += PAGE_SIZE;
    VMM::VMA::SetStart(pagemap, max_vaddr, 1);
    kpokln("LOAD ELF!");

    return hdr->e_entry;
}

/* 兼容包装: 保持原 6 参数签名 —— task.cpp 的 extern 声明按此签名 mangle,
 * 改参数表会链接失败。新调用方(sys_load)走 elf_load_impl 带长度校验。 */
uint64_t elf_load(uint8_t *data, pagemap_t *pagemap,
                  uint64_t *tls_offset = nullptr,
                  uint64_t *tls_memsz = nullptr,
                  uint64_t *tls_filesz = nullptr,
                  uint64_t *tls_align = nullptr) {
    return elf_load_impl(data, pagemap, tls_offset, tls_memsz,
                         tls_filesz, tls_align, 0);
}

extern uint64_t sched_tid;
extern uint32_t sched_prio_to_weight[16];

art_tree *NOT_RUNQ_P;
spinlock_t NOT_RUNQ_LOCK = 0;

/* ============ 统一失败回收 ============
 * thread 各字段由 memset 清零, 按非空判断已分配项;
 * 用户侧资源(ELF镜像/用户栈/信号栈/TLS)随 pagemap 一并销毁。
 * 所有失败路径 CR3 已归位 caller_pm, 此处不再切地址空间。 */
static void sys_load_fail(proc_t *proc, thread_t *thread,
                          char *path, uint8_t *buffer,
                          char **argv, int argc, char **envp, int envc) {
    if (path)   kfree(path);
    if (buffer) kfree(buffer);
    execve_cleanup(argc, envc, argv, envp);
    if (thread) {
        if (thread->fx_area)      VMM::Free(kernel_pagemap, thread->fx_area);
        if (thread->kernel_stack) VMM::Free(kernel_pagemap, (void*)thread->kernel_stack);
        kfree(thread);
    }
    if (proc) {
        if (proc->FDMan) {                       // 修复: 原来 kfree(parent) 漏掉的
            fd_manager_destroy(proc->FDMan);
            kfree(proc->FDMan);
        }
        if (pid2proc_tree) {
            uint64_t fl = spin_lock_irqsave(&PID2PROC_TREE_LOCK);
            art_delete(pid2proc_tree, (const uint8_t*)&proc->id, 8);
            spin_unlock_irqrestore(&PID2PROC_TREE_LOCK, fl);
        }
        if (proc->pagemap && proc->pagemap != kernel_pagemap)
            VMM::DestroyPM(proc->pagemap);       // 修复: 整个用户地址空间
        kfree(proc);
    }
}

uint64_t sys_load(uint64_t u_pathname, uint64_t u_argv, uint64_t u_envp, \
    GENERATE_IGN3()) {

    IGNV_3();

    if (unlikely(!is_user_address(u_pathname))) return -EFAULT;
    // POSIX 允许 argv/envp 为 NULL(空参数表); 非空才校验
    if (u_argv && !is_user_address(u_argv)) return -EFAULT;
    if (u_envp && !is_user_address(u_envp)) return -EFAULT;

    proc_t *caller = Schedule::this_proc();
    if (unlikely(!caller)) return -EPERM;                  // 修复: 空指针防御
    if (!caller->IsTrusted) return -EPERM;

    // 记住 syscall 进入时的地址空间(current_thread->pagemap 必为活跃 CR3)
    thread_t *my_thread = Schedule::this_thread();
    pagemap_t *caller_pm = (smp_started && my_thread && my_thread->pagemap)
                               ? my_thread->pagemap
                               : (pagemap_t*)kernel_pagemap;

    /* ---- 用户参数拷贝(带上界) ---- */
    char *pathname = nullptr;
    int64_t r = copy_user_str((const char*)u_pathname, &pathname, SYS_MAX_PATH);
    if (unlikely(r < 0)) return r;

    char **argv = nullptr, **envp = nullptr;
    int argc = 0, envc = 0;
    r = copy_user_strarray((char**)u_argv, &argv, &argc, SYS_MAX_ARGC);
    if (likely(r == 0))
        r = copy_user_strarray((char**)u_envp, &envp, &envc, SYS_MAX_ENVC);
    if (unlikely(r < 0)) { kfree(pathname); return r; }    // strarray 失败自清理

    /* ---- Create New Process (NOT TRUST AS DEFAULT FOR SAFE) ---- */
    proc_t *parent = Schedule::NewProcess(true, false);
    if (unlikely(!parent)) {
        sys_load_fail(nullptr, nullptr, pathname, nullptr, argv, argc, envp, envc);
        return -ENOMEM;
    }

    thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (unlikely(!thread)) {
        sys_load_fail(parent, nullptr, pathname, nullptr, argv, argc, envp, envc);
        return -ENOMEM;
    }
    _memset(thread, 0, sizeof(thread_t));

    cpu_t *cpu = get_lw_cpu();
    if (unlikely(!cpu)) {
        sys_load_fail(parent, thread, pathname, nullptr, argv, argc, envp, envc);
        return -EFAULT;
    }
    uint32_t cpu_num = cpu->id;
    uint32_t priority = my_thread ? my_thread->priority : 10;   // 修复: 空指针兜底

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
    if (unlikely(!MP)) {
        kerrorln("Cannot Find Mount Point!!!");
        sys_load_fail(parent, thread, pathname, nullptr, argv, argc, envp, envc);
        return -ENOENT;
    }
    void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    if (unlikely(!FileDesc)) {
        sys_load_fail(parent, thread, pathname, nullptr, argv, argc, envp, envc);
        return -ENOMEM;
    }
    _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
    if (MP->FSOPS->open(FileDesc, pathname, O_RDONLY) != 0) {
        kfree(FileDesc);
        sys_load_fail(parent, thread, pathname, nullptr, argv, argc, envp, envc);
        return -EACCES;
    }
    uint64_t FSize = MP->FSOPS->fsize(FileDesc);
    if (unlikely(FSize < sizeof(Elf64::Elf64_Ehdr))) {     // 修复: 空文件/过小文件
        MP->FSOPS->close(FileDesc); kfree(FileDesc);
        sys_load_fail(parent, thread, pathname, nullptr, argv, argc, envp, envc);
        return -EINVAL;
    }
    uint8_t *buffer = (uint8_t*)kmalloc(FSize);
    if (unlikely(!buffer)) {
        MP->FSOPS->close(FileDesc); kfree(FileDesc);
        sys_load_fail(parent, thread, pathname, nullptr, argv, argc, envp, envc);
        return -ENOMEM;
    }
    MP->FSOPS->read(FileDesc, buffer, FSize, 0);
    MP->FSOPS->close(FileDesc);
    kfree(FileDesc);
    kfree(pathname); pathname = nullptr;

    uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
    _memset(&thread->ctx, 0, sizeof(context_t));
    thread->ctx.rip = elf_load_impl(buffer, thread->pagemap,     // 修复: 带长度校验
                                    &tls_offset, &tls_memsz, &tls_filesz, &tls_align, FSize);
    if (unlikely(thread->ctx.rip == 0)) {
        kerrorln("ELF load failed!");
        sys_load_fail(parent, thread, nullptr, buffer, argv, argc, envp, envc);
        return -EINVAL;
    }

    // fx_area 必须内核态私有(user=true 会映射成 U/S=1, 用户程序可直接读写)
    thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), false);
    if (unlikely(!thread->fx_area)) {
        sys_load_fail(parent, thread, nullptr, buffer, argv, argc, envp, envc);
        return -ENOMEM;
    }
    _memset(thread->fx_area, 0, cpu->XsaveSize);
    cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

    uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
    if (unlikely(!kernel_stack)) {
        sys_load_fail(parent, thread, nullptr, buffer, argv, argc, envp, envc);
        return -ENOMEM;
    }
    _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
    thread->kernel_stack = kernel_stack;
    thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);

    uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
    if (unlikely(!thread_stack)) {
        sys_load_fail(parent, thread, nullptr, buffer, argv, argc, envp, envc);
        return -ENOMEM;
    }
    thread->stack = thread_stack;
    thread->thread_stack = thread_stack + 8 * PAGE_SIZE;

    uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
    if (unlikely(!sig_stack)) {
        sys_load_fail(parent, thread, nullptr, buffer, argv, argc, envp, envc);
        return -ENOMEM;
    }
    thread->sig_stack = sig_stack;

    thread->ctx.cs = 0x23;
    thread->ctx.ss = 0x1b;
    thread->ctx.rflags = 0x202;
    thread->ctx.rsp = thread->thread_stack;
    VMM::SwitchPageMap(thread->pagemap);        // 写用户栈需在目标地址空间下
    Schedule::PrepareUserStack(thread, argc, argv, envp);
    thread->thread_stack = thread->ctx.rsp;
    VMM::SwitchPageMap(caller_pm);              // 归位(PrepareUserStack 内部自恢复)

    if (tls_memsz > 0) {
        if (tls_align == 0) tls_align = 16;
        uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
        uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
        uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
        if (unlikely(!tls_mem)) {
            sys_load_fail(parent, thread, nullptr, buffer, argv, argc, envp, envc);
            return -ENOMEM;
        }
        uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
        VMM::SwitchPageMap(thread->pagemap);
        __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)(buffer + tls_offset), tls_filesz);
        *(uint64_t*)tcb_base = tcb_base;
        VMM::SwitchPageMap(caller_pm);
        thread->fs = tcb_base;
        thread->tls_base = tls_mem;
        thread->tls_pages = tls_pages;
    }

    kfree(buffer);
    execve_cleanup(argc, envc, argv, envp);

    thread->state = THREAD_RUNNING;

    if (unlikely(!NOT_RUNQ_P)) {                // Schedule::Init 未跑过的兜底
        sys_load_fail(parent, thread, nullptr, nullptr, nullptr, 0, nullptr, 0);
        return -EINVAL;
    }

    /* 并发回收防御: 进程已入 pid2proc_tree, kill(pid) 可在
    sys_load 执行期间启动 DeleteProc → idle 回收 kfree(proc)。
    挂链前把关: exiting 已置位则放弃, proc 交给回收方,
    本侧只清理 thread 的内核资源 (用户侧随 pagemap 回收)。
    注: 检查与挂链间仍有窄窗口, 彻底修复需 proc 引用计数。 */
    if (unlikely(parent->exiting)) {
        if (thread->fx_area)      VMM::Free(kernel_pagemap, thread->fx_area);
        if (thread->kernel_stack) VMM::Free(kernel_pagemap, (void*)thread->kernel_stack);
        kfree(thread);
        return -ESRCH;
    }

    // 线程账本: 先挂链再发布(launch 侧看到的 proc->threads 必然有效)
    Schedule::Internal::ProcessAddThread(parent, thread);

    spinlock_lock(&NOT_RUNQ_LOCK);
    art_insert(NOT_RUNQ_P, (const uint8_t*)&parent->id, 8, parent);
    spinlock_unlock(&NOT_RUNQ_LOCK);

    kpokln("LOADED PID:%d", parent->id);

    return parent->id;                          // CR3 == caller_pm, sysret 安全
}

uint64_t sys_launch(uint64_t pid, GENERATE_IGN5()){
    IGNV_5();
    cpu_t *me = this_cpu();
    if (unlikely(!me)) return -EFAULT;
    cpu_t *cpu = get_lw_cpu();
    if (unlikely(!cpu)) return -EFAULT;

    thread_t *thread = nullptr;
    proc_t *proc = nullptr;

    // 搜索 + 摘除同锁:并发 launch 同一 pid 天然串行 ——
    // 先到者删除树节点,后到者 art_search 落空返回 -ESRCH
    spinlock_lock(&NOT_RUNQ_LOCK);
    proc = (proc_t*)art_search(NOT_RUNQ_P, (const uint8_t*)&pid, 8);
    if (unlikely(!proc)) {
        spinlock_unlock(&NOT_RUNQ_LOCK);
        kwarnln("sys_launch: PROC NOT FOUND");
        return -ESRCH;
    }

    thread = proc->threads;
    if (unlikely(!thread)) {
        spinlock_unlock(&NOT_RUNQ_LOCK);
        kwarnln("THIS PROC IS EMPTY!!!");
        return -ESRCH;
    }

    if (thread->on_rq || thread->state != THREAD_RUNNING || proc->exiting) {
        spinlock_unlock(&NOT_RUNQ_LOCK);
        kwarnln("sys_launch: pid %d not launchable", (int)pid);
        return -EINVAL;
    }

    art_delete(NOT_RUNQ_P, (const uint8_t*)&pid, 8);   // 成功路径摘除
    spinlock_unlock(&NOT_RUNQ_LOCK);

    kinfoln("PROC->ID %d", proc->id);

    /* 入队 + pid 注册收进同一个 sched_lock 临界区 
     * 原实现 launch 后进程不在任何树里(NewProcess 不入 pid2proc_tree,
     * NOT_RUNQ 又被摘掉) → waitpid/kill/ps 全部失明。
     * 顺序约束:
     *   先注册后入队 → 存在"已注册但未入队"窗口, 并发 kill 会
     *     走 KillThread 的 on_rq==false 分支(线程泄漏且状态错乱);
     *   先入队后注册(无锁) → 快速退出的进程可能在注册前就被 idle
     *     回收, 随后的 art_insert 插入已释放的 proc (UAF)。
     * 持 sched_lock 串行化 kill 路径(KillThread 也要拿同一把锁),
     * 两个窗口同时消除。锁序 sched_lock → PID2PROC 无反向持有者, 安全。 */
    uint64_t rflags;
    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
    spinlock_lock(&cpu->sched_lock);

    if (unlikely(thread->on_rq || thread->state == THREAD_ZOMBIE)) {
        // 纯防御: 摘树后线程对外不可见, 理论上无人能改它
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        kwarnln("sys_launch: pid %d not launchable", (int)pid);
        return -EINVAL;
    }

    thread->state = THREAD_RUNNING;
    thread->timer_cpu = cpu->id;
    thread->cpu_num = cpu->id;
    // 不得再调 ProcessAddThread —— sys_load 已挂链, 二次挂链打坏环形链表
    cpu->has_runnable_thread = true;
    Schedule::Internal::InsertToQueue(cpu, thread);

    spinlock_unlock(&cpu->sched_lock);
    asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

    // 唤醒目标 CPU:"塞进队列"和"CPU 知道要跑它"是两件事
    if (cpu != me)
        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
    else
        asm volatile("int %0" :: "i"(SCHED_VEC + 1));

    return 0;
}