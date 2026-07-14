//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/allin.h>
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>
#include <klib/algorithm/art.h>
#include <atomic/atomic.h>

static uint64_t sched_tid = 0;
static uint64_t sched_pid = 0;
art_tree *pid2proc_tree = nullptr;
spinlock_t PID2PROC_TREE_LOCK = 0;

#define AGING_THRESHOLD_BASE 50
#define SCHED_STEAL_BATCH 8
#define MAX_PROMOTE_SNAPSHOT 256

// 全局分片红黑树
rb_sharded_root_t res_tree;

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap, 
                  uint64_t *tls_offset = nullptr, 
                  uint64_t *tls_memsz = nullptr, 
                  uint64_t *tls_filesz = nullptr, 
                  uint64_t *tls_align = nullptr);

void sched_idle() {
    while (true) {
        Schedule::PAUSE();
        asm volatile("sti; hlt; cli" ::: "memory");
        Schedule::Resume();
    }
}

static cpu_t *get_lw_cpu(cpu_t *ref_cpu = nullptr) {
    cpu_t *lw_cpu = nullptr;
    uint32_t ref_mask = ref_cpu ? cpu_simd_mask(ref_cpu) : 0;

    for (int32_t i = 0; i <= smp_last_cpu; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (cpu == nullptr) continue;
        if (ref_cpu && cpu_simd_mask(cpu) != ref_mask) continue;

        if (!lw_cpu) { lw_cpu = cpu; continue; }
        uint32_t current_count = atomic_load_4(&cpu->thread_count, 1);
        uint32_t lowest_count  = atomic_load_4(&lw_cpu->thread_count, 1);
        if (current_count < lowest_count) lw_cpu = cpu;
    }
    return lw_cpu ? lw_cpu : (ref_cpu ? ref_cpu : this_cpu());
}

namespace Schedule {

    // ==========================================
    // 红黑树回调函数实现
    // ==========================================
    static void res_lock(void* ctx) { spinlock_lock((spinlock_t*)ctx); }
    static void res_unlock(void* ctx) { spinlock_unlock((spinlock_t*)ctx); }
    // 读写锁退化为自旋锁，保护树结构修改
    static void res_rdlock(void* ctx) { spinlock_lock((spinlock_t*)ctx); }
    static void res_rdunlock(void* ctx) { spinlock_unlock((spinlock_t*)ctx); }

    static void* res_alloc_lock(uint32_t shard_idx) {
        spinlock_t* lock = (spinlock_t*)kmalloc(sizeof(spinlock_t));
        *lock = 0;
        return lock;
    }
    static void res_free_lock(void* ctx) { kfree(ctx); }
    
    static void* res_alloc_mem(size_t size) { return kmalloc(size); }
    static void res_free_mem(void* ptr) { kfree(ptr); }

    static uint32_t res_hash_fn(const void* key) {
        int64_t id = *(const int64_t*)key;
        return rb_hash_u64((const void*)(uintptr_t)id);
    }

    static const void* res_key_of(const rb_node_t* node) {
        KernelResource_t* res = container_of(node, KernelResource_t, node);
        return &res->res_id;
    }

    static int res_cmp(const rb_node_t* a, const rb_node_t* b) {
        KernelResource_t* ra = container_of(a, KernelResource_t, node);
        KernelResource_t* rb = container_of(b, KernelResource_t, node);
        if (ra->res_id < rb->res_id) return -1;
        if (ra->res_id > rb->res_id) return 1;
        return 0;
    }

    // 初始化资源表
    void InitResourceTable() {
        rb_shard_ops_t ops = {
            .hash_fn = res_hash_fn,
            .key_of = res_key_of,
            .cmp = res_cmp
        };
        // 初始化 256 个分片
        if (!rb_sharded_init(&res_tree, 256, &ops, 
                             res_lock, res_unlock, 
                             res_rdlock, res_rdunlock,
                             res_alloc_lock, res_free_lock,
                             res_alloc_mem, res_free_mem)) {
            Panic("Resource RBTree init failed!");
        }
    }

    bool AcquireResource(int64_t res_id) {
        thread_t* curr = this_thread();
        if (!curr) return false;

        const void* key = &res_id;
        // 获取对应分片
        rb_root_t* shard = rb_get_shard(&res_tree, key);
        
        KernelResource_t search_node;
        search_node.res_id = res_id;

        // 写锁保护：防止 TOCTOU 并发隐患，查找和插入必须是原子的
        RB_WLOCK(shard);
        
        rb_node_t* found_node = rb_search_locked_only(shard, &search_node.node, res_tree.ops.cmp);
        KernelResource_t* res = nullptr;

        if (found_node) {
            res = container_of(found_node, KernelResource_t, node);
        } else {
            // 资源不存在，分配并插入
            res = (KernelResource_t*)kmalloc(sizeof(KernelResource_t));
            if (!res) {
                RB_WUNLOCK(shard);
                return false;
            }
            rb_init_node(&res->node);
            res->res_id = res_id;
            res->owner = nullptr;
            res->wait_head = nullptr;
            rb_insert_raw(shard, &res->node, res_tree.ops.cmp);
            shard->cnt++;
        }

        if (res->owner == nullptr) {
            // 1. 快速路径：资源无主，直接获取
            res->owner = curr;
            curr->held_resource_id = res_id;
            RB_WUNLOCK(shard);
            return true;
        }

        // 2. 冲突路径：资源被占用，阻塞当前线程
        curr->requested_resource_id = res_id;
        curr->state = THREAD_BLOCKED;

        // 加入资源等待队列 (头插法)
        curr->res_wait_next = res->wait_head;
        curr->res_wait_prev = nullptr;
        if (res->wait_head) {
            res->wait_head->res_wait_prev = curr;
        }
        res->wait_head = curr;

        // 优先级继承：防止死锁与饥饿
        if (curr->priority < res->owner->priority) {
            if (res->owner->original_priority == -1) {
                res->owner->original_priority = res->owner->priority;
            }
            res->owner->priority = curr->priority;
        }

        RB_WUNLOCK(shard);

        // 从当前 MLFQ 队列移除并触发调度
        cpu_t* cpu = this_cpu();
        spinlock_lock(&cpu->sched_lock);
        Internal::RemoveFromQueue(cpu, curr);
        spinlock_unlock(&cpu->sched_lock);

        Yield(); // 线程挂起，唤醒后说明已获得资源
        return true;
    }

    void ReleaseResource(int64_t res_id) {
        thread_t* curr = this_thread();
        const void* key = &res_id;
        rb_root_t* shard = rb_get_shard(&res_tree, key);

        KernelResource_t search_node;
        search_node.res_id = res_id;

        RB_WLOCK(shard);
        rb_node_t* found_node = rb_search_locked_only(shard, &search_node.node, res_tree.ops.cmp);
        if (!found_node) {
            RB_WUNLOCK(shard);
            return;
        }

        KernelResource_t* res = container_of(found_node, KernelResource_t, node);
        if (res->owner != curr) {
            RB_WUNLOCK(shard);
            return; // 异常：非持有者释放
        }

        curr->held_resource_id = -1;
        // 恢复优先级继承
        if (curr->original_priority != -1) {
            curr->priority = curr->original_priority;
            curr->original_priority = -1;
        }

        thread_t* to_wake = nullptr;

        if (res->wait_head) {
            // 唤醒等待队列中的第一个线程
            to_wake = res->wait_head;
            res->wait_head = to_wake->res_wait_next;
            if (res->wait_head) {
                res->wait_head->res_wait_prev = nullptr;
            }

            // 资源所有权转移
            res->owner = to_wake;
            to_wake->held_resource_id = res_id;
            to_wake->requested_resource_id = -1;
            to_wake->state = THREAD_RUNNING;
        } else {
            // 无人等待，从红黑树中擦除资源以防内存泄漏
            rb_clear_hint_if_match(shard, &res->node);
            rb_erase_raw(shard, &res->node);
            shard->cnt--;
        }

        RB_WUNLOCK(shard);

        if (to_wake) {
            // 将唤醒的线程插回 MLFQ 队列
            cpu_t* target_cpu = get_cpu(to_wake->cpu_num);
            if (!target_cpu) target_cpu = this_cpu();

            spinlock_lock(&target_cpu->sched_lock);
            to_wake->cpu_num = target_cpu->id;
            Internal::InsertToQueue(target_cpu, to_wake);
            target_cpu->has_runnable_thread = true;
            spinlock_unlock(&target_cpu->sched_lock);

            // 唤醒空闲 CPU
            if (target_cpu != this_cpu() && target_cpu->current_thread == target_cpu->idle_thread) {
                LAPIC::IPI(target_cpu->lapic_id, SCHED_VEC + 1);
            }
        } else {
            // 释放无等待者的资源节点内存
            kfree(res);
        }
    }

    namespace Internal {
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread) {
            thread_queue_t *q = &cpu->thread_queues[thread->priority];
            if (thread->list_next == thread) {
                q->head = nullptr;
                q->current = nullptr;
            } else {
                if (q->head == thread) q->head = thread->list_next;
                if (q->current == thread) q->current = thread->list_next;
                thread->list_next->list_prev = thread->list_prev;
                thread->list_prev->list_next = thread->list_next;
            }
            thread->list_next = thread->list_prev = nullptr;
            cpu->thread_count--;
            if (thread->priority > 0) cpu->thread_count_lower--;
            if (cpu->thread_count == 1) cpu->has_surplus = false;
        }

        void InsertToQueue(cpu_t *cpu, thread_t *thread) {
            thread_queue_t *q = &cpu->thread_queues[thread->priority];
            cpu->thread_count++;
            if (thread->priority > 0) cpu->thread_count_lower++;
            if (cpu->thread_count == 2) cpu->has_surplus = true;

            if (!q->head) {
                thread->list_next = thread;
                thread->list_prev = thread;
                q->head = thread;
                q->current = thread;
            } else {
                thread->list_next = q->head;
                thread->list_prev = q->head->list_prev;
                q->head->list_prev->list_next = thread;
                q->head->list_prev = thread;
            }
        }

        void Demote(cpu_t *cpu, thread_t *thread) {
            if (thread->priority == THREAD_QUEUE_CNT - 1) return;
            RemoveFromQueue(cpu, thread);
            thread->priority++;
            thread->wait_ticks = 0;
            thread->preempt_count = 0;
            InsertToQueue(cpu, thread);
        }

        void Promote(cpu_t *cpu, thread_t *thread) {
            if (thread->priority == 0) return;
            RemoveFromQueue(cpu, thread);
            thread->priority--;
            thread->wait_ticks = 0;
            InsertToQueue(cpu, thread);
        }

        void ProcessAddThread(proc_t *parent, thread_t *thread) {
            if (!parent->threads) {
                parent->threads = thread;
                thread->next = thread;
                thread->prev = thread;
                return;
            }
            thread->next = parent->threads;
            thread->prev = parent->threads->prev;
            parent->threads->prev->next = thread;
            parent->threads->prev = thread;
        }

        thread_t *StealThread(cpu_t *cpu) {
            uint32_t my_mask = cpu_simd_mask(cpu);          

            for (int32_t i = 0; i <= smp_last_cpu; i++) {
                cpu_t *victim = smp_cpu_list[i];
                if (!victim || victim == cpu) continue;
                if (cpu_simd_mask(victim) != my_mask) continue;  
                if (!atomic_load_1(&victim->has_surplus, ATOMIC_RELAXED)) continue;
                if (!__sync_bool_compare_and_swap(&victim->sched_lock, 0, 1)) continue;

                if (atomic_load_4(&victim->thread_count, 1) <= 1) {
                    atomic_store_4(&victim->sched_lock, 0, ATOMIC_RELEASE);
                    continue;
                }

                thread_t *stolen_list = nullptr;
                thread_t *stolen_tail = nullptr;
                uint32_t stolen_count = 0;

                for (int32_t p = THREAD_QUEUE_CNT - 1; p >= 0; p--) {
                    thread_queue_t *vq = &victim->thread_queues[p];
                    if (!vq->head) continue;
                    
                    while (vq->head != vq->head->list_prev && stolen_count < SCHED_STEAL_BATCH) {
                        thread_t *stolen = nullptr;
                        if (p >= THREAD_QUEUE_CNT / 2) stolen = vq->head;
                        else stolen = vq->head->list_prev;
                        
                        RemoveFromQueue(victim, stolen);
                        
                        if (!stolen_list) {
                            stolen_list = stolen;
                            stolen_tail = stolen;
                            stolen->list_next = stolen;
                            stolen->list_prev = stolen;
                        } else {
                            stolen->list_next = stolen_list;
                            stolen->list_prev = stolen_tail;
                            stolen_tail->list_next = stolen;
                            stolen_list->list_prev = stolen;
                            stolen_tail = stolen;
                        }
                        stolen_count++;
                    }
                    if (stolen_count >= SCHED_STEAL_BATCH) break;
                }

                atomic_store_4(&victim->sched_lock, 0, ATOMIC_RELEASE);
                
                if (stolen_list) {
                    thread_t *curr = stolen_list;
                    do {
                        thread_t *next = curr->list_next;
                        curr->cpu_num = cpu->id;
                        curr->wait_ticks = 0;
                        curr->preempt_count = 0;
                        InsertToQueue(cpu, curr);
                        curr = next;
                    } while (curr != stolen_list);

                    cpu->sched_stats.thread_steals += stolen_count;
                    thread_t *to_run = stolen_list;
                    RemoveFromQueue(cpu, to_run);
                    return to_run;
                }
            }
            return nullptr;
        }

        void TryPush(cpu_t *cpu) {
            if (!atomic_load_1(&cpu->has_surplus, ATOMIC_RELAXED)) return;
            uint32_t my_count = atomic_load_4((volatile uint32_t*)&cpu->thread_count, 1);
            if (my_count < 4) return; 

            uint32_t my_mask = cpu_simd_mask(cpu);
            cpu_t *target = nullptr;
            uint32_t target_count = UINT32_MAX;

            for (int32_t i = 0; i <= smp_last_cpu; i++) {
                cpu_t *other = smp_cpu_list[i];
                if (!other || other == cpu) continue;
                if (cpu_simd_mask(other) != my_mask) continue;
                uint32_t oc = atomic_load_4((volatile uint32_t*)&other->thread_count, 1);
                if (oc + 2 < my_count && oc < target_count) {
                    target = other;
                    target_count = oc;
                }
            }
            if (!target) return;

            if (!__sync_bool_compare_and_swap(&target->sched_lock, 0, 1)) return;
            uint32_t tc = atomic_load_4((volatile uint32_t*)&target->thread_count, 1);
            uint32_t mc = atomic_load_4((volatile uint32_t*)&cpu->thread_count, 1);
            if (tc + 2 >= mc) {
                atomic_store_4(&target->sched_lock, 0, ATOMIC_RELEASE);
                return;
            }

            thread_t **to_push = cpu->promote_buf;
            uint32_t push_count = 0;

            for (int32_t p = THREAD_QUEUE_CNT - 1; p >= 1 && push_count < SCHED_STEAL_BATCH; p--) {
                thread_queue_t *q = &cpu->thread_queues[p];
                if (!q->head) continue;
                thread_t *curr = q->head;
                do {
                    if (curr != cpu->current_thread && curr->state == THREAD_RUNNING && curr->held_resource_id == -1) {
                        if (push_count < SCHED_STEAL_BATCH) to_push[push_count++] = curr;
                    }
                    curr = curr->list_next;
                } while (curr != q->head && push_count < SCHED_STEAL_BATCH);
            }

            for (uint32_t j = 0; j < push_count; j++) {
                RemoveFromQueue(cpu, to_push[j]);
                to_push[j]->cpu_num = target->id;
                to_push[j]->wait_ticks = 0;
                to_push[j]->preempt_count = 0;
                InsertToQueue(target, to_push[j]);
            }
            atomic_store_4(&target->sched_lock, 0, ATOMIC_RELEASE);
        }

        thread_t *Pick(cpu_t *cpu) {
            uint32_t lower_load = cpu->thread_count_lower;
            uint32_t dynamic_base = AGING_THRESHOLD_BASE + (lower_load * 8);
            if (dynamic_base > 500) dynamic_base = 500;

            for (uint32_t i = 1; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *q = &cpu->thread_queues[i];
                if (!q->head) continue;
                thread_t** to_promote = cpu->promote_buf;
                uint32_t promote_count = 0;
                thread_t *curr = q->head;
                do {
                    if (curr->state == THREAD_RUNNING) {
                        curr->wait_ticks++;
                        cpu->sched_stats.total_wait_ticks++;
                        if (curr->wait_ticks > (dynamic_base * (i + 1))) {
                            if (promote_count < MAX_PROMOTE_SNAPSHOT) to_promote[promote_count++] = curr;
                        }
                    }
                    curr = curr->list_next;
                } while (curr != q->head && q->head != nullptr);

                for (uint32_t j = 0; j < promote_count; j++) {
                    Promote(cpu, to_promote[j]);
                    cpu->sched_stats.aging_promotions++;
                }
            }

            for (uint32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *q = &cpu->thread_queues[i];
                if (!q->head) continue;
                if (!q->current) q->current = q->head;
                thread_t *start = q->current;
                thread_t *t = start;
                do {
                    if (t->state == THREAD_RUNNING) {
                        q->current = t->list_next;
                        t->wait_ticks = 0;
                        t->preempt_count = 0;
                        return t;
                    }
                    t = t->list_next;
                } while (t != start);
            }
            return StealThread(cpu);
        }

        void Switch(context_t *ctx) {
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            if (!cpu || cpu->preempt_count > 0) {
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return;
            }

            cpu->tick_count++;
            thread_t *curr_thread = cpu->current_thread;

            if (curr_thread && curr_thread != cpu->idle_thread) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (curr_thread->fx_area) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }

            spinlock_lock(&cpu->sched_lock);

            if (curr_thread && curr_thread->state == THREAD_RUNNING
                && curr_thread != cpu->idle_thread) {
                curr_thread->preempt_count++;
                if (curr_thread->preempt_count >= SCHED_PREEMPTION_MAX)
                    Demote(cpu, curr_thread);
            }

            if ((cpu->tick_count & 0x7) == 0) TryPush(cpu);                

            thread_t *next_thread = Pick(cpu);
            if (!next_thread) next_thread = cpu->idle_thread;

            bool is_switch = (next_thread != curr_thread);
            if (is_switch) {
                cpu->current_thread = next_thread;
                cpu->sched_stats.context_switches++;
            }

            spinlock_unlock(&cpu->sched_lock);

            if (!is_switch) {
                uint64_t q = (next_thread->custom_quantum > 0) ? next_thread->custom_quantum : cpu->thread_queues[next_thread->priority].quantum;
                LAPIC::Oneshot(SCHED_VEC, q);
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return; 
            }

            *ctx = next_thread->ctx;
            TSS::SetRSP(cpu->id, 0, (void*)next_thread->kernel_rsp);
            cpu->kernel_stack = next_thread->kernel_rsp;
            
            if (!curr_thread || curr_thread->pagemap != next_thread->pagemap) {
                VMM::SwitchPageMap(next_thread->pagemap);
            }
            cpu->OverLoadableFuncs.WRFSBASE(next_thread->fs);
            if (next_thread->fx_area) {
                cpu->OverLoadableFuncs.LoadSIMDState(next_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            }
            
            uint64_t quantum = (next_thread->custom_quantum > 0) ? next_thread->custom_quantum : cpu->thread_queues[next_thread->priority].quantum;
            LAPIC::Oneshot(SCHED_VEC, quantum);
            if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
        }

        void Preempt(context_t *ctx) { Switch(ctx); }
    }

    void Init() {
        if (!pid2proc_tree) {
            pid2proc_tree = (art_tree*)kmalloc(sizeof(art_tree));
            if (art_tree_init(pid2proc_tree) != 0) Panic("ART TREE INIT FAILED!");
        }
        InitResourceTable(); // 初始化红黑树资源表
        idt_install_irq(SCHED_VEC, (void*)Schedule::Internal::Preempt);
        idt_install_irq(SCHED_VEC + 1, (void*)Schedule::Internal::Switch);
        idt_set_ist(SCHED_VEC, 0);
        idt_set_ist(SCHED_VEC + 1, 0);
    }
    
    void Install() {
        for (uint32_t i = 0; i <= smp_last_cpu; i++) {
            cpu_t *cpu = smp_cpu_list[i];
            if (!cpu) continue;
            proc_t *proc = Schedule::NewProcess(false);
            thread_t *idle_t = Schedule::NewKernelThread(proc, cpu->id, THREAD_QUEUE_CNT - 1, sched_idle);
            spinlock_lock(&cpu->sched_lock);
            Internal::RemoveFromQueue(cpu, idle_t);
            spinlock_unlock(&cpu->sched_lock);
            cpu->idle_thread = idle_t;
        }
        atomic_store_8((volatile uint8_t*)&PIT::TickHandle, (uint64_t)(uintptr_t)&PIT::Tick_, 0);
    }

    proc_t *NewProcess(bool user) {
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED);
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { kfree(proc); return nullptr; }
        fd_manager_init(proc->FDMan);
        proc->fd_count = 4;
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree,(const uint8_t*)&proc->id,8,proc);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        if (argc <= 0) return;
        char **kernel_argv = nullptr, **kernel_envp = nullptr;
        uint64_t *thread_argv = nullptr, *thread_envp = nullptr;
        int32_t envc = 0;
        kernel_argv = (char**)kmalloc(argc * sizeof(char*));
        uint64_t stack_top = thread->ctx.rsp, offset = 0;
        pagemap_t *restore;
        if (!kernel_argv) return;
        for (int32_t i = 0; i < argc; i++) kernel_argv[i] = nullptr;
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            if (!kernel_argv[i]) goto cleanup;
            __memcpy(kernel_argv[i], argv[i], size);
        }
        while (envp[envc++]); envc -= 1;
        kernel_envp = (char**)kmalloc(envc * sizeof(char*));
        if (!kernel_envp) goto cleanup;
        for (int32_t i = 0; i < envc; i++) kernel_envp[i] = nullptr;
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(envp[i]) + 1;
            kernel_envp[i] = (char*)kmalloc(size);
            if (!kernel_envp[i]) goto cleanup;
            __memcpy(kernel_envp[i], envp[i], size);
        }
        thread_argv = (uint64_t*)kmalloc(argc * sizeof(uint64_t));
        if (!thread_argv) goto cleanup;
        if ((argc + envc) % 2 == 0) offset = 8;
        restore = VMM::SwitchPageMap(thread->pagemap);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(kernel_argv[i]) + 1;
            offset += ALIGN_UP(size, 16); 
            thread_argv[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_argv[i], size);
        }
        thread_envp = kmalloc(envc * 8);
        if(!thread_envp) goto cleanup;
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(kernel_envp[i]) + 1;
            offset += ALIGN_UP(size, 16);
            thread_envp[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
        }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = envc - 1; i >= 0; i--) { offset += 8; *(uint64_t*)(stack_top - offset) = thread_envp[i]; }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = argc - 1; i >= 0; i--) { offset += 8; *(uint64_t*)(stack_top - offset) = thread_argv[i]; }
        offset += 8; *(uint64_t*)(stack_top - offset) = argc;
        VMM::SwitchPageMap(restore);
        thread->ctx.rsp = stack_top - offset;
    cleanup:
        if (kernel_argv) { for (int32_t i = 0; i < argc; i++) if (kernel_argv[i]) kfree(kernel_argv[i]); kfree(kernel_argv); }
        if (kernel_envp) { for (int32_t i = 0; i < envc; i++) if (kernel_envp[i]) kfree(kernel_envp[i]); kfree(kernel_envp); }
        if (thread_argv) kfree(thread_argv);
    }

    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->IsForkThread = false;
        thread->pagemap = parent->pagemap;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        thread->held_resource_id = -1;
        thread->requested_resource_id = -1;
        thread->original_priority = -1;
        Schedule::Internal::ProcessAddThread(parent, thread);
        
        cpu_t *cpu = get_cpu(cpu_num);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        thread->stack = kernel_stack;
        thread->ctx.rip = (uint64_t)entry;
        thread->ctx.cs = 0x08;
        thread->ctx.ss = 0x10;
        thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->kernel_rsp;
        thread->thread_stack = thread->ctx.rsp;
        thread->state = THREAD_RUNNING;

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        thread->held_resource_id = -1;
        thread->requested_resource_id = -1;
        thread->original_priority = -1;
        Schedule::Internal::ProcessAddThread(parent, thread);

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
        MP->FSOPS->close(FileDesc);
        kfree(FileDesc);
        
        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        _memset(&thread->ctx, 0, sizeof(context_t));
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align); 
        if (thread->ctx.rip == 0) { kerrorln("ELF load failed!"); kfree(buffer); kfree(thread); return nullptr; }

        cpu_t *cpu = get_cpu(cpu_num);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);

        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        thread->stack = thread_stack;
        thread->thread_stack = thread_stack + 8 * PAGE_SIZE;

        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        thread->sig_stack = sig_stack;

        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->thread_stack;
        PrepareUserStack(thread, argc, argv, envp);
        thread->thread_stack = thread->ctx.rsp;

        if (tls_memsz > 0) {
            if (tls_align == 0) tls_align = 16;
            uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
            uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
            uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
            uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
            VMM::SwitchPageMap(thread->pagemap);
            __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)(buffer + tls_offset), tls_filesz);
            *(uint64_t*)tcb_base = tcb_base; 
            VMM::SwitchPageMap(kernel_pagemap); 
            thread->fs = tcb_base;
            thread->tls_base = tls_mem;
            thread->tls_pages = tls_pages;
        }

        kfree(buffer);
        thread->state = THREAD_RUNNING;

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));

        cpu_t *parent_cpu = get_cpu(parent->cpu_num);
        cpu_t *cpu = get_lw_cpu(parent_cpu);       

        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        __memcpy((void*)kernel_stack, (void*)parent->kernel_stack, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->IsForkThread = true;
        thread->pagemap = proc->pagemap;
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack;
        thread->sig_stack = parent->sig_stack;
        thread->tls_base = parent->tls_base;
        thread->tls_pages = parent->tls_pages;
        
        thread->held_resource_id = -1;
        thread->requested_resource_id = -1;
        thread->original_priority = -1;

        Schedule::Internal::ProcessAddThread(proc, thread);
        __memcpy(&thread->ctx, frame, sizeof(context_t));
        thread->ctx.rsp = parent->thread_stack;
        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = ((syscall_frame_t*)frame)->r11;
        thread->ctx.rax = 0;
        thread->ctx.rip = ((syscall_frame_t*)frame)->rcx;
        thread->thread_stack = parent->thread_stack;
        thread->fs = rdmsr(FS_BASE);
        thread->state = THREAD_RUNNING;

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
    }

    proc_t *ForkProcess() {
        proc_t *parent = this_proc();
        if (!parent) return nullptr;
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED);
        proc->parent = parent;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last = parent->children;
            while (last->sibling) last = last->sibling;
            last->sibling = proc;
        }
        proc->pagemap = VMM::Fork(parent->pagemap);
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        __memcpy(proc->FDMan, parent->FDMan, sizeof(fd_manager_t));
        proc->fd_count = parent->fd_count;
        return proc;
    }

    thread_t* this_thread() { cpu_t* cpu = this_cpu(); return cpu ? cpu->current_thread : nullptr; }
    proc_t *this_proc() { thread_t* t = this_thread(); return t ? t->parent : nullptr; }
    
    void Yield() {
        LAPIC::StopTimer();
        asm volatile("int %0" :: "i"(SCHED_VEC + 1));
    }

    void PAUSE() { LAPIC::StopTimer(); }
    void Resume() {
        cpu_t* cpu = this_cpu();
        if (cpu) LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1); 
    }
}