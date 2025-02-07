#pragma once

#ifndef _SMP_H_
#define _SMP_H_

#include <klib/klib.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/cpu.h>

extern u32 bsp_lapic_id;
extern u64 smp_cpu_count;

typedef struct cpu_info{
    u64 lapic_id;
    pagemap* pm;

    list* proc_list;

    struct process* proc;
    struct thread* thread;

    u64 proc_idx;

    struct process* idle_proc;

    atomic_lock sched_lock;

    bool scheduled_threads;
    bool scheduled_children;
} cpu_info;

cpu_info* this_cpu();
cpu_info* get_cpu(u64 lapic_id);

void smp_init();

#endif