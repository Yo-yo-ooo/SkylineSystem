/*
* SPDX-License-Identifier: GPL-2.0-only
* File: smp.h
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
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
}cpu_overloadable_functions_t;


typedef struct cpu_t{
    uint64_t kernel_stack;    // 偏移 0: 当前 CPU 的内核栈顶 (用于 syscall 入口)
    uint64_t user_scratch;    // 偏移 8: 临时存放用户态 RSP (用于 syscall 入口)

    uint32_t id;
    uint64_t lapic_ticks;
    pagemap_t *pagemap;
    thread_queue_t thread_queues[THREAD_QUEUE_CNT];
    thread_t *current_thread;
    uint64_t thread_count;
    int32_t sched_lock;
    idt_desc_t idtdesc;
    uint64_t *handlers;
    uint8_t IntrRegistCount = 0x20; //Base:0x20(CPU RSVD 0~0x20)
    uint64_t IntrBitMap[4]; // 256 Count Bitmap      
    int8_t* KernelXsaveSpace;

    int32_t preempt_count = 0;
    //bool InIntr = false;
    
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
} cpu_t;

//constexpr uint64_t ININTR_OFF = offsetof(cpu_t, InIntr);

extern uint32_t smp_bsp_cpu;

extern int32_t smp_last_cpu;
extern cpu_t *smp_cpu_list[MAX_CPU];
extern bool smp_started;

void smp_init();
extern "C" cpu_t *this_cpu();
cpu_t *get_cpu(uint32_t id);
void InitBSPCPUThread();

#endif