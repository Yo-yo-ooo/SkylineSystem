/*
* SPDX-License-Identifier: GPL-2.0-only
* File: smp.cpp
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
#include <arch/x86_64/allin.h>
#include <limine.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>

#pragma GCC push_options
#pragma GCC optimize ("O0")

cpu_t ZeroCPU = {0};
cpu_t *smp_cpu_list[MAX_CPU] = {&ZeroCPU};
volatile spinlock_t smp_lock = 0;
uint64_t started_count = 0;
bool smp_started = false;
int32_t smp_last_cpu = 0;
uint32_t smp_bsp_cpu;
extern art_tree *IRQHandlersTree;

void smp_setup_kstack(cpu_t *cpu) {
    void *stack = (void*)VMM::Alloc(kernel_pagemap, 8, true);
    TSS::SetIST(cpu->id, 0, (void*)((uint64_t)stack + 8 * PAGE_SIZE));
    TSS::SetRSP(cpu->id, 0, (void*)((uint64_t)stack + 8 * PAGE_SIZE));
}

void smp_setup_thread_queue(cpu_t *cpu) {
    _memset(cpu->thread_queues, 0, THREAD_QUEUE_CNT * sizeof(thread_queue_t));
    uint64_t quantum = 500;
    for (int32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
        cpu->thread_queues[i].quantum = quantum;
        quantum += 5;
    }
}

void smp_cpu_init(struct limine_mp_info *mp_info) {
    VMM::SwitchPageMap(kernel_pagemap);
    spinlock_lock(&smp_lock);
    GDT::Init(mp_info->lapic_id);
    idt_reinit();
    cpu_t *cpu = smp_cpu_list[mp_info->lapic_id];
    cpu->id = mp_info->lapic_id;
    LAPIC::Init();
    cpu->lapic_ticks = LAPIC::InitTimer();
    cpu->pagemap = kernel_pagemap;
    cpu->thread_count = 0;
    cpu->sched_lock = 0;
    cpu->has_runnable_thread = false;
    syscall_init();
    smp_setup_thread_queue(cpu);
    smp_setup_kstack(cpu);
    sse_enable();
    fpu_init();
    simd_cpu_init(cpu);
    kpok("Initialized CPU %d.\n", mp_info->lapic_id);
    started_count++;
    if (mp_info->lapic_id > smp_last_cpu) smp_last_cpu = mp_info->lapic_id;
    spinlock_unlock(&smp_lock);
    while (true)
        __asm__ volatile ("hlt");
}

extern volatile struct limine_mp_response *mp_response;
void smp_init() {
    
    cpu_t *bsp_cpu = (cpu_t*)kmalloc(sizeof(cpu_t));
    bsp_cpu->id = mp_response->bsp_lapic_id;
    bsp_cpu->pagemap = kernel_pagemap;
    bsp_cpu->lapic_ticks = LAPIC::InitTimer();
    bsp_cpu->thread_count = 0;
    bsp_cpu->sched_lock = 0;
    bsp_cpu->has_runnable_thread = false;
    smp_cpu_list[bsp_cpu->id] = bsp_cpu;
    smp_bsp_cpu = bsp_cpu->id;
    smp_setup_thread_queue(bsp_cpu);
    smp_setup_kstack(bsp_cpu);
    thread_t *init_thread = (thread_t*)kmalloc(sizeof(*init_thread));
    *init_thread = {};
    init_thread->state = THREAD_RUNNING;
    init_thread->id = -1;
    init_thread->cpu_num = bsp_cpu->id;

    init_thread->pagemap = kernel_pagemap;
    Schedule::Useless::AddThread(bsp_cpu, init_thread);
    bsp_cpu->current_thread = init_thread;
    //sse_enable();
    simd_cpu_init(bsp_cpu);
    kinfo("Detected %zu CPUs.\n", mp_response->cpu_count);
    for (uint64_t i = 0; i < mp_response->cpu_count; i++) {
        struct limine_mp_info *mp_info = mp_response->cpus[i];
        if (i == bsp_cpu->id)
            continue;
        smp_cpu_list[mp_info->lapic_id] = (cpu_t*)kmalloc(sizeof(cpu_t));
        mp_info->goto_address = smp_cpu_init;
        mp_info->extra_argument = (uint64_t)mp_info;
    }
    while (started_count < mp_response->cpu_count - 1)
        __asm__ volatile ("pause");
    smp_started = true;
    kpok("SMP Initialised.\n");
}

cpu_t *this_cpu() {
    if (!smp_started) return smp_cpu_list[mp_response->bsp_lapic_id];
    return smp_cpu_list[LAPIC::GetID()];
}

cpu_t *get_cpu(uint32_t id) {
    return smp_cpu_list[id];
}
#pragma GCC pop_options