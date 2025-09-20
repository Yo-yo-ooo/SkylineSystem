#pragma once

#ifndef _SMP_H_
#define _SMP_H_

#include <klib/klib.h>
//#include <arch/x86_64/schedule/sched.h>

//#include <arch/x86_64/cpu.h>

#define MAX_CPU 128
#define THREAD_QUEUE_CNT 16

typedef struct thread_t thread_t;

typedef struct thread_queue_t{
    thread_t *head;
    thread_t *current;
    uint64_t count;
    uint64_t quantum;
} thread_queue_t;

typedef struct cpu_t{
    uint32_t id;
    uint64_t lapic_ticks;
    pagemap_t *pagemap;
    thread_queue_t thread_queues[THREAD_QUEUE_CNT];
    thread_t *current_thread;
    uint64_t thread_count;
    int32_t sched_lock;
    bool has_runnable_thread;
} cpu_t;

extern uint32_t smp_bsp_cpu;

extern int32_t smp_last_cpu;
extern cpu_t *smp_cpu_list[MAX_CPU];
extern bool smp_started;

void smp_init();
cpu_t *this_cpu();
cpu_t *get_cpu(uint32_t id);

#endif