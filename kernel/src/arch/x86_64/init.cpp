//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/nvme/nvme.h>
#include <drivers/usb/xhci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>
#include <fs/fc.h>
#include <arch/x86_64/simd/simd.h>
#include <mem/heap.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <drivers/framebuffer/fb.h>
#include <klib/renderer/rnd.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/interrupt/idt.h>
#include <mem/pmm.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/sched_bench.h>
#include <arch/x86_64/pit/pit.h>
#include <drivers/net/e1000.h>
#include <kmod/kmod.h>
#include <kmod/kmod_seats.h>

/* 原 init.cpp 尾部常见的 Tier0 依赖（串口/GDT/IDT/PMM/VMM/SLAB/SMP/LAPIC/
   PIT/RTC 等基础头）保持原样，避免组件化重构改变既有编译单元语义。 */
#include <atomic/atomic.h>
#include <arch/x86_64/cpu.h>
#include <drivers/ahci/ahci.h>
#include <drivers/nvme/nvme.h>
#include <drivers/usb/xhci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>
#include <fs/fc.h>
#include <arch/x86_64/simd/simd.h>
#include <mem/heap.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <drivers/framebuffer/fb.h>
#include <klib/renderer/rnd.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/interrupt/idt.h>
#include <mem/pmm.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/sched_bench.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/rtc/rtc.h>
#include <drivers/mouse/x86/ps2mouse.h>

/* x86mem 运行时参数 (NT 存储阈值)。必须走 __KERNEL_INC__ 分支: 内核用
   -mno-sse 编译, 而这个宏会让 x86mem.h 跳过 nmmintrin.h / avx*intrin.h。 */
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
#define __KERNEL_INC__
#include "../../../../ablib/arch/x86_64/x86mem/x86mem.h"
#endif

/* ============================================================================
 * x86_64_init —— Tier0 固定核心 + 组件图驱动启动
 * ----------------------------------------------------------------------------
 * 重构后这里只保留**不可模块化**的 Tier0：
 *   - 早期串口/GDT/IDT/FPU/SIMD/SMEP/SMAP
 *   - PMM/VMM/SLAB/SLUB（页表与分配机制；分配策略座位另议）
 *   - syscall 入口装配（编号/语义/入口固定，实现由组件注册）
 *   - kmod 加载器/治理机制本身（Tier0 固定）
 * 其余全部是内建组件 (builtin component)，由 KmodBootComponents() 按
 * 统一拓扑图（manifest/.in/.out，provider 先序）初始化：
 *   ACPI/MADT -> LAPIC/IOAPIC -> PIT/HPET/RTC -> 文件缓存 -> SMP ->
 *   调度核心 -> 设备核心 -> PCI 核心 -> 驱动组件 -> PCI 枚举 ->
 *   块设备核心 -> VFS -> ext4/FatFs -> 根文件系统 -> 显示 ->
 *   调度安装 -> 网络 -> 用户态进程
 * 任何组件失败 -> 整张 boot 图回滚（本函数打印现场并停机，便于门禁定位）。
 * ==========================================================================*/

/* 引导期自检已在 sti 之前由 x86_64_init 同步调用（见下方 KmodSelfTest）。 */

uint32_t PrintFSERIAL = 0;

extern cpu_t *bsp_cpu;
extern "C" int32_t file_cache_writeback_callback(
    uint64_t file_id,
    const uint8_t *key, 
    uint32_t key_len, void *data, size_t data_len
);
extern void sys_sysinfo_init(void);

extern void enable_smep_smap();

void __init x86_64_init(void){
    /* Tier0 引导期强制关中断：组件图初始化期间任何 IRQ（尤其 PIT IRQ0 在
       handler 注册前被解除屏蔽）都会闯进"未捕获 IRQ -> 打印 -> kprintf 锁
       重入"路径，表现为串口随机停在某组件日志中间。上线（sti+IPI）之前
       不允许外部中断；各组件/驱动注册 handler 后再统一开中断。 */
    asm volatile("cli" ::: "memory");

    /* ---------------- Tier0 早段：不可模块化的核心机制 ---------------- */
    InitFunc("Serial(Simulater)",Serial::Init());
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");
    InitFunc("SSE",sse_enable());
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
    /* NT (streaming store) 阈值 = CPUID 探测到的 L3 容量。
       必须早于 PMM/VMM/SLAB —— 它们上来就是大块 memset/memcpy, 阈值不对
       会让这些搬运过早或过晚切到 streaming store (只影响性能, 但很明显)。
       探测失败时 x86mem_init_cache_limit 会保留 X86MEM_CACHE_LIMIT_DEFAULT。 */
    x86mem_init_cache_limit();
    kinfoln("x86mem: NT threshold = %llu KB (L3)",
            (unsigned long long)(x86mem_cache_limit / 1024));
#endif
    kinfoln("HHDM OFFSET:0x%X",hhdm_offset);

    InitFunc("GDT",GDT::Init(0));
    bsp_cpu->self = bsp_cpu;
    wrmsr(KERNEL_GS_BASE, (uint64_t)bsp_cpu);
    wrmsr(IA32_GS_MSR,(uint64_t)bsp_cpu);
    InitFunc("IDT",idt_init());
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());
    InitFunc("SLAB",SLAB::Init());
#ifdef __x86_64__
    // SLUB fuses into kmalloc only once SLAB itself is live: SLUB::Create borrows
    // SLAB for its cache descriptors, and the self-test mixes both allocators.
    if (SLUB::InitKmalloc()) { kpokln("SLUB: kmalloc caches online (16..1024 B)"); }
    else                     { kpokln("SLUB: kmalloc caches unavailable, using SLAB"); }
    if (SLUB::SelfTest()) { kpokln("SLUB: self-test OK (named cache + kmalloc fusion)"); }
    else                  { kpokln("SLUB: self-test FAILED stage=%u", SLUB::LastFailStage()); }
#endif
    InitFunc("SIMD Core 0",simd_cpu_init(this_cpu()));
    InitFunc("Intel SMEP & SMAP",enable_smep_smap());

    /* syscall 入口装配：编号/参数/返回/错误码/语义固定；实现由组件注册。 */
    InitFunc("Syscall",syscall_init());

    /* ---------------- Tier0: kmod 加载器/治理机制 ---------------- */
    InitFunc("Kmod",KmodInit());

    /* ---------------- Tier1+: 整个内核的组件图（拓扑驱动） ----------------
       这是启动序列组件化的核心：x86_64_init 不再硬编码 ACPI/SMP/PIT/PCI/
       驱动/文件系统/网络/显示/用户态的初始化顺序，而由每个内建组件的
       manifest/provider 依赖在统一拓扑图中表达。 */
    int32_t krc = KmodBootComponents();
    if (krc != KMOD_OK) {
        kerrorln("init: builtin component graph FAILED rc=%d (%s)",
                 (int)krc, KmodElfErrorString(krc));
        KmodDump();
        hcf();
    }

    /* ---------------- Tier2: 可加载 .ko（同一张图的扩展） ---------------- */
    InitFunc("Kmod ingest",KmodBootIngest());
    InitFunc("Kmod boot",KmodBootLoad());

    /* kmod 引导期自检：**在 sti 之前单线程执行**。
       原因：自检要反复加载/热替换/卸载模块（堆+VMM+座位+syscall 治理），
       与桌面/文件缓存/定时器抢占并发交织会放大既有 fs-cache/VMM 锁序问题
       （实测概率性 STALL WATCHDOG）。引导期单线程执行后，桌面线程才在
       下面的 sti+IPI 之后开始运行，模块治理链路的错误可确定性暴露。
       PrintFSERIAL=1：自检日志只走串口，不再重复写 flanterm 帧缓冲。 */
    atomic_store_4(&PrintFSERIAL,1,0);
    {
        int32_t srt = KmodSelfTest();
        kinfoln("[kmod] boot selftest done rc=%d (%s)", (int)srt,
                srt == KMOD_OK ? "PASS" : "FAIL");
        if (srt != KMOD_OK)
            KmodDump();
    }

    sys_sysinfo_init();

    /* ---------------- Tier0 收尾：上线 & idle ---------------- */
    asm volatile("sti");
    LAPIC::IPI(smp_bsp_cpu,SCHED_VEC);
    LAPIC::IPIOthers(smp_bsp_cpu, SCHED_VEC);

    while (true) {
        asm volatile("hlt");
    }
}
