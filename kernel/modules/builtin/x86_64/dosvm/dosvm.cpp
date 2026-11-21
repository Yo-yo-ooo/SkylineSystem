// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * dosvm.cpp — Tier1 内建组件 kernel.arch.dosvm
 * ----------------------------------------------------------------------------
 * 单核可逆退出 IA-32e 的核心：让一个 AP 真正走
 *
 *   long mode -> compatibility(32) -> protected(16) -> REAL MODE
 *            -> 运行 16 位 DOS payload -> 原路回到 long mode
 *
 * 其余核（含 BSP 与桌面 WM）全程留在 x86_64，调度/IPI 由 Tier0 的
 * g_cpu_offline_mask 过滤。硬件依据与完整序列见 programs/dos/trampoline.nasm。
 *
 * 分层：
 *   Tier0  smp.cpp / lapic.cpp / vmm.cpp / sched.cpp：offline 掩码 + IPI 过滤
 *   本文件  DynModule（resident）：trampoline 装载、0xF0 IPI handler、状态机、
 *          影屏、C 导出、syscall 210..214（原 dosvm.ko 门面已合并）
 *   L3     programs/dos/main.cpp：80x25 文本屏窗口客户端
 *
 * 关键设计（与用户讨论的 VM86 方案对比）：
 *   VM86 在 IA-32e 下硬件不支持，且仍需先退出 long mode；guest 线性地址
 *   仍是 20 位。这里我们只让「切换代码」住在 identity 映射的低 64 KiB，
 *   DOS payload 写好影屏后由本组件用 CopyToUser 搬到任意用户地址——地址
 *   解耦发生在软件数据通路上，不需要 VM86、不需要 32 位页表、不需要
 *   TSS/IO bitmap/#GP 反射 monitor。低 1 MiB 只占 16 个物理页，PMM::Init()
 *   已 mark_allocated(0x70, 16) 预留。
 *
 * 已知限制（QEMU 目标平台上可接受，真机需加固）：
 *   - NMI/SMI/#MC 不能屏蔽；实模式 IVT 已把 256 个向量指向 IRET，且窗口
 *     极短、全程 IF=0 + TPR 抬高 + 本地 LAPIC timer 停止 + LVT mask。
 *   - 若目标核在窗口内 triple fault，整机会重启；这是 CPU 模式切换的
 *     固有风险，串口日志是唯一证据，故 init/enter 都打印可检索的标记。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/kdll.h>

#include <arch/x86_64/dosvm/dosvm_contract.h>
#include <arch/x86_64/dosvm/dosvm_trampoline.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/pit/pit.h>
#include <klib/klib.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>

static int32_t dosvm_init(void);
static void    dosvm_exit(void);

KCOMPONENT_BEGIN(dosvm, "kernel.arch.dosvm", "1.0.0", KMOD_TIER_DYN,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_MMIO | KMOD_CAP_LOG |
                            KMOD_CAP_EXPERIMENTAL | KMOD_CAP_SYSCALL_IMPL |
                            KMOD_CAP_MODULE_LOAD | KMOD_CAP_MODULE_UNLOAD),
                 0, "single-core reversible IA-32e exit + 16-bit DOS; merged impl+syscall facade",
                 dosvm_init, dosvm_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(dosvm, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
/* 统一 DynModule 注入服务：dosvm 在 init 里显式注入 Tier0 pmm 与 acpi
 * resident 模块，证明"模块像 DLL 一样链接其他模块"的路径。 */
KCOMPONENT_IMPORT_DYNLINK(dosvm, kdll_ops)
/* 必须晚于 SMP 上线：init 要给每个 AP 安装 0xF0 handler 并读 smp_cpu_list。*/
KCOMPONENT_DEPENDS(dosvm, "kernel.arch.smp", smp)
/* 平台表的提供者；注入时按名字解析（也是 DynModule 依赖边）。 */
KCOMPONENT_DEPENDS(dosvm, "kernel.arch.acpi", acpi)

/* 低内存 state 区布局，必须与 programs/dos/trampoline.nasm 头注逐字节一致。*/
static constexpr uintptr_t ST_BASE     = 0x0007E000ull;
static constexpr uintptr_t ST_SCRATCH  = ST_BASE + 0x100;
static constexpr uintptr_t ST_CR3CLEAN = ST_BASE + 0x0F8;
static constexpr uintptr_t ST_MAGIC    = ST_BASE + 0x120;
static constexpr uintptr_t ST_FLAGS    = ST_BASE + 0x128;
static constexpr uintptr_t ST_STAGE    = ST_BASE + 0x130;  /* 1..11 progress */
/* 16 位 DOS 环境状态（trampoline 与 C++ 共同读写）。 */
static constexpr uintptr_t ST_RUNMODE  = ST_BASE + 0x138;  /* u32: 1 = .COM */
static constexpr uintptr_t ST_EXITCODE = ST_BASE + 0x13C;  /* u32 */
static constexpr uintptr_t ST_WAITKEY  = ST_BASE + 0x140;  /* u8 */
static constexpr uintptr_t ST_EXITED   = ST_BASE + 0x142;  /* u8 */
static constexpr uintptr_t ST_CURX     = ST_BASE + 0x144;  /* u16 */
static constexpr uintptr_t ST_CURY     = ST_BASE + 0x146;  /* u16 */
static constexpr uintptr_t ST_KBN      = ST_BASE + 0x148;  /* u16 */
static constexpr uintptr_t ST_KBI      = ST_BASE + 0x14A;  /* u16 */
static constexpr uintptr_t ST_KBUF     = ST_BASE + 0x150;  /* 32 x u16 */

static constexpr uintptr_t SHADOW      = 0x0007F000ull;
static constexpr uintptr_t IVT_PHYS    = 0x00073000ull;
static constexpr uint16_t  IVT_STUB_OFF= 0x0C00u;  /* bare IRET, past the blob */
static constexpr uint16_t  IDTR_OFF16  = 0x0C04u;
static constexpr uint16_t  LOW_SEG     = 0x7000u;

/* .COM staging window: PSP at 0x71000, image at 0x71100, both inside the
 * 64 KiB block.  Anything past 0x7E000 would run into the state page, so the
 * loader refuses images above 0xC000 bytes rather than let a program smash
 * the trampoline's bookkeeping. */
static constexpr uint64_t  COM_MAX_LEN     = 0x0000C000ull;
static constexpr uint64_t  COM_CMDLINE_MAX = 126ull;
static constexpr uint32_t  COM_KBD_MAX     = 32u;
static constexpr uint16_t  COM_PSP_TOP_SEG = 0x7E00u;  /* first seg past block */

/* 挂起上下文（与 trampoline.nasm 的 S_* 逐字节对应）。 */
static constexpr uintptr_t ST_RESUME   = ST_BASE + 0x190;  /* u16 bit0 */
static constexpr uintptr_t ST_PEND     = ST_BASE + 0x192;  /* u16: 16 / 21 */
static constexpr uintptr_t ST_CTX      = ST_BASE + 0x196;  /* 8 x u16 */
static constexpr uintptr_t ST_SEGS     = ST_BASE + 0x1A6;  /* 4 x u16 */
static constexpr uintptr_t ST_FRAME    = ST_BASE + 0x1AE;  /* 4 x u16 */
static constexpr uintptr_t ST_RETAX    = ST_BASE + 0x1B6;
/* 宿主文件服务请求块。 */
static constexpr uintptr_t ST_FSACT    = ST_BASE + 0x1B8;  /* 1 = pending, 2 = answered */
static constexpr uintptr_t ST_FSFUNC   = ST_BASE + 0x1BA;
static constexpr uintptr_t ST_FSHAND   = ST_BASE + 0x1BC;
static constexpr uintptr_t ST_FSCNT    = ST_BASE + 0x1BE;
static constexpr uintptr_t ST_FSOFF    = ST_BASE + 0x1C0;
static constexpr uintptr_t ST_FSSEG    = ST_BASE + 0x1C2;
static constexpr uintptr_t ST_FSRES    = ST_BASE + 0x1C4;
static constexpr uintptr_t ST_FSCF     = ST_BASE + 0x1C6;
static constexpr uintptr_t ST_FSERR    = ST_BASE + 0x1C8;
/* .EXE 入口（C++ 解析 MZ 头后写入）。 */
static constexpr uintptr_t ST_EXECS    = ST_BASE + 0x1CA;
static constexpr uintptr_t ST_EXEIP    = ST_BASE + 0x1CE;
static constexpr uintptr_t ST_EXESS    = ST_BASE + 0x1D2;
static constexpr uintptr_t ST_EXESP    = ST_BASE + 0x1D6;

/* dosvm_enter 的正向返回码由 contract.h 的宏提供（DOSVM_RC_NEED_FS /
 * DOSVM_RC_SUSPENDED）。这里刻意不再定义同名 constexpr：宏会把标识符直接
 * 替换成字面量，`static constexpr int DOSVM_RC_NEED_FS = 1;` 会被展开成
 * `static constexpr int 1 = 1;`，clang 报 expected unqualified-id。 */

/* 影屏首 16 字节 header，cells 从 +16 开始，共 4000 字节。 */
static constexpr size_t SNAP_CELLS_OFF = DOSVM_HDR;
static constexpr size_t SNAP_CELLS_LEN = DOSVM_CELLS;

static_assert(DOSVM_BUFSZ == 4016u, "snapshot ABI must stay 4016 bytes");
static_assert(SNAP_CELLS_OFF + SNAP_CELLS_LEN == DOSVM_BUFSZ,
              "header + cells must exactly fill the snapshot");
static_assert(DOSVM_TRAMP_BLOB_SIZE < DOSVM_COM_OFF,
              "trampoline blob must stay below the .COM staging window");
static_assert(DOSVM_TRAMP_BLOB_SIZE < IVT_STUB_OFF,
              "trampoline blob must not overlap the real-mode IVT stub");

namespace {

/* ---- 跨核状态机（IPI 异步，全部用原子量）----------------------------- */
volatile uint64_t g_generation  = 0;
volatile uint64_t g_last_cpu    = 0;
volatile uint64_t g_trips       = 0;
volatile uint32_t g_busy        = 0;          /* 1 = 一次往返进行中 */
volatile uint32_t g_request_cpu = 0xFFFFFFFFu;/* 本次 IPI 目标 AP 逻辑 id */
volatile uint32_t g_trip_done   = 0;
volatile uint32_t g_trip_ok     = 0;
volatile uint32_t g_ready       = 0;          /* init 成功且 SMP 就绪 */

/* 影屏本地副本：用户态读这一份，不直接映射内核低内存。*/
alignas(8) uint8_t      g_snapshot[DOSVM_BUFSZ];
volatile int32_t        g_snap_lock = 0;

/* ---- 低内存访问助手 ----------------------------------------------------
 * 关键：C++ 代码运行在 syscall / IPI 中断上下文，CR3 往往是**用户进程
 * 页表**，而用户页表只复制了 PML4[256..511]（high-half），低 4 GiB 的
 * identity 映射（PML4[0]）只存在于 kernel_pagemap。直接访问 0x7E120 会
 * #PF（实测 addr=0x7E120 rip=0xFFFFFFFF80069FE2，整机 HALT）。
 *
 * HHDM 别名位于 PML4E=256，属于被复制到每个用户页表的 high-half，因此
 * HIGHER_HALF(phys) 在任意 CR3 下都能读写同一块物理内存。C++ 侧一律走
 * HHDM；只有 trampoline 汇编必须用 identity 低地址（关 PG 后 HHDM 不存在），
 * 所以 IPI handler 会在调用它之前临时把 CR3 切到 kernel_pagemap。 */
inline uintptr_t low_va(uintptr_t phys) {
    return phys + hhdm_offset;
}
inline volatile uint8_t* low8(uintptr_t p) {
    return reinterpret_cast<volatile uint8_t*>(low_va(p));
}
inline void low_memset(uintptr_t dst, uint8_t v, size_t n) {
    volatile uint8_t* p = low8(dst);
    for (size_t i = 0; i < n; i++) p[i] = v;
}
inline void low_copy(uintptr_t dst, const uint8_t* src, size_t n) {
    volatile uint8_t* d = low8(dst);
    for (size_t i = 0; i < n; i++) d[i] = src[i];
}
inline uint64_t low_read64(uintptr_t p) {
    return *reinterpret_cast<volatile uint64_t*>(low_va(p));
}
inline void low_write64(uintptr_t p, uint64_t v) {
    *reinterpret_cast<volatile uint64_t*>(low_va(p)) = v;
}
inline void low_write32(uintptr_t p, uint32_t v) {
    *reinterpret_cast<volatile uint32_t*>(low_va(p)) = v;
}
inline void low_write16(uintptr_t p, uint16_t v) {
    *reinterpret_cast<volatile uint16_t*>(low_va(p)) = v;
}
inline uint8_t low_read8(uintptr_t p) {
    return *low8(p);
}
inline uint32_t low_read32(uintptr_t p) {
    return *reinterpret_cast<volatile uint32_t*>(low_va(p));
}
inline uint16_t low_read16(uintptr_t p) {
    return *reinterpret_cast<volatile uint16_t*>(low_va(p));
}

/* ---- 本地 LAPIC 中断屏蔽状态 -------------------------------------------
 * Tier0 的 IPI 过滤拦不住本地 LAPIC timer 与 IOAPIC 外部 IRQ，因此切换前
 * 由 0xF0 handler 亲自停 timer、抬 TPR、mask 全部 LVT，回来逐项恢复。 */
struct lapic_saved_t {
    uint64_t tpr, svr;
    uint64_t lvt_timer, lvt_thermal, lvt_perf;
    uint64_t lvt_lint0, lvt_lint1, lvt_error;
};
lapic_saved_t g_lapic_saved;

constexpr uint32_t R_TPR   = 0x080;
constexpr uint32_t R_SVR   = 0x0F0;
constexpr uint32_t R_TMR   = 0x320;
constexpr uint32_t R_THRM  = 0x330;
constexpr uint32_t R_PERF  = 0x340;
constexpr uint32_t R_LINT0 = 0x350;
constexpr uint32_t R_LINT1 = 0x360;
constexpr uint32_t R_ERR   = 0x370;
constexpr uint64_t LVT_MASKED = 1ull << 16;

void mask_local_interrupts() {
    lapic_saved_t& s = g_lapic_saved;
    s.tpr         = LAPIC::Read(R_TPR);
    s.svr         = LAPIC::Read(R_SVR);
    s.lvt_timer   = LAPIC::Read(R_TMR);
    s.lvt_thermal = LAPIC::Read(R_THRM);
    s.lvt_perf    = LAPIC::Read(R_PERF);
    s.lvt_lint0   = LAPIC::Read(R_LINT0);
    s.lvt_lint1   = LAPIC::Read(R_LINT1);
    s.lvt_error   = LAPIC::Read(R_ERR);

    LAPIC::StopTimer();   /* 本地 timer 不走 IPI 过滤，必须单独停 */
    LAPIC::Write(R_THRM,  s.lvt_thermal | LVT_MASKED);
    LAPIC::Write(R_PERF,  s.lvt_perf    | LVT_MASKED);
    LAPIC::Write(R_LINT0, s.lvt_lint0   | LVT_MASKED);
    LAPIC::Write(R_LINT1, s.lvt_lint1   | LVT_MASKED);
    LAPIC::Write(R_ERR,   s.lvt_error   | LVT_MASKED);
    LAPIC::Write(R_TPR,   0xFF);
}

void restore_local_interrupts(cpu_t* cpu) {
    const lapic_saved_t& s = g_lapic_saved;
    LAPIC::Write(R_THRM,  s.lvt_thermal);
    LAPIC::Write(R_PERF,  s.lvt_perf);
    LAPIC::Write(R_LINT0, s.lvt_lint0);
    LAPIC::Write(R_LINT1, s.lvt_lint1);
    LAPIC::Write(R_ERR,   s.lvt_error);
    LAPIC::Write(R_SVR,   s.svr);
    LAPIC::Write(R_TMR,   s.lvt_timer);
    LAPIC::Write(R_TPR,   s.tpr);
    LAPIC::Oneshot(SCHED_VEC, cpu->base_quantum * cpu->lapic_ticks);
}

/* ---- 实模式 IVT ---------------------------------------------------------
 * 256 个 4 字节向量 (offset16, segment16) 全部指向 0x7000:0x0800 处的
 * 单字节 IRET (0xCF)。由 C++ 填而非汇编填，避免 BITS 16 重定位表达式。 */
void install_real_mode_ivt() {
    *low8(DOSVM_LOW_BASE + IVT_STUB_OFF) = 0xCF;   /* IRET */
    low_write16(DOSVM_LOW_BASE + IDTR_OFF16,     0x03FF);
    low_write32(DOSVM_LOW_BASE + IDTR_OFF16 + 2, (uint32_t)IVT_PHYS);
    for (uint32_t i = 0; i < 256; i++) {
        low_write16(IVT_PHYS + i * 4,     IVT_STUB_OFF);
        low_write16(IVT_PHYS + i * 4 + 2, LOW_SEG);
    }
}

/* ---- 影屏快照 ---------------------------------------------------------- */
void refresh_snapshot_local() {
    uint64_t flags = DOSVM_FLAG_VALID;
    if (low_read64(ST_FLAGS) & 1u) flags |= DOSVM_FLAG_REAL_OK;
    if (low_read32(ST_RUNMODE) != 0) flags |= DOSVM_FLAG_COM_MODE;
    if (low8(ST_EXITED)[0])          flags |= DOSVM_FLAG_COM_EXITED;
    if (low8(ST_WAITKEY)[0])         flags |= DOSVM_FLAG_WAITKEY;
    if (low_read16(ST_FSACT) == 1)   flags |= DOSVM_FLAG_WAIT_FS;
    const uint64_t gen   = __atomic_load_n(&g_generation, __ATOMIC_RELAXED);
    const uint64_t trips = __atomic_load_n(&g_trips,      __ATOMIC_RELAXED);
    const uint64_t cpu   = __atomic_load_n(&g_last_cpu,   __ATOMIC_RELAXED);

    uint8_t* buf = g_snapshot;
    for (size_t i = 0; i < DOSVM_HDR; i++) buf[i] = 0;
    /* 手动小端组装，避免对可能非对齐的字节数组做 uint64 写入。 */
    for (int b = 0; b < 8; b++) {
        buf[b]     = (uint8_t)((gen | (flags << 32)) >> (8 * b));
        buf[8 + b] = (uint8_t)((cpu | (trips << 32)) >> (8 * b));
    }
    volatile uint8_t* cells = low8(SHADOW + SNAP_CELLS_OFF);
    for (size_t i = 0; i < SNAP_CELLS_LEN; i++)
        buf[SNAP_CELLS_OFF + i] = cells[i];
}

/* ---- 0xF0 IPI handler --------------------------------------------------
 * 运行在目标 AP 的中断上下文。0xF0 不在 idt_irq_handler 的自动 EOI 区间
 * (0x20..0x3F)，必须自己 EOI，否则 LAPIC ISR 会一直挂着该向量。 */
extern "C" void dosvm_ipi_handler(context_t* /*ctx*/) {
    LAPIC::EOI();

    cpu_t* cpu = this_cpu();
    const uint32_t me = cpu ? cpu->id : 0xFFFFFFFFu;
    const uint32_t want = __atomic_load_n(&g_request_cpu, __ATOMIC_ACQUIRE);
    if (!cpu || me != want) return;

    /* 建立安全窗口：cli 之后到 trampoline 返回之间，本核不响应任何外部
     * 中断，也不再被调度器/TLB shootdown 触碰（Tier0 已置 offline 位）。 */
    asm volatile("cli" ::: "memory");
    mask_local_interrupts();

    low_write64(ST_MAGIC, 0);
    low_write64(ST_FLAGS, 0);
    low_write64(ST_STAGE, 0);

    /* trampoline 在关 PG 前后都用 identity 低地址取指/访问 state，而用户
     * 进程页表没有 PML4[0] 的 identity 映射。临时切到 kernel_pagemap 的
     * 干净 CR3（不带 PCID / no-flush 位，因为 trampoline 会清 CR4.PCIDE），
     * 回来后恢复被打断进程原来的 CR3。这里不能用 SwitchPageMap()：它会
     * 改写 this_cpu()->pagemap 并调用 LazyTLB，在中断上下文里不安全。 */
    uint64_t saved_cr3 = 0;
    uint64_t kernel_cr3 = PHYSICAL((uint64_t)kernel_pagemap->toplvl) & ~0xFFFull;
    asm volatile("mov %%cr3, %0" : "=r"(saved_cr3) :: "memory");
    asm volatile("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    /* 调用 identity 映射的 flat-binary trampoline。它保存所有 GPR/CR/MSR、
     * 切模式、跑 16 位 payload、原路返回，最后恢复 RSP 并 ret 到这里。 */
    typedef void (*tramp_fn)(void);
    tramp_fn fn = reinterpret_cast<tramp_fn>(DOSVM_LOW_BASE);
    fn();

    asm volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");

    const uint64_t magic = low_read64(ST_MAGIC);
    const bool reached_real = (low_read64(ST_FLAGS) & 1u) != 0;
    const bool ok = (magic == DOSVM_STATE_MAGIC) && reached_real;

    /* 回到 long mode 后做一次全刷：offline 期间别的核跳过了本核的 TLB
     * shootdown，pm->cpus_with_tlb 里可能还留着陈旧条目。 */
    asm volatile("mov %%cr3, %%rax\n\t"
                 "mov %%rax, %%cr3" ::: "rax", "memory");

    restore_local_interrupts(cpu);

    __atomic_store_n(&g_trip_ok, ok ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trip_done, 1u, __ATOMIC_RELEASE);
}

/* ---- 组件 init --------------------------------------------------------- */
int32_t dosvm_component_init() {
    if (!smp_started || smp_last_cpu < 1) {
        kprintf_("[dosvm] DISABLED: no AP available (last_cpu=%d)\n",
                 (int)smp_last_cpu);
        return KMOD_OK;    /* 单核机器优雅降级，不阻断启动 */
    }

    /* 1) 清出整段低 64 KiB，再拷入 trampoline blob。 */
    low_memset(DOSVM_LOW_BASE, 0, (size_t)DOSVM_LOW_SIZE);
    low_copy(DOSVM_LOW_BASE, kDosvmTrampoline, DOSVM_TRAMP_BLOB_SIZE);

    /* 2) 实模式 IVT + IRET stub（防 NMI/SMI/#MC 落到 64 位 IDT 门上）。*/
    install_real_mode_ivt();

    /* 3) CR3_CLEAN：kernel_pagemap 顶层页表的物理地址，低 12 位清零。
     *    trampoline 回程在 PCIDE=0 状态下加载它；若带 PCID 会 #GP。 */
    uint64_t toplvl_va = (uint64_t)kernel_pagemap->toplvl;
    uint64_t toplvl_pa = (toplvl_va >= hhdm_offset)
                       ? (toplvl_va - hhdm_offset) : toplvl_va;
    low_write64(ST_CR3CLEAN, toplvl_pa & ~0xFFFull);
    low_write64(ST_MAGIC, 0);
    low_write64(ST_FLAGS, 0);

    /* 4) 影屏初始画面：80x25 全部空格 + 灰字，header 由 refresh 填。 */
    volatile uint8_t* cells = low8(SHADOW + SNAP_CELLS_OFF);
    for (size_t i = 0; i < SNAP_CELLS_LEN; i += 2) {
        cells[i]     = ' ';
        cells[i + 1] = 0x07;
    }

    /* 5) 给每个非 BSP 核安装 0xF0 handler。IDT 是 per-CPU 的，逐核装。 */
    for (int32_t i = 1; i <= smp_last_cpu; i++) {
        if (!smp_cpu_list[i]) continue;
        idt_install_irq_cpu((uint32_t)i, DOSVM_IPI_VEC,
                            (void*)&dosvm_ipi_handler);
    }

    __atomic_store_n(&g_ready, 1u, __ATOMIC_RELEASE);
    kprintf_("[dosvm] ready: blob=%u B @0x70000 pml4_pa=0x%llx "
             "ipi=0xF0 aps=1..%d syscall=210/211\n",
             (unsigned)DOSVM_TRAMP_BLOB_SIZE,
             (unsigned long long)(toplvl_pa & ~0xFFFull),
             (int)smp_last_cpu);
    return KMOD_OK;
}

}  /* namespace */

/* ==========================================================================
 * 统一 DynModule：dosvm 不再拆成 Tier1 实现 + Tier2 .ko 门面。
 * 本组件同时承担：
 *   - 真正的 long->real->long 实现；
 *   - syscall 210..214 的注册（KCOMPONENT_EXPORT_SYSCALL）；
 *   - DLL 式注入 pmm/acpi 的示范（KCOMPONENT_IMPORT_DYNLINK）。
 * ========================================================================== */
static int32_t g_inject_pmm_rc = KMOD_OK;
static int32_t g_inject_acpi_rc = KMOD_OK;

static int32_t dosvm_init(void) {
    int32_t rc = dosvm_component_init();
    if (rc != KMOD_OK)
        return rc;

    /* 像 LoadLibrary 一样把 Tier0 pmm 与平台 acpi 模块拉进本模块的链接面。
     * 两者此刻都已 resident/builtin-READY，所以注入是幂等 + 加 PIN；如果
     * 尚未加载，KdllInject 会走同一个动态链接事务。 */
    if (kdll_ops) {
        g_inject_pmm_rc = kdll_ops->inject(KDLL_CORE_PMM,
                                           KMOD_INJ_PIN | KMOD_INJ_SILENT);
        g_inject_acpi_rc = kdll_ops->inject("kernel.arch.acpi",
                                            KMOD_INJ_PIN | KMOD_INJ_SILENT);
        kprintf_("[dosvm] dynlink: inject %s rc=%d, %s rc=%d (pins=%u/%u)\n",
                 KDLL_CORE_PMM, (int)g_inject_pmm_rc, "kernel.arch.acpi",
                 (int)g_inject_acpi_rc,
                 (unsigned)KdllPinCount(KDLL_CORE_PMM),
                 (unsigned)KdllPinCount("kernel.arch.acpi"));
        if (g_inject_pmm_rc != KMOD_OK || g_inject_acpi_rc != KMOD_OK) {
            /* 注入失败不回滚整个 dosvm：AP/实模式窗口仍然可用，但把失败
             * 明确暴露在启动日志里，避免"看似 ready、实际缺依赖"。 */
            kprintf_("[dosvm] WARN: dynlink injection incomplete\n");
        }
    } else {
        kprintf_("[dosvm] WARN: kernel.dynlink.v1 unavailable\n");
    }
    return KMOD_OK;
}

static void dosvm_exit(void) {
    if (kdll_ops) {
        (void)kdll_ops->eject("kernel.arch.acpi");
        (void)kdll_ops->eject(KDLL_CORE_PMM);
    }
    __atomic_store_n(&g_ready, 0u, __ATOMIC_RELEASE);
    kprintf_("[dosvm] exit: APs remain online, trampoline left mapped\n");
}

/* ==========================================================================
 * 5 个 extern "C" 导出（kernel_exports.cpp 的 .kout 表把它们发布给 .ko）
 * ========================================================================== */
extern "C" int dosvm_enter(uint32_t cpu_id)
{
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return -ENODEV;
    if (smp_last_cpu < 1) return -ENODEV;

    /* 一次只允许一个往返：g_busy 用 CAS，避免两个用户进程同时摘同一个核。*/
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_busy, &expected, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -EBUSY;

    /* cpu_id==0 表示「自动挑一个非 BSP 的 AP」。Tier0 的
     * dosvm_core_offline(0) 返回 -EINVAL（BSP 不可摘），所以绝不能把 0
     * 原样传下去；这里映射到 1..smp_last_cpu 的真实逻辑 id。
     *
     * 更关键：必须排除「当前正在执行本 syscall 的核」。syscall 内核路径
     * IF=0（IA32_FMASK 清了 IF），若把 IPI 发给自己，LAPIC 的自 IPI 永远
     * 投递不进来，而这里又在忙等 g_trip_done —— 结果是本核死锁，wired
     * STALL WATCHDOG 报 "vec=0 (syscall) 3000ms"。实测 dos.elf 被调度到
     * cpu=1 且自动选核也选中 1 时就命中了这个坑。 */
    cpu_t* self = this_cpu();
    const uint32_t self_id = self ? self->id : 0;
    uint32_t target = cpu_id;
    if (target == 0) {
        uint64_t n = __atomic_load_n(&g_trips, __ATOMIC_RELAXED);
        /* 在 1..smp_last_cpu 里轮转，跳过 self_id；至少 2 个 AP 时一定能
         * 找到别的核。单 AP（smp_last_cpu==1）时若自己就是那颗 AP，
         * 明确返回 -EBUSY，而不是制造死锁。 */
        target = 0;
        for (uint32_t k = 1; k <= (uint32_t)smp_last_cpu; k++) {
            uint32_t cand = 1u + (uint32_t)((n + k - 1u) %
                                            (uint32_t)smp_last_cpu);
            if (cand == self_id) continue;
            if (!smp_cpu_list[cand]) continue;
            target = cand;
            break;
        }
        if (target == 0) {
            __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
            return -EBUSY;   /* 唯一可用的 AP 正是当前核 */
        }
    }
    if (target > (uint32_t)smp_last_cpu || !smp_cpu_list[target] ||
        target == self_id) {
        __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
        return -EINVAL;
    }

    cpu_t* tcpu = smp_cpu_list[target];
    int rc = dosvm_core_offline(target);
    if (rc != 0) {
        __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
        return rc;
    }

    __atomic_store_n(&g_trip_done, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trip_ok,   0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_request_cpu, target, __ATOMIC_RELEASE);

    kprintf_("[dosvm] trip begin: cpu=%u lapic_id=%u\n",
             (unsigned)target, (unsigned)tcpu->lapic_id);

    /* 0xF0 是 Tier0 唯一放行给 offline 核的向量。 */
    LAPIC::IPI(tcpu->lapic_id, DOSVM_IPI_VEC);

    /* 等目标核回来。超时用 TSC-based MonotonicMS（不依赖中断/调度）。*/
    const uint64_t deadline = PIT::MonotonicMS() + 2000u;
    while (!__atomic_load_n(&g_trip_done, __ATOMIC_ACQUIRE)) {
        if (PIT::MonotonicMS() > deadline) break;
        asm volatile("pause");
    }
    const uint32_t done = __atomic_load_n(&g_trip_done, __ATOMIC_ACQUIRE);
    const uint32_t ok   = __atomic_load_n(&g_trip_ok,   __ATOMIC_ACQUIRE);

    __atomic_store_n(&g_request_cpu, 0xFFFFFFFFu, __ATOMIC_RELEASE);

    /* 超时的核卡在实模式里回不来（IF=0，任何 IPI 都投不进去）。这时把
     * 它标回 online 是灾难性的：调度器会继续往那颗核投任务，而它永远
     * 不会响应。早期 payload 是固定代码不会死循环，所以一直无条件
     * online；现在 .COM 是任意程序，死循环（jmp $）是必须假设会发生的
     * 情况，因此只有真的回来了才归还给调度器。 */
    if (done)
        dosvm_core_online(target);
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);

    if (!done) {
        kprintf_("[dosvm] trip TIMEOUT on cpu=%u stage=%llu magic=0x%llx "
                 "(core left OFFLINE)\n",
                 (unsigned)target,
                 (unsigned long long)low_read64(ST_STAGE),
                 (unsigned long long)low_read64(ST_MAGIC));
        return -ETIMEDOUT;
    }
    if (!ok) {
        kprintf_("[dosvm] trip FAILED on cpu=%u stage=%llu magic=0x%llx "
                 "flags=0x%llx\n",
                 (unsigned)target,
                 (unsigned long long)low_read64(ST_STAGE),
                 (unsigned long long)low_read64(ST_MAGIC),
                 (unsigned long long)low_read64(ST_FLAGS));
        return -EAGAIN;
    }

    __atomic_add_fetch(&g_trips, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_last_cpu, (uint64_t)target, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_generation, 1, __ATOMIC_ACQ_REL);

    /* 刷新本地快照（持轻量自旋锁，避免与 dosvm_read 并发读到半份）。*/
    int32_t snap_expected = 0;
    while (!__atomic_compare_exchange_n(&g_snap_lock, &snap_expected, 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        snap_expected = 0;
        asm volatile("pause");
    }
    refresh_snapshot_local();
    __atomic_store_n(&g_snap_lock, 0, __ATOMIC_RELEASE);

    kprintf_("[dosvm] trip OK: cpu=%u gen=%llu trips=%llu\n",
             (unsigned)target,
             (unsigned long long)__atomic_load_n(&g_generation, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&g_trips, __ATOMIC_RELAXED));

    /* 把 16 位侧的挂起原因翻译成正向返回码：用户态据此决定是交键恢复、
     * 代执行文件请求，还是就此收工。返回值 0 之外不再有别的正向语义。 */
    if (low_read16(ST_FSACT) == 1) {
        const uint16_t fn = low_read16(ST_FSFUNC);
        kprintf_("[dosvm] host fs request: ah=0x%x bx=%u cx=%u\n",
                 (unsigned)(fn >> 8),
                 (unsigned)low_read16(ST_FSHAND),
                 (unsigned)low_read16(ST_FSCNT));
        return DOSVM_RC_NEED_FS;
    }
    if (low_read16(ST_RESUME) != 0)
        return DOSVM_RC_SUSPENDED;
    return 0;
}

extern "C" long dosvm_read(void* user_buf, uint64_t cap)
{
    if (!user_buf || cap < DOSVM_BUFSZ) return -EINVAL;
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return -EAGAIN;
    if (__atomic_load_n(&g_generation, __ATOMIC_ACQUIRE) == 0) return -EAGAIN;

    /* 拷一份到本地栈缓冲，持锁时间只包含 memcpy，不含 CopyToUser。*/
    uint8_t tmp[DOSVM_BUFSZ];
    int32_t snap_expected = 0;
    while (!__atomic_compare_exchange_n(&g_snap_lock, &snap_expected, 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        snap_expected = 0;
        asm volatile("pause");
    }
    for (size_t i = 0; i < DOSVM_BUFSZ; i++) tmp[i] = g_snapshot[i];
    __atomic_store_n(&g_snap_lock, 0, __ATOMIC_RELEASE);

    proc_t* proc = Schedule::this_proc();
    if (!proc || !proc->pagemap) return -EFAULT;
    if (!VMM::UserAccess::CopyToUser(proc->pagemap, (uint64_t)user_buf,
                                     tmp, DOSVM_BUFSZ))
        return -EFAULT;
    return (long)DOSVM_BUFSZ;
}

/* 把用户态读进来的 .COM 映像摆到 0x71000 的加载区，并构造 PSP。
 * len == 0 表示卸载映像、回到内置 banner 演示。
 * 一次 trip 只允许一个映像在途；所有写入都经 HHDM 别名落到物理低内存。 */
extern "C" int dosvm_stage_com(void* user_img, uint64_t len,
                              void* user_args, uint64_t args_len)
{
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return -ENODEV;
    if (args_len > COM_CMDLINE_MAX) return -EINVAL;
    if (len > COM_MAX_LEN) return -EINVAL;
    if (len != 0 && !user_img) return -EINVAL;
    if (args_len != 0 && !user_args) return -EINVAL;
    if (__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return -EBUSY;

    proc_t* proc = Schedule::this_proc();
    if (!proc || !proc->pagemap) return -EFAULT;

    const uintptr_t block = DOSVM_LOW_BASE + DOSVM_COM_OFF;

    /* 整窗清零: 短映像不能继承上一份的长尾。 */
    low_memset(block, 0, COM_MAX_LEN + 0x100);

    if (len == 0) {
        low_write32(ST_RUNMODE, 0);
        kprintf_("[dosvm] --com: image ejected, back to banner\n");
        return 0;
    }

    uint8_t chunk[4096];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!VMM::UserAccess::CopyFromUser(proc->pagemap, chunk,
                                           (const void*)((uint64_t)user_img + done),
                                           n))
            return -EFAULT;
        low_copy(block + 0x100 + done, chunk, (size_t)n);
        done += n;
    }

    /* PSP 按 MS-DOS 的布局:
       +0x00 int 20h  (程序用裸 ret 也能干净退出)
       +0x02 段顶     (第一个不属于本 64 KiB 块的段)
       +0x80 命令行长度 + 命令尾 */
    low_write16(block + 0x00, 0x20CDu);
    low_write16(block + 0x02, (uint16_t)COM_PSP_TOP_SEG);
    if (args_len) {
        uint8_t cmd[COM_CMDLINE_MAX];
        if (!VMM::UserAccess::CopyFromUser(proc->pagemap, cmd,
                                           (const void*)user_args, args_len))
            return -EFAULT;
        low8(block + 0x80)[0] = (uint8_t)args_len;
        low_copy(block + 0x81, cmd, (size_t)args_len);
        low8(block + 0x81 + args_len)[0] = 0x0Du;
    }

    /* 新映像作废任何挂起上下文与未完成的文件请求。 */
    low_write16(ST_RESUME, 0);
    low_write16(ST_FSACT, 0);
    low_write16(ST_FSHAND, 0);

    const uintptr_t img = block + 0x100;
    const bool is_mz = (len >= 0x1Cu) && low_read16(img) == 0x5A4Du; /* 'MZ' */

    if (is_mz) {
        /* ---- .EXE: 解析 MZ 头、应用重定位、按 header 定入口 ---- */
        const uint16_t e_cblp    = low_read16(img + 0x02);
        const uint16_t e_cp      = low_read16(img + 0x04);
        const uint16_t e_crlc    = low_read16(img + 0x06);
        const uint16_t e_cparhdr = low_read16(img + 0x08);
        const uint16_t e_ss      = low_read16(img + 0x0E);
        const uint16_t e_sp      = low_read16(img + 0x10);
        const uint16_t e_cs      = low_read16(img + 0x14);
        const uint16_t e_ip      = low_read16(img + 0x16);
        const uint16_t e_lfarlc  = low_read16(img + 0x18);

        const uint64_t hdr = (uint64_t)e_cparhdr * 16u;
        if (e_cp == 0 || hdr >= len) return -EINVAL;

        uint64_t payload = (uint64_t)(e_cp - 1u) * 512u + e_cblp;
        if (payload > len - hdr) payload = len - hdr;
        if (payload > COM_MAX_LEN - 0x100) return -EINVAL;

        /* 映像起点段：0x71100 / 16。DOS 把整份文件（含 header）拷到
         * load_seg:0000，CS/SS 都是相对这个起点的段值。 */
        const uint16_t load_seg = (uint16_t)((DOSVM_LOW_BASE + DOSVM_COM_OFF + 0x100) / 16u);

        for (uint16_t i = 0; i < e_crlc; i++) {
            const uintptr_t ro = (uintptr_t)e_lfarlc + (uintptr_t)i * 4u;
            const uint16_t r_off = low_read16(img + ro);
            const uint16_t r_seg = low_read16(img + ro + 2u);
            const uintptr_t target = img + (uintptr_t)r_seg * 16u + r_off;
            /* 重定位目标必须落在我们这块 64 KiB 里，否则直接拒绝而不是
             * 让一次 16 位写穿掉别的内存。 */
            if (target < img || target + 2u > block + COM_MAX_LEN)
                return -EINVAL;
            low_write16(target, (uint16_t)(low_read16(target) + load_seg));
        }

        const uint16_t cs = (uint16_t)(load_seg + e_cs);
        const uint16_t ss = (uint16_t)(load_seg + e_ss);
        /* 栈顶（以及入口）必须留在低内存预留区里。 */
        if ((uint64_t)ss * 16u + e_sp > 0x00083000ull) return -EINVAL;
        if ((uint64_t)cs * 16u + e_ip > 0x00083000ull) return -EINVAL;

        low_write16(ST_EXECS, cs);
        low_write16(ST_EXEIP, e_ip);
        low_write16(ST_EXESS, ss);
        low_write16(ST_EXESP, e_sp);
        low_write32(ST_RUNMODE, 2);

        kprintf_("[dosvm] staged .EXE: %llu B reloc=%u cs=%04x ip=%04x "
                 "ss=%04x sp=%04x\n",
                 (unsigned long long)payload, (unsigned)e_crlc,
                 (unsigned)cs, (unsigned)e_ip, (unsigned)ss, (unsigned)e_sp);
        return 0;
    }

    low_write32(ST_RUNMODE, 1);
    kprintf_("[dosvm] staged .COM: %llu B, cmd tail %llu B\n",
             (unsigned long long)len, (unsigned long long)args_len);
    return 0;
}

/* 预置键盘队列。元素是 u16: AH = scan code, AL = ascii。实模式里 IF=0，
 * 没有活键盘路径，所以按键必须在 trip 之前塞进来。 */
extern "C" int dosvm_push_keys(void* user_keys, uint64_t count)
{
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return -ENODEV;
    if (__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return -EBUSY;
    if (!user_keys || count == 0) return -EINVAL;
    if (count > COM_KBD_MAX) count = COM_KBD_MAX;

    proc_t* proc = Schedule::this_proc();
    if (!proc || !proc->pagemap) return -EFAULT;

    uint16_t keys[COM_KBD_MAX];
    if (!VMM::UserAccess::CopyFromUser(proc->pagemap, keys,
                                       (const void*)user_keys, count * 2u))
        return -EFAULT;

    for (uint32_t i = 0; i < (uint32_t)count; i++)
        low_write16(ST_KBUF + (uintptr_t)i * 2u, keys[i]);
    low_write16(ST_KBN, (uint16_t)count);
    low_write16(ST_KBI, 0);
    return 0;
}

/* 低内存窗口读写：用户态只能看到自己的地址空间，而 16 位程序的映像、
 * PSP、挂起上下文和文件请求块都在 0x70000..0x7FFFF 这块物理内存里。
 * 这条 syscall 是唯一的合法通道，off 相对 DOSVM_LOW_BASE，边界由内核
 * 卡死在本块内 —— 用户态拿不到这块以外的任何物理内存。
 * is_write != 0: 用户 -> 低内存；否则 低内存 -> 用户。 */
extern "C" int dosvm_mem_rw(uint64_t off, void* user_buf, uint64_t len,
                            uint32_t is_write)
{
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return -ENODEV;
    if (!user_buf || len == 0) return -EINVAL;
    if (off >= DOSVM_LOW_SIZE) return -EINVAL;
    if (len > DOSVM_LOW_SIZE - off) return -EINVAL;

    proc_t* proc = Schedule::this_proc();
    if (!proc || !proc->pagemap) return -EFAULT;

    uint8_t chunk[512];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        const uintptr_t pa = DOSVM_LOW_BASE + off + done;
        if (is_write) {
            if (!VMM::UserAccess::CopyFromUser(proc->pagemap, chunk,
                                               (const void*)((uint64_t)user_buf + done),
                                               n))
                return -EFAULT;
            low_copy(pa, chunk, (size_t)n);
        } else {
            for (uint64_t i = 0; i < n; i++) chunk[i] = low8(pa)[i];
            if (!VMM::UserAccess::CopyToUser(proc->pagemap,
                                             (uint64_t)user_buf + done,
                                             chunk, n))
                return -EFAULT;
        }
        done += n;
    }
    return 0;
}

extern "C" uint64_t dosvm_generation(void) {
    return __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
}
extern "C" uint64_t dosvm_last_cpu(void) {
    return __atomic_load_n(&g_last_cpu, __ATOMIC_ACQUIRE);
}
extern "C" uint64_t dosvm_trips(void) {
    return __atomic_load_n(&g_trips, __ATOMIC_ACQUIRE);
}

/* ==========================================================================
 * syscall 210..214：原 Tier2 dosvm.ko 的薄门面，合并进本 DynModule。
 * 注册槽仍在动态命名空间 [128,511]；固定 dispatch ABI 不变，用户态无感。
 * ========================================================================== */
static uint64_t dosvm_read_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5,
                                   void *frame) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return static_cast<uint64_t>(dosvm_read(reinterpret_cast<void *>(a0), a1));
}

static uint64_t dosvm_enter_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5,
                                    void *frame) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return static_cast<uint64_t>(
        static_cast<int64_t>(dosvm_enter(static_cast<uint32_t>(a0))));
}

static uint64_t dosvm_stage_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5,
                                    void *frame) {
    (void)a4; (void)a5; (void)frame;
    return static_cast<uint64_t>(static_cast<int64_t>(
        dosvm_stage_com(reinterpret_cast<void *>(a0), a1,
                        reinterpret_cast<void *>(a2), a3)));
}

static uint64_t dosvm_keys_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5,
                                   void *frame) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return static_cast<uint64_t>(static_cast<int64_t>(
        dosvm_push_keys(reinterpret_cast<void *>(a0), a1)));
}

static uint64_t dosvm_mem_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5,
                                  void *frame) {
    (void)a4; (void)a5; (void)frame;
    return static_cast<uint64_t>(static_cast<int64_t>(
        dosvm_mem_rw(a0, reinterpret_cast<void *>(a1), a2,
                     static_cast<uint32_t>(a3))));
}

KCOMPONENT_EXPORT_SYSCALL(dosvm, DOSVM_SYSCALL_READ, dosvm_read_syscall);
KCOMPONENT_EXPORT_SYSCALL(dosvm, DOSVM_SYSCALL_ENTER, dosvm_enter_syscall);
KCOMPONENT_EXPORT_SYSCALL(dosvm, DOSVM_SYSCALL_STAGE, dosvm_stage_syscall);
KCOMPONENT_EXPORT_SYSCALL(dosvm, DOSVM_SYSCALL_KEYS, dosvm_keys_syscall);
KCOMPONENT_EXPORT_SYSCALL(dosvm, DOSVM_SYSCALL_MEM, dosvm_mem_syscall);
KCOMPONENT_EXPORT_NAMESPACE(dosvm, "vendor.dosvm.syscall", dosvm_ns);

KCOMPONENT_END(dosvm)
