//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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

#include <arch/x86_64/schedule/syscall.h>

#define PMM_PCP_MAX 256    // 每个 CPU 最多缓存 256 页 (1MB)
#define PMM_PCP_BATCH 64   // 每次去全局位图“进货”或“退货”的数量

typedef struct cpu_overloadable_functions_t{
    void (*StoreSIMDState)(char* area,uint32_t Lo,uint32_t Hi);
    void (*LoadSIMDState)(char* area,uint32_t Lo,uint32_t Hi);
    void (*WRFSBASE)(uint64_t value);

    void *(*MemcpyCore)(void *dest, void *src, size_t numbytes);
    void *(*MemmoveCore)(void *dest, void *src, size_t numbytes);
    void *(*MemsetCore)(void *dest, const uint8_t val, size_t numbytes);
    void *(*MemcmpCore)(const void *str1, const void *str2, size_t numbytes, int equality);
    
}cpu_overloadable_functions_t;

// 时间轮参数 (TV_SIZE = 64，位运算极速)
#define TV_BITS 6
#define TV_SIZE (1 << TV_BITS)
#define TV_MASK (TV_SIZE - 1)

typedef void (*interrupt_handler_t)(context_t*);
typedef struct cpu_t{
    struct cpu_t* self;          // 偏移 0：指向自身，this_cpu() 快速获取
    uint64_t kernel_stack;       // 偏移 8：当前 CPU 内核栈顶（syscall 入口切栈用）
    uint64_t user_scratch;       // 偏移 16：临时保存用户态 RSP

    uint32_t id;
    uint64_t lapic_ticks;
    uint32_t lapic_id;
    pagemap_t *pagemap;
    thread_queue_t thread_queues[THREAD_QUEUE_CNT];
    thread_t *current_thread;
    uint64_t thread_count;
    int32_t sched_lock;
    idt_desc_t idtdesc;
    interrupt_handler_t handlers[256];
    uint8_t IntrRegistCount = 0x20; //Base:0x20(CPU RSVD 0~0x20)
    uint64_t IntrBitMap[4]; // 256 Count Bitmap      
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

    thread_t* tv1[TV_SIZE]; // 第一级：0 ~ 63 ms
    thread_t* tv2[TV_SIZE]; // 第二级：64 ~ 4095 ms
    thread_t* tv3[TV_SIZE]; // 第三级：4096 ~ 262143 ms (~4.3 分钟)
    uint64_t timer_last_tick; // 记录上一次处理到的时间戳

    alignas(16) uint8_t exit_stack[4096];
} cpu_t;
constexpr uint64_t SIZEOF_CPU_T = sizeof(cpu_t);

extern uint32_t smp_bsp_cpu;

extern int32_t smp_last_cpu;
extern cpu_t *smp_cpu_list[MAX_CPU];
extern bool smp_started;

void smp_init();
extern "C" cpu_t *this_cpu();
cpu_t *get_cpu(uint32_t id);
void InitBSPCPUThread();

#endif