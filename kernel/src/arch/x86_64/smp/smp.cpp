//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/allin.h>
#include <limine.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <arch/x86_64/cpu/vf.h>

cpu_t ZeroCPU = {0};
cpu_t *bsp_cpu = &ZeroCPU;
cpu_t *smp_cpu_list[MAX_CPU] = {&ZeroCPU};
volatile spinlock_t smp_lock = 0;
volatile uint64_t started_count = 0; 
bool smp_started = false;
int32_t smp_last_cpu = 0;
extern volatile struct limine_mp_response *mp_response;

void smp_setup_kstack(cpu_t *cpu) {
    void *stack = (void*)VMM::Alloc(kernel_pagemap, 8, true);

    TSS::SetIST(cpu->id, 0, (void*)((uint64_t)stack + 8 * PAGE_SIZE));
    TSS::SetRSP(cpu->id, 0, (void*)((uint64_t)stack + 8 * PAGE_SIZE));
}

void smp_setup_thread_queue(cpu_t *cpu) {
    _memset(cpu->thread_queues, 0, THREAD_QUEUE_CNT * sizeof(thread_queue_t));
    for (int32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
        uint64_t ms = 10 + (i * 5);
        cpu->thread_queues[i].quantum = ms * cpu->lapic_ticks;
    }
}

bool CheckFSGSBASE() {
    uint32_t eax, ebx, ecx, edx;
    asm volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0)
    );
    return (ebx & (1 << 5));
}

void EnableFSGSBASE(cpu_t *cpu) {
    if (!CheckFSGSBASE()){
        cpu->OverLoadableFuncs.WRFSBASE = WRFSBASE_V0;
        return;
    }

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 16); // Set the 16th bit to enable FSGSBASE instructions
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
    cpu->OverLoadableFuncs.WRFSBASE = WRFSBASE_V1;
}

void smp_cpu_init(struct limine_mp_info *mp_info) {
    VMM::SwitchPageMap(kernel_pagemap);
    GDT::Init(mp_info->lapic_id);
    idt_reinit(mp_info->lapic_id);
    
    spinlock_lock(&smp_lock);
    cpu_t *cpu = smp_cpu_list[mp_info->lapic_id];
    cpu->id = mp_info->lapic_id;
    spinlock_unlock(&smp_lock);
    
    LAPIC::Init();
    cpu->lapic_ticks = LAPIC::InitTimer();
    cpu->pagemap = kernel_pagemap;
    cpu->thread_count = 0;
    cpu->sched_lock = 0;
    cpu->has_runnable_thread = false;
    EnableFSGSBASE(cpu);
    idt_install_irq_cpu(cpu->id, 48, (void*)Schedule::Internal::Preempt);
    idt_install_irq_cpu(cpu->id, 49, (void*)Schedule::Internal::Switch);
    
    idt_set_ist_cpu(cpu->id, SCHED_VEC, 1);
    idt_set_ist_cpu(cpu->id, SCHED_VEC + 1, 1);
    syscall_init();
    smp_setup_thread_queue(cpu);
    smp_setup_kstack(cpu);
    
    sse_enable();
    fpu_init();
    simd_cpu_init(cpu);
    
    wrmsr(KERNEL_GS_BASE, (uint64_t)cpu);
    asm volatile("swapgs" ::: "memory");

    // 修复：去掉 sizeof，直接使用空间大小，或者 sizeof(cpu->tv1)
    _memset(cpu->tv1, 0, sizeof(cpu->tv1)); 
    _memset(cpu->tv3, 0, sizeof(cpu->tv3));
    _memset(cpu->tv2, 0, sizeof(cpu->tv2));
    cpu->timer_last_tick = PIT::TimeSinceBootMS();

    spinlock_lock(&smp_lock);
    kpok("Initialized CPU %d.\n", mp_info->lapic_id);
    started_count++;
    if (mp_info->lapic_id > smp_last_cpu) 
        smp_last_cpu = mp_info->lapic_id;
    spinlock_unlock(&smp_lock);
    
    asm volatile("sti");
    while (true)
        __asm__ volatile ("hlt");
}

void smp_init() {
    // 修复：不要重新声明 bsp_cpu，直接给全局变量赋值
    bsp_cpu = (cpu_t*)kmalloc(sizeof(cpu_t)); 
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
    EnableFSGSBASE(bsp_cpu);
    wrmsr(KERNEL_GS_BASE, (uint64_t)bsp_cpu);
    asm volatile("swapgs" ::: "memory");
    
    simd_cpu_init(bsp_cpu);
    kinfo("Detected %zu CPUs.\n", mp_response->cpu_count);
    
    for (uint64_t i = 0; i < mp_response->cpu_count; i++) {
        struct limine_mp_info *mp_info = mp_response->cpus[i];
        if (mp_info->lapic_id == bsp_cpu->id)
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

extern "C" cpu_t *this_cpu() {
    if (!smp_started) return smp_cpu_list[mp_response->bsp_lapic_id];
    return smp_cpu_list[LAPIC::GetID()];
    
}

cpu_t *get_cpu(uint32_t id) {
    if (id >= MAX_CPU) return NULL; 
    return smp_cpu_list[id];
}

void InitBSPCPUThread(){
    thread_t *init_thread = (thread_t*)kmalloc(sizeof(*init_thread));
    *init_thread = {};
    init_thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((smp_cpu_list[smp_bsp_cpu]->XsaveSize), PAGE_SIZE), true);
    _memset(init_thread->fx_area, 0, smp_cpu_list[smp_bsp_cpu]->XsaveSize);
    
    uint32_t eax = smp_cpu_list[smp_bsp_cpu]->XsaveMaskLo;
    uint32_t edx = smp_cpu_list[smp_bsp_cpu]->XsaveMaskHi;
    smp_cpu_list[smp_bsp_cpu]->OverLoadableFuncs.StoreSIMDState(init_thread->fx_area, eax, edx);
    
    init_thread->state = THREAD_RUNNING;
    init_thread->id = -1;
    init_thread->cpu_num = smp_bsp_cpu;

    _memset(smp_cpu_list[smp_bsp_cpu]->tv1, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv1));
    _memset(smp_cpu_list[smp_bsp_cpu]->tv3, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv3));
    _memset(smp_cpu_list[smp_bsp_cpu]->tv2, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv2));
    smp_cpu_list[smp_bsp_cpu]->timer_last_tick = PIT::TimeSinceBootMS();

    init_thread->pagemap = kernel_pagemap;
    Schedule::Internal::AddThread(smp_cpu_list[smp_bsp_cpu], init_thread);
    smp_cpu_list[smp_bsp_cpu]->current_thread = init_thread;
}