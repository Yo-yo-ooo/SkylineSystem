// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// task.cpp - Task Lifecycle Management
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>
#include <klib/algorithm/art.h>
#include <atomic/atomic.h>
#include <fs/fc.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/cpu/smap.h>      // SmapGuard

#ifndef likely
#define likely(x)     __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x)   __builtin_expect(!!(x), 0)
#endif

#define WAIT_THREAD_TIMEOUT_MS 1000ULL
#define KILL_RETRY_TIMEOUT_MS  1000ULL   // 修复: kill_thread_batch 重试上限

extern uint32_t sched_prio_to_weight[16];

art_tree *pid2proc_tree = nullptr;
spinlock_t PID2PROC_TREE_LOCK = 0;
spinlock_t PROC_LIST_LOCK = 0;
uint64_t sched_pid = 0;
uint64_t sched_tid = 0;

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap,
                  uint64_t *tls_offset = nullptr,
                  uint64_t *tls_memsz = nullptr,
                  uint64_t *tls_filesz = nullptr,
                  uint64_t *tls_align = nullptr);

static inline void detach_thread_from_proc(thread_t *thread) {
    if (thread->parent && thread->parent->threads) {
        if (thread->next == thread) {
            thread->parent->threads = nullptr;
        } else {
            if (thread->parent->threads == thread) thread->parent->threads = thread->next;
            thread->prev->next = thread->next;
            thread->next->prev = thread->prev;
        }
    }
    thread->parent = nullptr;
    thread->next = thread->prev = nullptr;
}

static inline void wait_for_transfer(thread_t *t) {
    uint64_t wait_start = PIT::TimeSinceBootMS();
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_TRANSFER) {
        if (PIT::TimeSinceBootMS() - wait_start > WAIT_THREAD_TIMEOUT_MS) {
            Panic("Thread stuck in TRANSFER state for too long!");
        }
        asm volatile("pause");
    }
}

static void kill_thread_batch(thread_t *target, cpu_t *self_cpu, bool &need_wait) {
    /* 修复: 重试循环加超时 —— THREAD_ZOMBIE==0 语义下, 构造中的线程
       (memset 后未赋 state) 会让 state==ZOMBIE 的 continue 分支无限自旋 */
    uint64_t batch_start = PIT::TimeSinceBootMS();

    while (true) {
        if (unlikely(PIT::TimeSinceBootMS() - batch_start > KILL_RETRY_TIMEOUT_MS))
            Panic("kill_thread_batch: target stuck (state/cpu migration race)");

        wait_for_transfer(target);

        uint32_t target_cpu_num = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
        if (target_cpu_num >= MAX_CPU || !smp_cpu_list[target_cpu_num]) {
            asm volatile("pause");
            continue;
        }
        cpu_t *t_cpu = smp_cpu_list[target_cpu_num];

        uint32_t timer_cpu_num = MAX_CPU;
        if (target->timer_bucket != nullptr) {
            timer_cpu_num = __atomic_load_n(&target->timer_cpu, __ATOMIC_ACQUIRE);
        }
        cpu_t *timer_cpu = (timer_cpu_num < MAX_CPU) ? smp_cpu_list[timer_cpu_num] : nullptr;

        bool need_ipi = false;
        uint64_t rflags;
        if (timer_cpu && timer_cpu != t_cpu) {
            cpu_t *lock1 = (t_cpu->id < timer_cpu->id) ? t_cpu : timer_cpu;
            cpu_t *lock2 = (t_cpu->id < timer_cpu->id) ? timer_cpu : t_cpu;

            rflags = spin_lock_irqsave(&lock1->sched_lock);
            spinlock_lock(&lock2->sched_lock);

            uint32_t cur_target_cpu = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
            uint32_t cur_timer_cpu = __atomic_load_n(&target->timer_cpu, __ATOMIC_ACQUIRE);

            if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) == THREAD_ZOMBIE ||
                cur_target_cpu != target_cpu_num ||
                (target->timer_bucket != nullptr && cur_timer_cpu != timer_cpu_num)) {
                spinlock_unlock(&lock2->sched_lock);
                spin_unlock_irqrestore(&lock1->sched_lock, rflags);
                continue;
            }

            target->state = THREAD_ZOMBIE;

            if (t_cpu->current_thread != target) {
                if (!target->IsForkThread && target->pagemap != kernel_pagemap) {
                    if (target->stack && target->stack != target->kernel_stack) { VMM::Free(target->pagemap, (void*)target->stack); target->stack = 0; }
                    if (target->sig_stack) { VMM::Free(target->pagemap, (void*)target->sig_stack); target->sig_stack = 0; }
                    if (target->tls_base) { VMM::Free(target->pagemap, (void*)target->tls_base); target->tls_base = 0; }
                }
            }

            if (target->timer_bucket != nullptr && cur_timer_cpu == timer_cpu->id) {
                Schedule::Internal::TimerRemove(target);
            }
            if (target->on_rq) {
                Schedule::Internal::RemoveFromQueue(t_cpu, target);
            }

            if (t_cpu->current_thread == target) {
                need_wait = true;
                need_ipi = (t_cpu != self_cpu);
            } else {
                target->zombie_next = t_cpu->zombie_list;
                t_cpu->zombie_list = target;
                t_cpu->zombie_count++;
            }

            spinlock_unlock(&lock2->sched_lock);
            spin_unlock_irqrestore(&lock1->sched_lock, rflags);
        } else {
            rflags = spin_lock_irqsave(&t_cpu->sched_lock);

            uint32_t cur_target_cpu = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
            uint32_t cur_timer_cpu = __atomic_load_n(&target->timer_cpu, __ATOMIC_ACQUIRE);

            if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) == THREAD_ZOMBIE ||
                cur_target_cpu != target_cpu_num ||
                (target->timer_bucket != nullptr && cur_timer_cpu != target_cpu_num)) {
                spin_unlock_irqrestore(&t_cpu->sched_lock, rflags);
                continue;
            }

            target->state = THREAD_ZOMBIE;

            if (t_cpu->current_thread != target) {
                if (!target->IsForkThread && target->pagemap != kernel_pagemap) {
                    if (target->stack && target->stack != target->kernel_stack) { VMM::Free(target->pagemap, (void*)target->stack); target->stack = 0; }
                    if (target->sig_stack) { VMM::Free(target->pagemap, (void*)target->sig_stack); target->sig_stack = 0; }
                    if (target->tls_base) { VMM::Free(target->pagemap, (void*)target->tls_base); target->tls_base = 0; }
                }
            }

            if (target->on_rq) {
                Schedule::Internal::RemoveFromQueue(t_cpu, target);
            } else if (target->timer_bucket) {
                Schedule::Internal::TimerRemove(target);
            }

            if (t_cpu->current_thread == target) {
                need_wait = true;
                need_ipi = (t_cpu != self_cpu);
            } else {
                target->zombie_next = t_cpu->zombie_list;
                t_cpu->zombie_list = target;
                t_cpu->zombie_count++;
            }
            spin_unlock_irqrestore(&t_cpu->sched_lock, rflags);
        }

        if (need_ipi) {
            LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC);
        }
        return;
    }
}

static void SyncKillProcThreads(proc_t *proc, thread_t *except_thread) {
    if (!proc) return;
    cpu_t *self_cpu = this_cpu();

    while (true) {
        thread_t *batch[64];
        int count = 0;
        uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
        thread_t *t = proc->threads;
        if (t) {
            thread_t *start = t;
            do {
                thread_t *next = t->next;
                if (t != except_thread && count < 64) {
                    detach_thread_from_proc(t);
                    batch[count++] = t;
                }
                t = next;
            } while (t != start && proc->threads);
        }
        spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);

        if (count == 0) break;

        bool need_wait[64] = {false};
        for (int i = 0; i < count; i++) {
            kill_thread_batch(batch[i], self_cpu, need_wait[i]);
        }
        for (int i = 0; i < count; i++) {
            if (need_wait[i]) {
                Schedule::WaitForThreadOffCpu(batch[i]);
            }
        }
    }
}

/* ============================================================
 *  Per-CPU 进程异步资源回收机制 (带水位线控制)
 * ============================================================ */
#define PROC_ZOMBIE_HIGH_WATERMARK 8
#define PROC_ZOMBIE_LOW_WATERMARK 2
#define PROC_ZOMBIE_BATCH 4

static proc_t *proc_zombie_list[MAX_CPU] = {nullptr};
static uint32_t proc_zombie_count[MAX_CPU] = {0};

static void EnqueueProcZombie(proc_t *proc) {
    uint64_t flags = irq_save();
    cpu_t *cpu = this_cpu();
    if (cpu) {
        proc->sibling = proc_zombie_list[cpu->id];
        proc_zombie_list[cpu->id] = proc;
        proc_zombie_count[cpu->id]++;
    } else {
        proc->sibling = proc_zombie_list[0];
        proc_zombie_list[0] = proc;
        proc_zombie_count[0]++;
    }
    irq_restore(flags);
}

namespace Schedule {
    void DrainProcZombieList(cpu_t *cpu) {
        if (!cpu) return;

        uint64_t rflags = irq_save();
        uint32_t count = proc_zombie_count[cpu->id];
        if (count == 0) {
            irq_restore(rflags);
            return;
        }

        uint32_t to_reclaim;
        if (count >= PROC_ZOMBIE_HIGH_WATERMARK) {
            to_reclaim = count - PROC_ZOMBIE_LOW_WATERMARK;
        } else {
            to_reclaim = (count > PROC_ZOMBIE_BATCH) ? PROC_ZOMBIE_BATCH : count;
        }
        if (to_reclaim > count) to_reclaim = count;

        proc_t *batch = nullptr;
        for (uint32_t i = 0; i < to_reclaim; i++) {
            proc_t *p = proc_zombie_list[cpu->id];
            if (!p) break;
            proc_zombie_list[cpu->id] = p->sibling;
            proc_zombie_count[cpu->id]--;

            p->sibling = batch;
            batch = p;
        }
        irq_restore(rflags);

        proc_t *p = batch;
        while (p) {
            proc_t *next = p->sibling;

            uint64_t flags = spin_lock_irqsave(&PID2PROC_TREE_LOCK);
            art_delete(pid2proc_tree, (const uint8_t*)&p->id, 8);
            spin_unlock_irqrestore(&PID2PROC_TREE_LOCK, flags);

            flags = spin_lock_irqsave(&PROC_LIST_LOCK);
            proc_t *child = p->children;
            while (child) {
                proc_t *next_child = child->sibling;
                if (__sync_lock_test_and_set(&child->exiting, 1) == 0) {
                    child->parent = nullptr;
                    child->sibling = nullptr;
                    EnqueueProcZombie(child);
                } else {
                    child->parent = nullptr;
                }
                child = next_child;
            }
            p->children = nullptr;
            spin_unlock_irqrestore(&PROC_LIST_LOCK, flags);

            if (p->pagemap && p->pagemap != kernel_pagemap) {
                VMM::DestroyPM(p->pagemap);
                p->pagemap = nullptr;
            }

            if (p->FDMan) {
                fd_manager_destroy(p->FDMan);
                kfree(p->FDMan);
                p->FDMan = nullptr;
            }

            kfree(p);
            p = next;
        }
    }
}

namespace Schedule {
    void FreeThreadResources(thread_t *thread) {
        if (thread->timer_bucket != nullptr) {
            uint32_t timer_cpu_num = __atomic_load_n(&thread->timer_cpu, __ATOMIC_ACQUIRE);
            if (timer_cpu_num < MAX_CPU) {
                cpu_t *timer_cpu = smp_cpu_list[timer_cpu_num];
                if (timer_cpu) {
                    uint64_t rflags = spin_lock_irqsave(&timer_cpu->sched_lock);
                    if (thread->timer_bucket != nullptr && __atomic_load_n(&thread->timer_cpu, __ATOMIC_ACQUIRE) == timer_cpu_num) {
                        Internal::TimerRemove(thread);
                    }
                    spin_unlock_irqrestore(&timer_cpu->sched_lock, rflags);
                }
            }
        }
        if (thread->fx_area) VMM::Free(kernel_pagemap, (void*)thread->fx_area);
        if (thread->kernel_stack) VMM::Free(kernel_pagemap, (void*)thread->kernel_stack);
    }

    void KillThread(thread_t *thread) {
        if (!thread) return;
        wait_for_transfer(thread);

        cpu_t *cpu = get_cpu(thread->cpu_num);
        if (!cpu) return;
        bool was_running = false;

        uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
        detach_thread_from_proc(thread);
        spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);

        rflags = spin_lock_irqsave(&cpu->sched_lock);
        thread->state = THREAD_ZOMBIE;
        was_running = (cpu->current_thread == thread);

        if (!was_running) {
            if (!thread->IsForkThread && thread->pagemap != kernel_pagemap) {
                if (thread->stack && thread->stack != thread->kernel_stack) { VMM::Free(thread->pagemap, (void*)thread->stack); thread->stack = 0; }
                if (thread->sig_stack) { VMM::Free(thread->pagemap, (void*)thread->sig_stack); thread->sig_stack = 0; }
                if (thread->tls_base) { VMM::Free(thread->pagemap, (void*)thread->tls_base); thread->tls_base = 0; }
            }
        }

        if (thread->on_rq) {
            Internal::RemoveFromQueue(cpu, thread);
            thread->zombie_next = cpu->zombie_list;
            cpu->zombie_list = thread;
            cpu->zombie_count++;
        } else if (thread->timer_bucket != nullptr) {
            Internal::TimerRemove(thread);
            thread->zombie_next = cpu->zombie_list;
            cpu->zombie_list = thread;
            cpu->zombie_count++;
        }
        /* 注: 既不在 rq 也不在 timer 的窗口 (如 TRANSFER 换队瞬间,
           wait_for_transfer 的 check-then-act 缝隙) 线程会泄漏 ——
           状态已是 ZOMBIE, 无人再引用它。概率极低, 完整修复需要
           以线程为中心的重试而非此处的一次性判定。 */
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);

        if (was_running) {
            cpu_t *cur_cpu = this_cpu();
            if (cpu != cur_cpu) LAPIC::IPI(cpu->lapic_id, SCHED_VEC);
        }
    }

    void KillProcessThreads(proc_t *proc) {
        SyncKillProcThreads(proc, nullptr);
    }

    void WaitForThreadOffCpu(thread_t *thread) {
        if (!thread) return;
        uint64_t start_time = PIT::TimeSinceBootMS();
        while (true) {
            uint32_t cpu_num = __atomic_load_n(&thread->cpu_num, __ATOMIC_ACQUIRE);
            if (cpu_num >= MAX_CPU) break;
            cpu_t *cpu = smp_cpu_list[cpu_num];
            if (!cpu) break;
            thread_t *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
            if (curr != thread) break;
            if (PIT::TimeSinceBootMS() - start_time > WAIT_THREAD_TIMEOUT_MS) Panic("WaitForThreadOffCpu: Thread stuck on CPU (timeout)");
            asm volatile("pause");
        }
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) return;

        uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
        if (proc->parent) {
            if (proc->parent->children == proc) proc->parent->children = proc->sibling;
            else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) sibling = sibling->sibling;
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            proc->sibling = nullptr;
        }
        spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);

        SyncKillProcThreads(proc, nullptr);

        if (proc->FDMan) {
            fd_manager_destroy(proc->FDMan);
            kfree(proc->FDMan);
            proc->FDMan = nullptr;
        }

        EnqueueProcZombie(proc);
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;

        SyncKillProcThreads(proc, curr_thread);

        uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
        if (curr_thread->next == curr_thread) proc->threads = nullptr;
        else {
            if (proc->threads == curr_thread) proc->threads = curr_thread->next;
            curr_thread->prev->next = curr_thread->next;
            curr_thread->next->prev = curr_thread->prev;
        }
        curr_thread->parent = nullptr;
        curr_thread->next = curr_thread->prev = nullptr;

        if (proc->parent) {
            if (proc->parent->children == proc) proc->parent->children = proc->sibling;
            else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) sibling = sibling->sibling;
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
        }
        spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);

        pagemap_t *pm_to_destroy = proc->pagemap;
        proc->pagemap = nullptr;
        curr_thread->pagemap = kernel_pagemap;

        if (!curr_thread->IsForkThread && pm_to_destroy != kernel_pagemap) {
            if (curr_thread->stack && curr_thread->stack != curr_thread->kernel_stack) { VMM::Free(pm_to_destroy, (void*)curr_thread->stack); curr_thread->stack = 0; }
            if (curr_thread->sig_stack) { VMM::Free(pm_to_destroy, (void*)curr_thread->sig_stack); curr_thread->sig_stack = 0; }
            if (curr_thread->tls_base) { VMM::Free(pm_to_destroy, (void*)curr_thread->tls_base); curr_thread->tls_base = 0; }
        }

        VMM::SwitchPageMap(kernel_pagemap);
        proc->pagemap = pm_to_destroy;

        if (proc->FDMan) {
            fd_manager_destroy(proc->FDMan);
            kfree(proc->FDMan);
            proc->FDMan = nullptr;
        }

        EnqueueProcZombie(proc);

        rflags = spin_lock_irqsave(&cpu->sched_lock);
        curr_thread->state = THREAD_ZOMBIE;
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);

        kinfoln("Exit PROC %d", pid);

        Schedule::Yield();
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc, int32_t exit_code){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();
        /* 修复: 空指针防御 —— Exit 路径 this_thread()/this_proc()
           可能返回 null (中断上下文/未初始化), 原实现直接解引用崩溃 */
        if (!curr_thread || !cpu || !proc) return;

        curr_thread->exit_code = exit_code;

        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) {
            VMM::SwitchPageMap(kernel_pagemap);

            uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
            if (curr_thread->parent) {
                if (curr_thread->next == curr_thread) proc->threads = nullptr;
                else {
                    if (proc->threads == curr_thread) proc->threads = curr_thread->next;
                    curr_thread->prev->next = curr_thread->next;
                    curr_thread->next->prev = curr_thread->prev;
                }
                curr_thread->parent = nullptr;
                curr_thread->next = curr_thread->prev = nullptr;
            }
            spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);

            rflags = spin_lock_irqsave(&cpu->sched_lock);
            curr_thread->pagemap = kernel_pagemap;
            curr_thread->stack = 0;
            curr_thread->sig_stack = 0;
            curr_thread->tls_base = 0;
            __atomic_store_n(&curr_thread->state, THREAD_ZOMBIE, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&cpu->sched_lock, rflags);

            Schedule::Yield();
            while(1) { asm volatile("hlt"); }
        }

        FinalizeProcExit(proc, cpu);
        while(1) { asm volatile("hlt"); }
    }

    void Exit(int32_t code) {
        asm volatile("cli"); LAPIC::StopTimer();
        proc_t *curr_proc = Schedule::this_proc();
        if (!curr_proc) {          // 修复: 无所属进程时无法退出, 原地自旋
            while(1) { asm volatile("hlt"); }
        }
        PROC_KILL(curr_proc, code);
    }

    /* ================= 进程与线程创建接口 ================= */
    namespace Internal {
        void ProcessAddThread(proc_t *parent, thread_t *thread) {
            uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
            if (!parent->threads) {
                parent->threads = thread;
                thread->next = thread;
                thread->prev = thread;
            } else {
                thread->next = parent->threads;
                thread->prev = parent->threads->prev;
                parent->threads->prev->next = thread;
                parent->threads->prev = thread;
            }
            spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);
        }
    }

    proc_t *NewProcess(bool user, bool Trusted) {
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid, 1, ATOMIC_RELAXED);
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        if (user && !proc->pagemap) { kfree(proc); return nullptr; }
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { if (user) VMM::DestroyPM(proc->pagemap); kfree(proc); return nullptr; }
        fd_manager_init(proc->FDMan);
        proc->fd_count = 0;
        proc->IsTrusted = Trusted;
        /* 你这版改为无条件入树 (好改动: pid 全局可解析)。
           连锁要求见 sys_load.cpp 补丁: 失败回收必须摘树,
           sys_launch 不得再插一遍。 */
        uint64_t rflags = spin_lock_irqsave(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree, (const uint8_t*)&proc->id, 8, proc);
        spin_unlock_irqrestore(&PID2PROC_TREE_LOCK, rflags);
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        /* 修复: envp 允许 NULL (空环境) —— 原实现 !envp 直接 return,
           连 argv/argc 都不写, 用户程序拿到垃圾栈帧 */
        /* A no-argument process (argc==0 / argv==NULL) still needs a valid
           SysV initial stack frame: RSP must point INSIDE the mapped stack at
           argc, followed by NULL-terminated argv[]/envp[]. The old early return
           left RSP exactly on the stack region's upper boundary (an unmapped
           page); it only worked because the sig-stack was allocated immediately
           above under deterministic placement. ASLR moves the sig-stack away,
           turning that page into a hole and faulting on the first pop in _start.
           Normalize to argc=0 and fall through to build the minimal frame. */
        if (argc < 0) return;
        if (argc == 0 || !argv) argc = 0;
        char **kernel_argv = nullptr, **kernel_envp = nullptr;
        uint64_t *thread_argv = nullptr, *thread_envp = nullptr;
        int32_t envc = 0; uint64_t offset = 0;
        uint64_t stack_top = 0; pagemap_t *restore = nullptr;

        kernel_argv = nullptr;
        if (argc > 0) {
            kernel_argv = (char**)kmalloc(argc * sizeof(char*));
            if (!kernel_argv) return;
            for (int32_t i = 0; i < argc; i++) kernel_argv[i] = nullptr;
        }
        for (int32_t i = 0; i < argc; i++) {
            if(!argv[i]) goto cleanup;
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            if (!kernel_argv[i]) goto cleanup;
            __memcpy(kernel_argv[i], argv[i], size);
        }

        if (envp) { while (envp[envc++]); envc -= 1; }   // 修复: envp NULL 时 envc=0
        if (envc > 0) {
            kernel_envp = (char**)kmalloc(envc * sizeof(char*));
            if (!kernel_envp) goto cleanup;
            for (int32_t i = 0; i < envc; i++) kernel_envp[i] = nullptr;
            for (int32_t i = 0; i < envc; i++) {
                if(!envp[i]) goto cleanup;
                int32_t size = strlen(envp[i]) + 1;
                kernel_envp[i] = (char*)kmalloc(size);
                if (!kernel_envp[i]) goto cleanup;
                __memcpy(kernel_envp[i], envp[i], size);
            }
        }

        thread_argv = nullptr;
        if (argc > 0) {
            thread_argv = (uint64_t*)kmalloc(argc * sizeof(uint64_t));
            if (!thread_argv) goto cleanup;
        }
        stack_top = thread->ctx.rsp;
        if ((argc + envc) % 2 == 0) offset = 8;
        restore = VMM::SwitchPageMap(thread->pagemap);
        {
        SmapGuard stack_ug;   // below we write argv/envp into the live user stack
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(kernel_argv[i]) + 1;
            offset += ALIGN_UP(size, 16);
            thread_argv[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_argv[i], size);
        }

        if (envc > 0) {
            thread_envp = (uint64_t*)kmalloc(envc * sizeof(uint64_t));
            if(!thread_envp) { VMM::SwitchPageMap(restore); goto cleanup; }
            for (int32_t i = 0; i < envc; i++) {
                int32_t size = strlen(kernel_envp[i]) + 1;
                offset += ALIGN_UP(size, 16);
                thread_envp[i] = stack_top - offset;
                __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
            }
        }

        offset += 8; *(uint64_t*)(stack_top - offset) = 0;
        for (int32_t i = envc - 1; i >= 0; i--) { offset += 8; *(uint64_t*)(stack_top - offset) = thread_envp[i]; }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0;
        for (int32_t i = argc - 1; i >= 0; i--) { offset += 8; *(uint64_t*)(stack_top - offset) = thread_argv[i]; }
        offset += 8; *(uint64_t*)(stack_top - offset) = argc;
        }   /* close SmapGuard scope; the inner goto cleanup destructs it */
        VMM::SwitchPageMap(restore);
        thread->ctx.rsp = stack_top - offset;

    cleanup:
        if (kernel_argv) { for (int32_t i = 0; i < argc; i++) if (kernel_argv[i]) kfree(kernel_argv[i]); kfree(kernel_argv); }
        if (kernel_envp) { for (int32_t i = 0; i < envc; i++) if (kernel_envp[i]) kfree(kernel_envp[i]); kfree(kernel_envp); }
        if (thread_argv) kfree(thread_argv);
        if (thread_envp) kfree(thread_envp);
    }

    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        thread->timer_cpu = cpu_num;
        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu_num; thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->priority = priority > 15 ? 15 : priority;
        thread->weight = sched_prio_to_weight[thread->priority];
        thread->state = THREAD_RUNNING;   // 修复: 提前 —— 不依赖后续路径补设
        cpu_t *cpu = get_cpu(cpu_num);
        uint64_t base_vruntime = cpu->avg_vruntime;
        uint64_t half_slice = cpu->base_quantum / 2;
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;
        thread->fx_area = (char*)VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), false);
        if (!thread->fx_area) { kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, (void*)thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack; thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        thread->stack = kernel_stack; thread->ctx.rip = (uint64_t)entry;
        thread->ctx.cs = 0x08; thread->ctx.ss = 0x10; thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->kernel_rsp; thread->thread_stack = thread->ctx.rsp;
        Schedule::Internal::ProcessAddThread(parent, thread);

        uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);
        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        thread->timer_cpu = cpu_num;
        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu_num; thread->parent = parent; thread->pagemap = parent->pagemap;
        thread->priority = priority > 15 ? 15 : priority;
        thread->weight = sched_prio_to_weight[thread->priority];
        thread->state = THREAD_RUNNING;   // 修复: 提前
        cpu_t *cpu = get_cpu(cpu_num);
        uint64_t base_vruntime = cpu->avg_vruntime;
        uint64_t half_slice = cpu->base_quantum / 2;
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;

        __hmap_s_mp *MP = GetMount(Path);
        if(!MP) { kerrorln("Cannot Find Mount Point!!!"); kfree(thread); return nullptr; }
        void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
        if (!FileDesc) { kfree(thread); return nullptr; }
        _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
        if(MP->FSOPS->open(FileDesc, Path, O_RDONLY) != 0) { kfree(FileDesc); kfree(thread); return nullptr; }
        uint64_t FSize = MP->FSOPS->fsize(FileDesc);
        uint8_t *buffer = (uint8_t*)kmalloc(FSize);
        if (!buffer) { MP->FSOPS->close(FileDesc); kfree(FileDesc); kfree(thread); return nullptr; }
        MP->FSOPS->read(FileDesc, buffer, FSize, 0);
        MP->FSOPS->close(FileDesc); kfree(FileDesc);

        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        _memset(&thread->ctx, 0, sizeof(context_t));
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align);
        if (thread->ctx.rip == 0) {
            kerrorln("ELF load failed!");
            /* 注: 此前 PT_LOAD 已 map 的页泄漏 (pagemap 归 parent, 由
               进程退出统一回收, 非 immediate UAF, 可接受) */
            kfree(buffer); kfree(thread); return nullptr;
        }

        thread->fx_area = (char*)VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), false);
        if (!thread->fx_area) { kfree(buffer); kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, (void*)thread->fx_area); kfree(buffer); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack; thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        if (!thread_stack) { VMM::Free(kernel_pagemap, (void*)thread->fx_area); VMM::Free(kernel_pagemap, (void*)kernel_stack); kfree(buffer); kfree(thread); return nullptr; }
        thread->stack = thread_stack; thread->thread_stack = thread_stack + 8 * PAGE_SIZE;
        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        if (!sig_stack) { VMM::Free(kernel_pagemap, (void*)thread->fx_area); VMM::Free(kernel_pagemap, (void*)kernel_stack); VMM::Free(thread->pagemap, (void*)thread_stack); kfree(buffer); kfree(thread); return nullptr; }
        thread->sig_stack = sig_stack;
        thread->ctx.cs = 0x23; thread->ctx.ss = 0x1b; thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->thread_stack;
        PrepareUserStack(thread, argc, argv, envp);
        thread->thread_stack = thread->ctx.rsp;

        if (tls_memsz > 0) {
            if (tls_align == 0) tls_align = 16;
            uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
            uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
            uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
            if (!tls_mem) {
                kfree(buffer);
                VMM::Free(kernel_pagemap, (void*)thread->fx_area);
                VMM::Free(kernel_pagemap, (void*)kernel_stack);
                VMM::Free(thread->pagemap, (void*)thread_stack);
                VMM::Free(thread->pagemap, (void*)sig_stack);
                kfree(thread);
                return nullptr;
            }
            uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
            VMM::SwitchPageMap(thread->pagemap);
            { SmapGuard tls_ug;   // TLS image + TCB self pointer are user pages
              __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)(buffer + tls_offset), tls_filesz);
              *(uint64_t*)tcb_base = tcb_base;
            }
            VMM::SwitchPageMap(kernel_pagemap);
            thread->fs = tcb_base; thread->tls_base = tls_mem; thread->tls_pages = tls_pages;
        }

        kfree(buffer);
        Schedule::Internal::ProcessAddThread(parent, thread);

        uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);
        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        /* 修复: state 必须在 ProcessAddThread 之前设置 ——
           THREAD_ZOMBIE==0 语义下, 挂链后再补设存在 ZOMBIE 窗口,
           并发 SyncKillProcThreads 命中窗口会让 kill_thread_batch
           在 state==ZOMBIE 的 continue 上无限自旋 (重试循环原本无超时) */
        thread->state = THREAD_RUNNING;
        cpu_t *parent_cpu = get_cpu(parent->cpu_num);
        cpu_t *cpu = get_lw_cpu(parent_cpu);
        thread->fx_area = (char*)VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), false);
        if (!thread->fx_area) { kfree(thread); return nullptr; }

        uint64_t rflags = spin_lock_irqsave(&parent_cpu->sched_lock);
        if (parent_cpu->current_thread == parent && parent->fx_area) {
            parent_cpu->OverLoadableFuncs.StoreSIMDState(parent->fx_area, parent_cpu->XsaveMaskLo, parent_cpu->XsaveMaskHi);
        }
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);
        spin_unlock_irqrestore(&parent_cpu->sched_lock, rflags);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu->id; thread->parent = proc; thread->IsForkThread = true; thread->pagemap = proc->pagemap;
        thread->kernel_stack = kernel_stack; thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack; thread->sig_stack = parent->sig_stack;
        thread->tls_base = parent->tls_base; thread->tls_pages = parent->tls_pages;
        thread->timer_cpu = cpu->id;
        Schedule::Internal::ProcessAddThread(proc, thread);   // 此时 state 已是 RUNNING
        __memcpy(&thread->ctx, frame, sizeof(context_t));
        thread->ctx.rsp = ((context_t*)frame)->rsp;
        thread->ctx.cs = 0x23; thread->ctx.ss = 0x1b; thread->ctx.rflags = ((syscall_frame_t*)frame)->r11;
        thread->ctx.rax = 0; thread->ctx.rip = ((syscall_frame_t*)frame)->rcx;
        thread->thread_stack = thread->ctx.rsp; thread->fs = rdmsr(FS_BASE);
        thread->priority = parent->priority; thread->weight = parent->weight;

        uint64_t base_vruntime = cpu->avg_vruntime;
        uint64_t half_slice = cpu->base_quantum / 2;
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;

        rflags = spin_lock_irqsave(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);
        return thread;
    }

    proc_t *ForkProcess() {
        proc_t *parent = this_proc();
        if (!parent) return nullptr;
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED); proc->parent = parent;
        proc->IsTrusted = parent->IsTrusted;   // 修复: 原实现漏设, memset 后恒为 false
        proc->pagemap = VMM::Fork(parent->pagemap);
        if (!proc->pagemap) { kfree(proc); return nullptr; }
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { VMM::DestroyPM(proc->pagemap); kfree(proc); return nullptr; }

        uint64_t rflags = spin_lock_irqsave(&PROC_LIST_LOCK);
        /* ⚠️ 已知问题: FDMan 整结构 memcpy 是浅拷贝 —— 内部指针
           (缓冲区/文件表项)被父子共享, 双方退出时 fd_manager_destroy
           双重释放。需要 fd_manager_dup() 深拷贝接口才能修, 本轮仅标注。 */
        __memcpy(proc->FDMan, parent->FDMan, sizeof(fd_manager_t));
        proc->fd_count = parent->fd_count;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last = parent->children;
            while (last->sibling) last = last->sibling;
            last->sibling = proc;
        }
        spin_unlock_irqrestore(&PROC_LIST_LOCK, rflags);

        rflags = spin_lock_irqsave(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree, (const uint8_t*)&proc->id, 8, proc);
        spin_unlock_irqrestore(&PID2PROC_TREE_LOCK, rflags);
        return proc;
    }
}