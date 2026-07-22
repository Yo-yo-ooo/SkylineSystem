//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#ifndef _SMP_H_
#define _SMP_H_

#include <klib/klib.h>

#define MAX_CPU 128
#define THREAD_QUEUE_CNT 16
#define MAX_PROMOTE_SNAPSHOT 256  

typedef struct thread_t thread_t;

#include <arch/x86_64/schedule/syscall.h>

#define PMM_PCP_MAX 256    
#define PMM_PCP_BATCH 64   

typedef struct cpu_overloadable_functions_t{
    void (*StoreSIMDState)(char* area,uint32_t Lo,uint32_t Hi);
    void (*LoadSIMDState)(char* area,uint32_t Lo,uint32_t Hi);
    void (*WRFSBASE)(uint64_t value);

    void *(*MemcpyCore)(void *dest, void *src, size_t numbytes);
    void *(*MemmoveCore)(void *dest, void *src, size_t numbytes);
    void *(*MemsetCore)(void *dest, const uint8_t val, size_t numbytes);
    void *(*MemcmpCore)(const void *str1, const void *str2, size_t numbytes, int equality);
} cpu_overloadable_functions_t;

#define TV_BITS 6
#define TV_SIZE (1 << TV_BITS)
#define TV_MASK (TV_SIZE - 1)

typedef struct sched_stats_t {
    uint64_t context_switches;
    uint64_t thread_steals;
    uint64_t steal_attempts;
    uint64_t zombie_reclaims;
    uint64_t try_pushes;
    uint64_t push_success;
} sched_stats_t;

typedef void (*interrupt_handler_t)(context_t*);

typedef struct {
    void* freelist[8]; 
    uint32_t count[8]; 
} cpu_slab_t;

typedef struct cpu_t {
    struct cpu_t* self;          
    uint64_t kernel_stack;       
    uint64_t user_scratch;       

    uint32_t id;
    uint64_t lapic_ticks;
    uint32_t lapic_id;
    pagemap_t *pagemap;
    
    rb_root_t runqueue_root;
    uint64_t min_vruntime;       
    uint64_t base_quantum;       
    
    thread_t *current_thread;
    
    volatile uint64_t thread_count;
    int32_t sched_lock;
    idt_desc_t idtdesc;
    interrupt_handler_t handlers[256];
    uint8_t IntrRegistCount = 0x20; 
    uint64_t IntrBitMap[4];         
    int8_t* KernelXsaveSpace;

    int32_t preempt_count = 0;
    bool InIntr = false;
    
    bool has_runnable_thread;
    bool SupportSIMD = false;
    bool SupportXSAVE = false;
    bool SupportAVX = false;
    bool SupportAVX2 = false;
    bool SupportAVX512 = false;
    bool SupportSSE4_2 = false;
    bool SupportXSAVEOPT = false;

    uint64_t XsaveSize;
    uint32_t XsaveMaskLo;
    uint32_t XsaveMaskHi;

    void* pmm_cache[PMM_PCP_MAX];
    uint32_t pmm_cache_count;

    cpu_overloadable_functions_t OverLoadableFuncs;

    thread_t* tv1[TV_SIZE]; 
    thread_t* tv2[TV_SIZE]; 
    thread_t* tv3[TV_SIZE]; 
    uint64_t timer_last_tick; 

    alignas(16) uint8_t exit_stack[4096];
    
    uint64_t tick_count = 0;             
    uint64_t total_thread_count = 0;     
    
    volatile uint32_t thread_count_lower = 0; 
    volatile bool has_surplus = false; 

    thread_t* promote_buf[MAX_PROMOTE_SNAPSHOT];
    uint32_t simd_mask;
    sched_stats_t sched_stats;
    thread_t *idle_thread;
    cpu_slab_t cslab;
    
    uint64_t zombie_count;
    thread_t *zombie_list;
    uint64_t total_weight;
    uint64_t avg_vruntime;
    uint64_t avg_vruntime_rem;
} cpu_t;

constexpr uint64_t SIZEOF_CPU_T = sizeof(cpu_t);
constexpr uint64_t OFF_CPU_ININTR = offsetof(cpu_t,InIntr);

extern uint32_t smp_bsp_cpu;
extern int32_t smp_last_cpu;
extern cpu_t *smp_cpu_list[MAX_CPU];
extern bool smp_started;

void smp_init();
extern "C" cpu_t *this_cpu();
cpu_t *get_cpu(uint32_t id);
void InitCPUThread();

static inline uint32_t cpu_simd_mask(const cpu_t *cpu) {
    uint32_t m = 0;
    if (cpu->SupportSIMD)     m |= 0x01;
    if (cpu->SupportXSAVE)    m |= 0x02;
    if (cpu->SupportSSE4_2)   m |= 0x04;
    if (cpu->SupportAVX)      m |= 0x08;
    if (cpu->SupportAVX2)     m |= 0x10;
    if (cpu->SupportAVX512)   m |= 0x20;
    if (cpu->SupportXSAVEOPT) m |= 0x40;
    return m;
}

static inline bool simd_compatible(const cpu_t *a, const cpu_t *b) {
    return cpu_simd_mask(a) == cpu_simd_mask(b);
}

#endif