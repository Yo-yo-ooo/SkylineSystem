#pragma once

#include "../../../klib/klib.h"

typedef struct {
    u64 lapic_id;
    pagemap* pm;

    list* proc_list;

    process* proc;
    thread* thread;

    u64 proc_idx;

    process* idle_proc;

    atomic_lock sched_lock;

    bool scheduled_threads;
    bool scheduled_children;
} cpu_info;