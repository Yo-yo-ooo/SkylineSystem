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

        void TimerRemove(thread_t* t) {
            if (t->timer_bucket == nullptr) return;
            
            if (t->timer_prev) t->timer_prev->timer_next = t->timer_next;
            else *t->timer_bucket = t->timer_next;
            
            if (t->timer_next) t->timer_next->timer_prev = t->timer_prev;
            
            t->timer_bucket = nullptr;
            t->timer_next = nullptr;
            t->timer_prev = nullptr;
        }

        void TimerAdd(cpu_t* cpu, thread_t* t, uint64_t expires) {
            uint64_t now = PIT::TimeSinceBootMS();
            
            if (expires <= now) {
                t->state = THREAD_RUNNING;
                return;
            }
            
            uint64_t delta = expires - now;
            t->timer_wakeup = expires;
            t->timer_next = nullptr;
            t->timer_prev = nullptr;
            t->timer_cpu = cpu->id; // 记录所在CPU

            thread_t** bucket;
            if (delta < TV_SIZE) {
                bucket = &cpu->tv1[expires & TV_MASK];
            } else if (delta < (1ULL << (2 * TV_BITS))) {
                bucket = &cpu->tv2[(expires >> TV_BITS) & TV_MASK];
            } else {
                bucket = &cpu->tv3[(expires >> (2 * TV_BITS)) & TV_MASK];
            }

            if (*bucket) (*bucket)->timer_prev = t;
            t->timer_next = *bucket;
            t->timer_bucket = bucket;
            *bucket = t;
        }

        void TimerCascade(cpu_t* cpu, thread_t** tv, int idx) {
            thread_t* t = tv[idx];
            tv[idx] = nullptr;
            
            while (t) {
                thread_t* next = t->timer_next;
                t->timer_bucket = nullptr;
                t->timer_next = nullptr;
                t->timer_prev = nullptr;
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
        if (now == cpu->timer_last_tick) return;
        
        uint64_t rflags;
        asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);

        while (cpu->timer_last_tick < now) {
            cpu->timer_last_tick++;
            uint64_t tick = cpu->timer_last_tick;
            
            int idx1 = tick & TV_MASK;
            thread_t* t = cpu->tv1[idx1];
            cpu->tv1[idx1] = nullptr;
            while (t) {
                thread_t* next = t->timer_next;
                t->timer_bucket = nullptr;
                t->state = THREAD_RUNNING;
                t = next;
            }

            if (idx1 == 0) {
                int idx2 = (tick >> TV_BITS) & TV_MASK;
                Schedule::Internal::TimerCascade(cpu, cpu->tv2, idx2);
                if (idx2 == 0) {
                    int idx3 = (tick >> (2 * TV_BITS)) & TV_MASK;
                    Schedule::Internal::TimerCascade(cpu, cpu->tv3, idx3);
                }
            }
        }

        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
    }
}