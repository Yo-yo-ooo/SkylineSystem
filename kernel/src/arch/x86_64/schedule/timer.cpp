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

namespace Schedule {
    namespace Internal {

        // 将线程从时间轮双向链表中 O(1) 摘除
        void TimerRemove(thread_t* t) {
            if (t->timer_bucket == nullptr) return; // 不在任何桶里
            
            if (t->timer_prev) t->timer_prev->timer_next = t->timer_next;
            else *t->timer_bucket = t->timer_next; // 如果是头节点，更新桶头
            
            if (t->timer_next) t->timer_next->timer_prev = t->timer_prev;
            
            t->timer_bucket = nullptr;
            t->timer_next = nullptr;
            t->timer_prev = nullptr;
        }

        // 将线程 O(1) 插入到正确的时间轮桶中
        void TimerAdd(cpu_t* cpu, thread_t* t, uint64_t expires) {
            uint64_t now = PIT::TimeSinceBootMS();
            
            // 如果已经过期，直接设为运行态，不放入轮盘
            if (expires <= now) {
                t->state = THREAD_RUNNING;
                return;
            }
            
            uint64_t delta = expires - now;
            t->timer_wakeup = expires;
            t->timer_next = nullptr;
            t->timer_prev = nullptr;

            thread_t** bucket;
            if (delta < TV_SIZE) {
                // 落在第一级轮盘 (0-63ms)
                bucket = &cpu->tv1[expires & TV_MASK];
            } else if (delta < (1ULL << (2 * TV_BITS))) {
                // 落在第二级轮盘 (64-4095ms)
                bucket = &cpu->tv2[(expires >> TV_BITS) & TV_MASK];
            } else {
                // 落在第三级轮盘 (>4096ms)
                bucket = &cpu->tv3[(expires >> (2 * TV_BITS)) & TV_MASK];
            }

            // 插入到桶头部
            if (*bucket) (*bucket)->timer_prev = t;
            t->timer_next = *bucket;
            t->timer_bucket = bucket;
            *bucket = t;
        }

        // 级联函数：当低级轮盘走完一圈，把高级轮盘的节点剥落下来
        void TimerCascade(cpu_t* cpu, thread_t** tv, int idx) {
            thread_t* t = tv[idx];
            tv[idx] = nullptr; // 清空该桶
            
            while (t) {
                thread_t* next = t->timer_next;
                // 清空旧链接
                t->timer_bucket = nullptr;
                t->timer_next = nullptr;
                t->timer_prev = nullptr;
                // 重新插入，它会自动落到正确的低级轮盘里
                TimerAdd(cpu, t, t->timer_wakeup);
                t = next;
            }
        }

    }

    void Sleep(uint64_t ms){
        if (ms == 0) return;
        LAPIC::StopTimer();
        
        thread_t* current = Schedule::this_thread();
        cpu_t* cpu = this_cpu();

        uint64_t expires = PIT::TimeSinceBootMS() + ms;
        
        // 加自旋锁保护时间轮插入
        uint64_t rflags;
        asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        
        current->state = THREAD_SLEEPING;
        Schedule::Internal::TimerAdd(cpu, current, expires);
        
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        Schedule::Yield();
    }

    void Tick() {
        cpu_t *cpu = this_cpu();
        if (!cpu) return;

        uint64_t now = PIT::TimeSinceBootMS();
        
        // 【无锁 Fast-Path】
        // 时间没有推进，或者当前毫秒对应的桶是空的，直接跳过！
        if (now == cpu->timer_last_tick) return;
        
        // 加锁处理
        uint64_t rflags;
        asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);

        // 确保不越界 (如果挂起了一段时间，最多追赶到当前时间)
        while (cpu->timer_last_tick < now) {
            cpu->timer_last_tick++;
            uint64_t tick = cpu->timer_last_tick;
            
            // 1. 唤醒第一级轮盘的当前桶
            int idx1 = tick & TV_MASK;
            thread_t* t = cpu->tv1[idx1];
            cpu->tv1[idx1] = nullptr;
            while (t) {
                thread_t* next = t->timer_next;
                t->timer_bucket = nullptr;
                t->state = THREAD_RUNNING; // 唤醒！
                t = next;
            }

            // 2. 如果第一级轮盘转完一圈 (idx1 == 0)，把第二级轮盘的对应桶剥落下来
            if (idx1 == 0) {
                int idx2 = (tick >> TV_BITS) & TV_MASK;
                Schedule::Internal::TimerCascade(cpu, cpu->tv2, idx2);
                
                // 3. 如果第二级轮盘也转完一圈，把第三级的剥落下来
                if (idx2 == 0) {
                    int idx3 = (tick >> (2 * TV_BITS)) & TV_MASK;
                    Schedule::Internal::TimerCascade(cpu, cpu->tv3, idx3);
                }
            }
        }

        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
    }
} // namespace end