// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/art.h>
#include <atomic/atomic.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/cpu/smap.h>      // SmapGuard
#include <arch/x86_64/vmm/vmm.h>       // UserAccess::CopyToUser / is_user_address
#include <arch/x86_64/dosvm/dosvm_contract.h>

extern art_tree *pid2proc_tree;

uint64_t sys_getpid(GENERATE_IGN6()) {
    IGNV_6();
    return Schedule::this_proc()->id;
}

uint64_t sys_gettid(GENERATE_IGN6()) {
    IGNV_6();
    return Schedule::this_thread()->id;
}

uint64_t sys_exit(uint64_t code, GENERATE_IGN5()) {
    IGNV_5();
    /* 退出前把"用户态在哪一条指令上调用 exit"记下来 —— 这是"进程怎么自己
       退了"唯一可靠的定位手段: 内核日志给出这个 rip, 拿它去 objdump 对应的
       ELF 就能落到具体函数/行。syscall_handler 把当前帧通过第 7 个参数
       传进来 (见 syscall.h 的 nullframe)。 */
    if (nullframe) {
        kinfoln("[exit] pid=%d code=%d user_rip=%p user_rsp=%p",
                Schedule::this_proc() ? (int)Schedule::this_proc()->id : -1,
                (int)code, (void *)nullframe->rip, (void *)nullframe->rsp);
    }
    Schedule::Exit((int32_t)code);
    return 0;
}

uint64_t sched_yield(GENERATE_IGN6()){
    IGNV_6();

    Schedule::Yield();
    return 0;
}

extern spinlock_t PID2PROC_TREE_LOCK;
uint64_t sys_kill(uint64_t pid,uint64_t sig, GENERATE_IGN4()) {
    IGNV_4();

    proc_t *me = Schedule::this_proc();
    if (!me || !me->IsTrusted) return -EPERM;

    spinlock_lock(&PID2PROC_TREE_LOCK);
    proc_t *proc = (proc_t*)art_search(pid2proc_tree,(const uint8_t*)&pid,8);
    /* 树锁内取引用 —— 解锁后 zombie 回收器可能已
       摘除该 proc, 引用保证 DeleteProc 前的使用安全。 曾
       隔离回退; 证明 fault 根因是 D7, 本接线恢复 */
    if (proc) proc_get(proc);
    spinlock_unlock(&PID2PROC_TREE_LOCK);
    if (!proc || proc->exiting) { if (proc) proc_put(proc); return -ESRCH; }
    if (proc->pagemap == kernel_pagemap) { proc_put(proc); return -EPERM; }  /* never kill a kernel proc */

    /* sig==0 = 存活探测 (POSIX kill(pid,0)) —— WM 用于死窗口
       回收轮询, 不击杀 */
    if (sig == 0) { proc_put(proc); return 0; }

    if (proc == me) { proc_put(proc); return -EPERM; }    /* use sys_exit for self */

    /* Safe teardown of an unrelated process: mark it exiting, synchronously
       pull every thread off all CPUs, close its FDs, and queue the body for
       asynchronous reclaim. The caller keeps running (PROC_KILL is self-kill). */
    Schedule::DeleteProc(proc);
    proc_put(proc);                    /* 释放 kill 路径引用 */
    return 0;
}

uint64_t sys_fork(syscall_frame_t *frame){
    Schedule::PAUSE();
    proc_t* proc = Schedule::ForkProcess();
    if (!proc) { Schedule::Resume(); return (uint64_t)-1; }
    thread_t *thread = Schedule::ForkThread(proc, Schedule::this_thread(), frame);
    if (!thread) { Schedule::Resume(); return (uint64_t)-1; }
    Schedule::Resume();
    return proc->id;
}

extern uint64_t sched_tid;
extern uint32_t sched_prio_to_weight[16];

uint64_t sys_thread_launch(uint64_t entry, uint64_t hint, GENERATE_IGN4()){

    IGNV_4();
    proc_t *me = Schedule::this_proc();
    thread_t *curr = Schedule::this_thread();
    if (!me || !curr) return -EPERM;
    if (!is_user_address(entry)) return -EINVAL;    /* 入口必须是用户地址 */

    /* hint: UINT64_MAX 内核选核, 否则指定核 */
    cpu_t *cpu;
    if (hint != UINT64_MAX && hint < MAX_CPU && smp_cpu_list[hint])
        cpu = smp_cpu_list[hint];
    else
        cpu = get_lw_cpu();
    if (!cpu) return -EFAULT;
    /* Tier0 / dosvm: hint 可以显式指向一个已下线核; 在分配/入队之前
       拒绝, 否则新线程会被塞进该核的 runqueue, 且唤醒 IPI 被底层丢弃,
       线程要等核回来才可能被调度。进程/线程结构尚未挂链, -EBUSY 可重试。 */
    if (dosvm_core_is_offline(cpu->id)) return -EBUSY;

    thread_t *t = (thread_t*)kmalloc(sizeof(thread_t));
    if (!t) return -ENOMEM;
    _memset(t, 0, sizeof(thread_t));
    t->state = THREAD_RUNNING;              

    t->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
    t->timer_cpu = cpu->id;
    t->cpu_num = cpu->id;
    t->parent = me;
    t->pagemap = me->pagemap;                /* 共享地址空间 = 线程的本质 */
    t->priority = curr->priority;
    t->weight = sched_prio_to_weight[t->priority];
    uint64_t base_vruntime = cpu->avg_vruntime;
    uint64_t half_slice = cpu->base_quantum / 2;
    t->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;


    t->fx_area = (char*)VMM::Alloc(kernel_pagemap,
                            DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), false);
    if (!t->fx_area) { kfree(t); return -ENOMEM; }
    _memset(t->fx_area, 0, cpu->XsaveSize);
    cpu->OverLoadableFuncs.StoreSIMDState(t->fx_area,
                                          cpu->XsaveMaskLo, cpu->XsaveMaskHi);

    /* 内核栈 */
    uint64_t kstack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
    if (!kstack) { VMM::Free(kernel_pagemap, t->fx_area); kfree(t); return -ENOMEM; }
    _memset((void*)kstack, 0, 4 * PAGE_SIZE);
    t->kernel_stack = kstack;
    t->kernel_rsp = kstack + PAGE_SIZE * 4;

    /*  用户栈 — 内核分配 (无 stack 参数的代价, 内核必须管):
       在调用者的 pagemap 里分配。
       原来是 8 页 (32 KiB): 桌面合成器的 worker 在压力下会打到栈底 ——
       实测崩溃点 RSP 恰好等于缺页地址 (CR2), 且 RIP 在
       flush_tls_scb_all (线程退出时调 allocator_thread_exit_cleanup),
       即**栈已经溢出到未映射页**。32 页 (128 KiB) 给 worker 留足余量,
       每个线程多 96 KiB, 4 个 worker 约多 384 KiB。 */
    uint64_t ustack = (uint64_t)VMM::Alloc(me->pagemap, USER_THREAD_STACK_PAGES, true);
    if (!ustack) {
        VMM::Free(kernel_pagemap, (void*)kstack);
        VMM::Free(kernel_pagemap, t->fx_area);
        kfree(t);
        return -ENOMEM;
    }
    t->stack = ustack;

    /* 用户上下文 — 无 arg。
       初始 rsp 必须落在**映射区内**并留出一个可读的槽位:
       原来写成 `ustack + N*PAGE` 恰好越过最后一个有效字节, 该地址未映射。
       任何在第一次 push 之前就读 [rsp] 的指令 (ret / pop / 取栈上参数 /
       栈保护读) 都会立刻缺页, 而且 CR2 与 RSP 完全相同、且是页对齐的 ——
       与实测崩溃日志 (RSP==CR2==0x...000, ERROR_CODE=4 读, 用户态) 完全吻合。
       这里减 8: [rsp] 落在最后一个有效字节内, 同时保持 16 字节对齐要求
       (纯线程入口没有返回地址, 减 8 是常见做法)。 */
    t->ctx.rip = entry;
    t->ctx.rsp = ((ustack + USER_THREAD_STACK_PAGES * PAGE_SIZE) & ~0xFULL) - 8;
    t->ctx.rdi = 0;                          /* 定义死: arg == 0 */
    t->ctx.cs = 0x23; t->ctx.ss = 0x1b; t->ctx.rflags = 0x202;
    t->thread_stack = t->ctx.rsp;

    /* ---- Per-thread TLS: clone the caller TLS block --------------------
       Threads share the pagemap, but every thread needs its OWN static-TLS
       area and its own TCB (the [fs:0] self pointer). Snapshot the caller's
       whole TLS block, then relocate the TCB self pointer into the new copy.
       The context switch loads it through WRFSBASE(t->fs). A caller without
       TLS leaves t->fs == 0, preserving the previous behaviour. */
    if (curr->tls_base && curr->tls_pages) {
        uint64_t new_tls = (uint64_t)VMM::Alloc(me->pagemap, curr->tls_pages, true);
        if (!new_tls) {
            VMM::Free(me->pagemap, (void*)ustack);
            VMM::Free(kernel_pagemap, (void*)kstack);
            VMM::Free(kernel_pagemap, t->fx_area);
            kfree(t);
            return -ENOMEM;
        }
        uint64_t new_fs = curr->fs + (new_tls - curr->tls_base);
        { SmapGuard tls_ug;   /* both source and dest TLS blocks are user pages */
          __memcpy((void*)new_tls, (void*)curr->tls_base,
                   curr->tls_pages * PAGE_SIZE);
          *(uint64_t*)new_fs = new_fs;             /* relocate [fs:0] self ptr */
        }
        t->tls_base  = new_tls;
        t->tls_pages = curr->tls_pages;
        t->fs        = new_fs;
    }

    /* ---- 一切就绪, 最后一刻发射 (此后 t 不可再碰) ---- */
    if (!Schedule::Internal::ProcessAddThread(me, t)) {
        /* 锁内重检失败 —— me 已退出, 归还已分配资源 */
        if (t->kernel_stack) VMM::Free(kernel_pagemap, (void*)t->kernel_stack);
        kfree(t);
        return (uint64_t)-1;
    }

    uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
    cpu->has_runnable_thread = true;
    Schedule::Internal::InsertToQueue(cpu, t);
    spin_unlock_irqrestore(&cpu->sched_lock, rflags);

    /* 唤醒目标核 (launch 语义: 注册即启动, 核可能睡着不知道队列有货) */
    cpu_t *self = this_cpu();
    /* Tier0 / dosvm: 离线核不收 64 位语义 SCHED_VEC (底层也会丢)。 */
    if (cpu != self && !dosvm_core_is_offline(cpu->id))
        LAPIC::IPI(cpu->lapic_id, SCHED_VEC);

    return t->id;
}

/* ============================================================
 * sys_child_event —— 子进程退出事件 (固定 syscall 31, ABI v1)
 * ------------------------------------------------------------
 * 参数: a0 = 用户缓冲指针，指向 8 字节结构 { int32 pid; int32 code; }
 * 返回:  1 = 成功复制一条事件到用户缓冲
 *        0 = 当前进程的事件队列为空
 *       <0 = -errno (-EINVAL 无进程/非法缓冲, -EFAULT 用户拷贝失败)
 *
 * 语义固定：事件由内核在子进程退出时排队到父进程（sys_load/fork 建立的
 * parent 关系）的固定 ring（PROC_CHILD_EVENT_CAP=8）；ring 满时丢最旧，
 * 调用方可保留低频 sys_kill(pid,0) 兜底扫描。消费不阻塞；同一进程多线程
 * 并发调用安全（PROC_LIST_LOCK 串行化 tail 推进），每条事件只会被一个
 * 调用者取到。
 * ============================================================ */
struct child_event_user {
    int32_t pid;
    int32_t code;
};

uint64_t sys_child_event(uint64_t u_buf, GENERATE_IGN5()) {
    IGNV_5();
    proc_t *me = Schedule::this_proc();
    if (!me) return -EINVAL;
    /* 先做用户地址校验（不触碰用户页），拷贝放到锁外，避免缺页时持锁 */
    if (u_buf == 0 || !is_user_address(u_buf) ||
        !is_user_buffer_valid(u_buf, sizeof(struct child_event_user)))
        return -EINVAL;

    int32_t pid = 0;
    int32_t code = 0;
    if (!Schedule::ConsumeChildExitEvent(&pid, &code))
        return 0;

    struct child_event_user ev;
    ev.pid = pid;
    ev.code = code;
    if (!VMM::UserAccess::CopyToUser(me->pagemap, u_buf, &ev, sizeof(ev)))
        return -EFAULT;
    return 1;
}