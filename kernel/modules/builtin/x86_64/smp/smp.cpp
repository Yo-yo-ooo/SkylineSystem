//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <arch/x86_64/cpu/vf.h>
#include <klib/algorithm/rbtree.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/lapic/lapic.h>
#include <mem/pmm.h>
#include <klib/errno.h>
#include <arch/x86_64/dosvm/dosvm_contract.h>

extern "C" int32_t file_cache_writeback_callback(
    uint64_t file_id,
    const uint8_t *key, 
    uint32_t key_len, void *data, size_t data_len
);

cpu_t ZeroCPU = {0};
cpu_t *bsp_cpu = &ZeroCPU;
cpu_t *smp_cpu_list[MAX_CPU] = {&ZeroCPU};
spinlock_t smp_lock = 0;
volatile uint64_t started_count = 0; 
bool smp_started = false;
int32_t smp_last_cpu = 0; 
extern volatile struct limine_mp_response *mp_response;
extern void enable_smep_smap();
void sched_idle();   /* real idle loop: queue self-check + halt */

uint32_t apic_id_to_logical[MAX_CPU * 2] = {0};

/* ==========================================================================
 * Tier0 / dosvm: 单核可逆退出 IA-32e 的 "下线掩码"。
 *
 * 某个 AP 会真的清 CR0.PG 进入 real mode。它一旦处在非 x64 窗口, 任何按
 * 64 位 IDT 语义投递的 IPI (SCHED_VEC / TLB_FLUSH_VEC / ...) 都会让它跳到
 * 一个不存在的 64 位 IDT 门 -> triple fault -> 整机死。所以所有 IPI 发送
 * 路径必须先查这个掩码 (LAPIC::IPI 里做统一兜底, 调度/TLB 路径在源头跳)。
 *
 * 位图语义: 位 N = 逻辑 cpu id N 已下线。契约只导出一个 uint64_t
 * (g_cpu_offline_mask, 位 0..63), 而 MAX_CPU = 128, 故 id 64..127 由本文件
 * 私有的影子字承载, 避免 1ULL << >=64 的 UB 和位别名。四个 API 同时看
 * 两个字, 对调用方完全透明; 导出的符号仍精确反映低 64 个核的状态。
 *
 * 注意: dosvm_core_offline() 只负责"置位 + 全屏障", 不发 IPI、不自旋等
 * 目标核应答 (会与 Tier1 的 IPI handler 形成死锁)。真正进入安全窗口由
 * Tier1 在 0xF0 IPI handler 里自己 cli 完成; 0xF0 向量在本掩码面前放行。
 * ========================================================================== */
extern "C" {
volatile uint64_t g_cpu_offline_mask = 0;
}
/* 高 64 个逻辑 id 的影子字 (不导出, 只通过下面四个 API 访问)。 */
static volatile uint64_t s_cpu_offline_mask_hi = 0;

/* 原子内建函数不接受带 volatile 限定的指针类型做泛型实参, 统一在这里
   去掉限定; 之后所有访问都走 __atomic_* 内建, 编译器不会缓存旧值。 */
static inline uint64_t *dosvm_offline_word(uint32_t cpu_id) {
    return (cpu_id < 64u) ? (uint64_t *)&g_cpu_offline_mask
                          : (uint64_t *)&s_cpu_offline_mask_hi;
}
static inline uint64_t dosvm_offline_bit(uint32_t cpu_id) {
    return 1ULL << (cpu_id & 63u);
}

extern "C" int dosvm_core_offline(uint32_t cpu_id) {
    /* MAX_CPU=128: 越界拒绝; BSP(0) 承担 PIT/全局初始化, 不允许摘除。 */
    if (cpu_id >= (uint32_t)MAX_CPU || cpu_id == 0u) return -EINVAL;

    uint64_t *word = dosvm_offline_word(cpu_id);
    const uint64_t bit = dosvm_offline_bit(cpu_id);
    /* fetch_or 的返回值让"已置位"判定与置位本身成为一次原子操作:
       两个并发调用者只有一个能拿到 cleared 的旧值, 另一个直接 -EBUSY,
       且绝不误清别人持有的位。 */
    const uint64_t old = __atomic_fetch_or(word, bit, __ATOMIC_ACQ_REL);
    if (old & bit) return -EBUSY;

    /* 排空: 保证掩码对本核后续的所有 IPI 发送点可见 (也让编译器不把
       后面的发送越过这次置位)。不等待对方 —— 对方的安全窗口由 0xF0
       handler 自行建立。 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

extern "C" void dosvm_core_online(uint32_t cpu_id) {
    if (cpu_id >= (uint32_t)MAX_CPU || cpu_id == 0u) return;
    uint64_t *word = dosvm_offline_word(cpu_id);
    __atomic_fetch_and(word, ~dosvm_offline_bit(cpu_id), __ATOMIC_ACQ_REL);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

extern "C" int dosvm_core_is_offline(uint32_t cpu_id) {
    if (cpu_id >= (uint32_t)MAX_CPU) return 0;
    uint64_t *word = dosvm_offline_word(cpu_id);
    return (__atomic_load_n(word, __ATOMIC_ACQUIRE) &
            dosvm_offline_bit(cpu_id)) ? 1 : 0;
}

extern "C" int dosvm_lapic_offline(uint32_t lapic_id) {
    /* apic_id_to_logical[] 是 MAX_CPU*2 项, 0xFF = 无效映射;
       超界 (例如 x2APIC 的大 id) 一律当作"不离线", 宁可不拦也不能
       用越界下标读内存。 */
    if (lapic_id >= (uint32_t)(MAX_CPU * 2)) return 0;
    const uint32_t logical = apic_id_to_logical[lapic_id];
    if (logical == 0xFFu || logical >= (uint32_t)MAX_CPU) return 0;
    return dosvm_core_is_offline(logical);
}

void smp_setup_kstack(cpu_t *cpu) {
    void *rsps = (void*)VMM::Alloc(kernel_pagemap, 8, false);
    void *ists = (void*)VMM::Alloc(kernel_pagemap,8,false);
    TSS::SetIST(cpu->id, 1, (void*)((uint64_t)ists + 8 * PAGE_SIZE));
    TSS::SetRSP(cpu->id, 0, (void*)((uint64_t)rsps + 8 * PAGE_SIZE));
    /* #DF(8)/NMI(2)/#MC(18) 走 IST1：即使当前内核栈已损坏（#DF 的典型
       成因），异常仍能在独立栈上进入 C 处理器，而不是 triple fault。
       必须在 SetIST 之后设置 IDT 的 ist 位。 */
    idt_set_ist_cpu(cpu->id, 8, 1);   /* #DF */
    idt_set_ist_cpu(cpu->id, 2, 1);   /* NMI */
    idt_set_ist_cpu(cpu->id, 18, 1);  /* #MC */
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
    
    uint64_t logical_id = apic_id_to_logical[mp_info->lapic_id];
    
    GDT::Init(logical_id);
    idt_reinit(logical_id);
    
    

    spinlock_lock(&smp_lock);
    
    cpu_t *cpu = smp_cpu_list[logical_id];
    cpu->self = cpu;
    //_memset(&cpu->cslab, 0, sizeof(cpu_slab_t));
    for (int i = 0; i < MAX_SLAB_ORDER; i++) {
        cpu->cslab.freelist[i] = nullptr;
        cpu->cslab.count[i] = 0;
    }
    wrmsr(KERNEL_GS_BASE, (uint64_t)cpu);
    wrmsr(IA32_GS_MSR,(uint64_t)cpu);

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

    idt_install_irq_cpu(cpu->id, SCHED_VEC, (void*)Schedule::Internal::Switch);   /* 原硬编码 48 */
    //idt_install_irq_cpu(cpu->id, 49, (void*)Schedule::Internal::Switch);
    idt_set_ist_cpu(cpu->id, SCHED_VEC, 0);
    //idt_set_ist_cpu(cpu->id, SCHED_VEC + 1, 0);
    
    syscall_init();
    smp_setup_thread_queue(cpu);
    
    
    sse_enable();
    fpu_init();
    simd_cpu_init(cpu);
    cpu->simd_mask = cpu_simd_mask(cpu);
    
    enable_smep_smap();

    _memset(cpu->tv1, 0, sizeof(cpu->tv1)); 
    _memset(cpu->tv3, 0, sizeof(cpu->tv3));
    _memset(cpu->tv2, 0, sizeof(cpu->tv2));
    cpu->timer_last_tick = PIT::TimeSinceBootMS();

    cpu->file_cache = (file_cache_cpu_t*)kmalloc(sizeof(file_cache_cpu_t));
    file_cache_cpu_init_hook(cpu->file_cache, cpu->id, file_cache_writeback_callback);
    /* 每个 AP 的缓存也要设上限 (理由见 init.cpp 的同一处理) */
    {
        uint64_t free_bytes = PMM::FreePages() << 12;
        uint64_t per_cpu = free_bytes / 32;
        /* 每个 CPU 的文件缓存预算: 至少 512 KiB, 至多 4 MiB。
           实测把上限压到 1 MiB 反而更糟 (80 MiB 下 OOM 从 50 涨到 171) ——
           缓存太小 → 读盘次数暴增 → 每次读都要临时 kbuf, 瞬时压力更大。 */
        if (per_cpu < (512u << 10))  per_cpu = (512u << 10);
        if (per_cpu > (4u << 20))    per_cpu = (4u << 20);
        file_cache_set_limits_hook(cpu->file_cache, per_cpu, per_cpu * 2);
    }
    spinlock_lock(&smp_lock);
    kpok("Initialized CPU (Logical:%d, APIC:%d).\n", logical_id, mp_info->lapic_id);
    started_count++;
    if ((int64_t)logical_id > (int64_t)smp_last_cpu) 
        smp_last_cpu = logical_id; 
    spinlock_unlock(&smp_lock);
    
    /* Arm the FIRST periodic scheduler tick before idling. An AP that halts
       with its LAPIC timer left disabled (InitTimer ends with the timer
       disabled) has no fallback when the remote launch/wake IPI is missed: it
       would sleep until an unrelated IPI arrives (observed ~14s stalls). The
       tick guarantees the scheduler is re-entered every base slice. */
    LAPIC::Oneshot(SCHED_VEC, cpu->base_quantum * cpu->lapic_ticks);
    asm volatile("sti");
    /* Run the real idle loop on this boot stack; the first Switch snapshots
       this context as the idle thread context. Does not return. */
    sched_idle();
    __builtin_unreachable();
}

void smp_init() {
    for (int i = 0; i < MAX_CPU * 2; i++) apic_id_to_logical[i] = 0xFF;

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
        memset_fscpuf(new_cpu, 0, sizeof(cpu_t));
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
    if(unlikely(!smp_started))
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
    init_thread->fx_area = (char*)VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((smp_cpu_list[smp_bsp_cpu]->XsaveSize), PAGE_SIZE), false);
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