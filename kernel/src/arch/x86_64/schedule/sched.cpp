#include "sched.h"
#include "../lapic/lapic.h"
#include "../../../klib/klib.h"
#include "../smp/smp.h"
#include "../../../klib/kio.h"
#include "../cpu.h"

namespace Schedule{

    process* GetNextProc(cpu_info* c) {
        lock(&c->sched_lock);
        struct process* proc;
        while (true) {
            proc = (struct process*)List::Iterate(c->proc_list, true);
            if (proc->threads->count > 0)
                break;
        }
        unlock(&c->sched_lock);
        return proc;
    }

    thread* GetNextThread(process* proc) {
        lock(&proc->lock);
        struct thread* t;
        while (true) {
            t = (struct thread*)List::Iterate(proc->threads, false);
            if (t == NULL) {
                get_cpu(proc->cpu)->scheduled_threads = true;
                break;
            }
            if (t->state == SCHED_RUNNING) {
            break;
            }
        }
        unlock(&proc->lock);
        return t;
    }


    void Schedule(registers* r){
        LAPIC::StopTimer();

        cpu_info* c = this_cpu();

        if (c->thread) {
            thread* t = c->thread;
            t->ctx = *r;
            t->pm = c->pm;
            t->gs = read_kernel_gs();
            __asm__ volatile ("fxsave %0" : : "m"(t->fxsave));
        } else {
            c->proc = GetNextProc(c);
        }

        // Find a suitable thread
        thread* next_thread = GetNextThread(c->proc);
        while (c->scheduled_threads) {
            c->proc = GetNextProc(c);
            next_thread = GetNextThread(c->proc);
            c->scheduled_threads = false;
        }

        c->thread = next_thread;

        *r = c->thread->ctx;
        VMM::SwitchPM(c->thread->pm);
        write_kernel_gs((u64)c->thread);

        __asm__ volatile ("fxrstor %0" : : "m"(c->thread->fxsave));

        LAPIC::EOI();
        LAPIC::Oneshot(0x80, 5);
    }
}