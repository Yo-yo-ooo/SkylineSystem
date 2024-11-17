#pragma once

#include "../../../klib/klib.h"
#include "../schedule/sched.h"
#include "../../../mem/vmm.h"

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