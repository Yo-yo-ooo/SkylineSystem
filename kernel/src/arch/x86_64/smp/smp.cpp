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
int32_t smp_last_cpu = 0; // 现在这里代表最大的“逻辑” CPU ID
extern volatile struct limine_mp_response *mp_response;

// 新增：物理 APIC ID 到逻辑 CPU ID 的映射表
// 假设系统最多支持 256 个 APIC ID
uint8_t apic_id_to_logical[256] = {0};

void smp_setup_kstack(cpu_t *cpu) {
    void *stack = (void*)VMM::Alloc(kernel_pagemap, 8, true);
    // 使用逻辑 ID (cpu->id) 来设置 TSS
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
    cr4 |= (1 << 16);
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
    cpu->OverLoadableFuncs.WRFSBASE = WRFSBASE_V1;
}

void smp_cpu_init(struct limine_mp_info *mp_info) {
    VMM::SwitchPageMap(kernel_pagemap);
    
    // 获取当前 CPU 的逻辑 ID
    uint8_t logical_id = apic_id_to_logical[mp_info->lapic_id];
    
    // GDT/IDT 等基于数组的结构体应使用逻辑 ID 作为索引
    GDT::Init(logical_id);
    idt_reinit(logical_id);
    
    spinlock_lock(&smp_lock);
    cpu_t *cpu = smp_cpu_list[logical_id];
    cpu->id = logical_id;          // 逻辑 ID
    cpu->lapic_id = mp_info->lapic_id; // 物理 APIC ID
    spinlock_unlock(&smp_lock);
    
    LAPIC::Init();
    cpu->lapic_ticks = LAPIC::InitTimer();
    cpu->pagemap = kernel_pagemap;
    cpu->thread_count = 0;
    cpu->sched_lock = 0;
    cpu->has_runnable_thread = false;
    EnableFSGSBASE(cpu);
    
    // 使用逻辑 ID 安装 IDT
    idt_install_irq_cpu(cpu->id, 48, (void*)Schedule::Internal::Preempt);
    idt_install_irq_cpu(cpu->id, 49, (void*)Schedule::Internal::Switch);
    idt_set_ist_cpu(cpu->id, SCHED_VEC, 0);
    idt_set_ist_cpu(cpu->id, SCHED_VEC + 1, 0);
    
    syscall_init();
    smp_setup_thread_queue(cpu);
    smp_setup_kstack(cpu);
    
    sse_enable();
    fpu_init();
    simd_cpu_init(cpu);
    cpu->simd_mask = cpu_simd_mask(cpu);
    
    wrmsr(KERNEL_GS_BASE, (uint64_t)cpu);
    asm volatile("swapgs" ::: "memory");

    _memset(cpu->tv1, 0, sizeof(cpu->tv1)); 
    _memset(cpu->tv3, 0, sizeof(cpu->tv3));
    _memset(cpu->tv2, 0, sizeof(cpu->tv2));
    cpu->timer_last_tick = PIT::TimeSinceBootMS();

    spinlock_lock(&smp_lock);
    kpok("Initialized CPU (Logical:%d, APIC:%d).\n", logical_id, mp_info->lapic_id);
    started_count++;
    if (logical_id > smp_last_cpu) 
        smp_last_cpu = logical_id; // 记录最大的逻辑 ID
    spinlock_unlock(&smp_lock);
    
    asm volatile("sti");
    while (true)
        __asm__ volatile ("hlt");
}

void smp_init() {
    // 初始化映射表为无效 (0xFF)
    for (int i = 0; i < 256; i++) apic_id_to_logical[i] = 0xFF;

    bsp_cpu = (cpu_t*)kmalloc(sizeof(cpu_t)); 
    bsp_cpu->lapic_id = mp_response->bsp_lapic_id; // 保存物理 APIC ID
    bsp_cpu->id = 0;                               // BSP 逻辑 ID 固定为 0
    apic_id_to_logical[bsp_cpu->lapic_id] = 0;     // 建立映射
    
    bsp_cpu->pagemap = kernel_pagemap;
    bsp_cpu->lapic_ticks = LAPIC::InitTimer();
    bsp_cpu->thread_count = 0;
    bsp_cpu->sched_lock = 0;
    bsp_cpu->has_runnable_thread = false;
    smp_cpu_list[0] = bsp_cpu;                     // 用逻辑 ID 存入数组
    smp_bsp_cpu = 0;                               // 记录 BSP 的逻辑 ID
    
    smp_setup_thread_queue(bsp_cpu);
    smp_setup_kstack(bsp_cpu);
    EnableFSGSBASE(bsp_cpu);
    wrmsr(KERNEL_GS_BASE, (uint64_t)bsp_cpu);
    asm volatile("swapgs" ::: "memory");
    
    simd_cpu_init(bsp_cpu);
    kinfo("Detected %zu CPUs.\n", mp_response->cpu_count);
    
    uint32_t next_logical_id = 1; // AP 的逻辑 ID 从 1 开始
    for (uint64_t i = 0; i < mp_response->cpu_count; i++) {
        struct limine_mp_info *mp_info = mp_response->cpus[i];
        // 使用物理 APIC ID 进行比较
        if (mp_info->lapic_id == bsp_cpu->lapic_id)
            continue;
        
        cpu_t *new_cpu = (cpu_t*)kmalloc(sizeof(cpu_t));
        new_cpu->lapic_id = mp_info->lapic_id;      // 保存物理 APIC ID
        new_cpu->id = next_logical_id;              // 分配逻辑 ID
        
        apic_id_to_logical[mp_info->lapic_id] = next_logical_id; // 建立映射
        smp_cpu_list[next_logical_id] = new_cpu;    // 用逻辑 ID 存入数组
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
    if (!smp_started) return smp_cpu_list[0]; // smp_bsp_cpu (逻辑0)
    
    uint32_t apic_id = LAPIC::GetID();
    if (apic_id < 256 && apic_id_to_logical[apic_id] != 0xFF) {
        return smp_cpu_list[apic_id_to_logical[apic_id]];
    }
    return &ZeroCPU; // 异常 fallback
}

cpu_t *get_cpu(uint32_t id) {
    // 这里的 id 现在期望传入的是逻辑 ID
    if (id >= MAX_CPU) return NULL; 
    return smp_cpu_list[id];
}

void InitCPUThread(){
    
    thread_t *init_thread = (thread_t*)kmalloc(sizeof(*init_thread));
    *init_thread = {};
    init_thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((smp_cpu_list[smp_bsp_cpu]->XsaveSize), PAGE_SIZE), true);
    _memset(init_thread->fx_area, 0, smp_cpu_list[smp_bsp_cpu]->XsaveSize);
    
    uint32_t eax = smp_cpu_list[smp_bsp_cpu]->XsaveMaskLo;
    uint32_t edx = smp_cpu_list[smp_bsp_cpu]->XsaveMaskHi;
    smp_cpu_list[smp_bsp_cpu]->OverLoadableFuncs.StoreSIMDState(init_thread->fx_area, eax, edx);
    
    init_thread->state = THREAD_RUNNING;
    init_thread->id = -1;
    init_thread->cpu_num = smp_bsp_cpu; // 绑定逻辑 ID

    _memset(smp_cpu_list[smp_bsp_cpu]->tv1, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv1));
    _memset(smp_cpu_list[smp_bsp_cpu]->tv3, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv3));
    _memset(smp_cpu_list[smp_bsp_cpu]->tv2, 0, sizeof(smp_cpu_list[smp_bsp_cpu]->tv2));
    smp_cpu_list[smp_bsp_cpu]->timer_last_tick = PIT::TimeSinceBootMS();

    init_thread->pagemap = kernel_pagemap;
    Schedule::Internal::InsertToQueue(smp_cpu_list[smp_bsp_cpu], init_thread);
    smp_cpu_list[smp_bsp_cpu]->current_thread = init_thread;
}