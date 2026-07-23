//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/allin.h>
#include <limine.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <arch/x86_64/cpu/vf.h>
#include <klib/algorithm/rbtree.h>


cpu_t ZeroCPU = {0};
cpu_t *bsp_cpu = &ZeroCPU;
cpu_t *smp_cpu_list[MAX_CPU] = {&ZeroCPU};
volatile spinlock_t smp_lock = 0;
volatile uint64_t started_count = 0; 
bool smp_started = false;
int32_t smp_last_cpu = 0; 
extern volatile struct limine_mp_response *mp_response;

uint8_t apic_id_to_logical[256] = {0};

void smp_setup_kstack(cpu_t *cpu) {
    void *rsps = (void*)VMM::Alloc(kernel_pagemap, 8, true);
    void *ists = (void*)VMM::Alloc(kernel_pagemap,8,true);
    TSS::SetIST(cpu->id, 1, (void*)((uint64_t)ists + 8 * PAGE_SIZE));
    TSS::SetRSP(cpu->id, 0, (void*)((uint64_t)rsps + 8 * PAGE_SIZE));
}

void smp_setup_thread_queue(cpu_t *cpu) {
    rb_root_init(&cpu->runqueue_root, nullptr, nullptr, nullptr, nullptr, nullptr);
    cpu->min_vruntime = 0;
    cpu->base_quantum = 5; // 基础时间片 5ms
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
    cr4 |= (1 << 16);
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
    cpu->OverLoadableFuncs.WRFSBASE = WRFSBASE_V1;
}

void smp_cpu_init(struct limine_mp_info *mp_info) {
    VMM::SwitchPageMap(kernel_pagemap);
    
    uint8_t logical_id = apic_id_to_logical[mp_info->lapic_id];
    
    GDT::Init(logical_id);
    idt_reinit(logical_id);
    
    cpu_t *cpu = smp_cpu_list[logical_id];
    cpu->self = cpu;
    wrmsr(KERNEL_GS_BASE, (uint64_t)cpu);
    wrmsr(IA32_GS_MSR,(uint64_t)cpu);

    spinlock_lock(&smp_lock);
    
    //asm volatile("swapgs" ::: "memory");
    cpu->id = logical_id;          
    cpu->lapic_id = mp_info->lapic_id; 
    spinlock_unlock(&smp_lock);
    
    LAPIC::Init();
    cpu->lapic_ticks = LAPIC::InitTimer();
    cpu->pagemap = kernel_pagemap;
    cpu->thread_count = 0;
    cpu->sched_lock = 0;
    cpu->has_runnable_thread = false;
    
    EnableFSGSBASE(cpu);
    
    smp_setup_kstack(cpu);

    idt_install_irq_cpu(cpu->id, 48, (void*)Schedule::Internal::Preempt);
    idt_install_irq_cpu(cpu->id, 49, (void*)Schedule::Internal::Switch);
    idt_set_ist_cpu(cpu->id, SCHED_VEC, 0);
    idt_set_ist_cpu(cpu->id, SCHED_VEC + 1, 0);
    
    syscall_init();
    smp_setup_thread_queue(cpu);
    
    
    sse_enable();
    fpu_init();
    simd_cpu_init(cpu);
    cpu->simd_mask = cpu_simd_mask(cpu);
    
    

    _memset(cpu->tv1, 0, sizeof(cpu->tv1)); 
    _memset(cpu->tv3, 0, sizeof(cpu->tv3));
    _memset(cpu->tv2, 0, sizeof(cpu->tv2));
    cpu->timer_last_tick = PIT::TimeSinceBootMS();

    spinlock_lock(&smp_lock);
    kpok("Initialized CPU (Logical:%d, APIC:%d).\n", logical_id, mp_info->lapic_id);
    started_count++;
    if (logical_id > smp_last_cpu) 
        smp_last_cpu = logical_id; 
    spinlock_unlock(&smp_lock);
    
    asm volatile("sti");
    while (true)
        __asm__ volatile ("hlt");
}

void smp_init() {
    for (int i = 0; i < 256; i++) apic_id_to_logical[i] = 0xFF;

    //bsp_cpu = (cpu_t*)kmalloc(sizeof(cpu_t)); 
    bsp_cpu->lapic_id = mp_response->bsp_lapic_id; 
    bsp_cpu->id = 0;                               
    apic_id_to_logical[bsp_cpu->lapic_id] = 0;     
    
    bsp_cpu->pagemap = kernel_pagemap;
    bsp_cpu->lapic_ticks = LAPIC::InitTimer();
    bsp_cpu->thread_count = 0;
    bsp_cpu->sched_lock = 0;
    bsp_cpu->has_runnable_thread = false;
    smp_cpu_list[0] = bsp_cpu;                         
    smp_bsp_cpu = 0;                               
    
    smp_setup_thread_queue(bsp_cpu);
    smp_setup_kstack(bsp_cpu);
    EnableFSGSBASE(bsp_cpu);
    
    
    simd_cpu_init(bsp_cpu);
    kinfo("Detected %zu CPUs.\n", mp_response->cpu_count);
    
    uint32_t next_logical_id = 1; 
    for (uint64_t i = 0; i < mp_response->cpu_count; i++) {
        struct limine_mp_info *mp_info = mp_response->cpus[i];
        if (mp_info->lapic_id == bsp_cpu->lapic_id)
            continue;
        
        cpu_t *new_cpu = (cpu_t*)kmalloc(sizeof(cpu_t));
        new_cpu->lapic_id = mp_info->lapic_id;      
        new_cpu->id = next_logical_id;              
        
        apic_id_to_logical[mp_info->lapic_id] = next_logical_id; 
        smp_cpu_list[next_logical_id] = new_cpu;    
        next_logical_id++;
        
        mp_info->goto_address = smp_cpu_init;
        mp_info->extra_argument = (uint64_t)mp_info;
    }
    
    while (started_count < mp_response->cpu_count - 1)
        __asm__ volatile ("pause");
        
    smp_started = true;
    kpok("SMP Initialised.\n");
}

extern "C" cpu_t *this_cpu() {
    if(!smp_started)
        return bsp_cpu;
    cpu_t *cpu;
    asm volatile("movq %%gs:0, %0" : "=r"(cpu) : : "memory");
    return cpu;
}

cpu_t *get_cpu(uint32_t id) {
    if (id >= MAX_CPU) return NULL; 
    return smp_cpu_list[id];
}

void InitCPUThread(){
    // 创建一个内核进程作为 init_thread 的父进程
    proc_t *init_proc = Schedule::NewProcess(false);
    
    thread_t *init_thread = (thread_t*)kmalloc(sizeof(*init_thread));
    *init_thread = {};
    
    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    init_thread->kernel_rsp = current_rsp;
    init_thread->ctx.rsp = current_rsp;
    
    // 初始化上下文，防止被提前调度时 RIP=0 导致崩溃
    init_thread->ctx.rip = (uint64_t)&&init_thread_ready;
    init_thread->ctx.cs = 0x08;
    init_thread->ctx.ss = 0x10;
    init_thread->ctx.rflags = 0x202;
    
init_thread_ready:
    init_thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((smp_cpu_list[smp_bsp_cpu]->XsaveSize), PAGE_SIZE), true);
    _memset(init_thread->fx_area, 0, smp_cpu_list[smp_bsp_cpu]->XsaveSize);
    
    uint32_t eax = smp_cpu_list[smp_bsp_cpu]->XsaveMaskLo;
    uint32_t edx = smp_cpu_list[smp_bsp_cpu]->XsaveMaskHi;
    smp_cpu_list[smp_bsp_cpu]->OverLoadableFuncs.StoreSIMDState(init_thread->fx_area, eax, edx);
    
    init_thread->state = THREAD_RUNNING;
    init_thread->id = -1;
    init_thread->cpu_num = smp_bsp_cpu; 
    init_thread->parent = init_proc;

    _memset(smp_cpu_list[smp_bsp_cpu]->tv1, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv1));
    _memset(smp_cpu_list[smp_bsp_cpu]->tv3, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv3));
    _memset(smp_cpu_list[smp_bsp_cpu]->tv2, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv2));
    smp_cpu_list[smp_bsp_cpu]->timer_last_tick = PIT::TimeSinceBootMS();

    init_thread->pagemap = kernel_pagemap;
    
    init_thread->vruntime = smp_cpu_list[smp_bsp_cpu]->min_vruntime;
    init_thread->weight = 1024; // 默认权重
    
    // 【关键修复】：先禁用中断，设置 current_thread，再插入队列！
    // 防止插入队列后立刻被中断，导致调度器挑出未完全初始化的 init_thread 运行
    asm volatile("cli");
    smp_cpu_list[smp_bsp_cpu]->current_thread = init_thread;
    Schedule::Internal::InsertToQueue(smp_cpu_list[smp_bsp_cpu], init_thread);
    asm volatile("sti");
}