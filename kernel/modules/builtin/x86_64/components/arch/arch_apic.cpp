//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * arch_apic.cpp —— 内建组件 kernel.arch.apic (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：
 *   DISABLE PIC:  outb(PIC1_DATA,0xff) / outb(PIC2_DATA,0xff)   // 原 165-166
 *   ENABLE ICMR:  outb(0x22,0x70) / outb(0x23,0x01)             // 原 169-170
 *   InitFunc("LAPIC", LAPIC::Init());                           // 原 173
 *   InitFunc("IOAPIC", IOAPIC::Init());                         // 原 174
 *   IOAPIC::RemapIRQ(bsp_cpu->lapic_id, 0, 32, false);          // 原 204
 *
 * 依赖 kernel.arch.acpi（LAPIC/IOAPIC 的 MMIO 基址来自 MADT/MCFG 解析）。
 * 迁移注意：旧代码的 RemapIRQ 在 smp_init() 之后（bsp_cpu->lapic_id 已由 MP
 * 响应赋值）；组件拓扑里 apic 在 smp 之前，这里改用 LAPIC::GetID() 读硬件
 * APIC ID（与 MP 响应的 BSP LAPIC ID 等价，且在非 0 BSP 的机器上更稳）。
 *
 * 导出契约 kernel.apic.v1（无独立头文件；消费者极少，先内联固定布局）：
 *   abi/ops_size + available() + lapic_id() + eoi()
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <klib/kio.h>   /* outb */

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

#define KMOD_CONTRACT_APIC_V1 "kernel.apic.v1"
#define KMOD_APIC_ABI_V1      1u

static int32_t arch_apic_init(void);
static void arch_apic_exit(void);

KCOMPONENT_BEGIN(arch_apic, "kernel.arch.apic", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_IRQ |
                                          KMOD_CAP_LOG),
                 0, "LAPIC/IOAPIC init + 8259 PIC mask (IRQ0 remap in kernel.arch.irq)",
                 arch_apic_init, arch_apic_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(arch_apic, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(arch_apic, "kernel.arch.acpi", acpi)

namespace {

/* kernel.apic.v1 的线格式（v1 冻结；变更须升 abi 并保留旧结构）。 */
typedef struct kmod_apic_ops {
    uint32_t abi_version;   /* = KMOD_APIC_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_apic_ops_t) */
    int32_t (*available)(void);
    uint32_t (*lapic_id)(void);
    void (*eoi)(void);
} kmod_apic_ops_t;

bool g_apic_ok = false;

int32_t apic_available(void)
{
    return g_apic_ok ? 1 : 0;
}

uint32_t apic_lapic_id(void)
{
    return (uint32_t)LAPIC::GetID();
}

void apic_eoi(void)
{
    LAPIC::EOI();
}

const kmod_apic_ops_t g_apic_ops = {
    KMOD_APIC_ABI_V1,
    (uint32_t)sizeof(kmod_apic_ops_t),
    apic_available,
    apic_lapic_id,
    apic_eoi,
};

} /* namespace */

static int32_t arch_apic_init(void)
{
    /* 1) 屏蔽 8259 PIC（Tier0 走 IOAPIC 后 legacy PIC 不得再产生 IRQ） */
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
    /* 2) 打开 ICMR（与原 init.cpp 完全一致的两条端口写） */
    outb(0x22, 0x70);
    outb(0x23, 0x01);

    LAPIC::Init();
    IOAPIC::Init();
    /* 3) 注意：legacy PIT (IRQ0/GSI0) 的 IOAPIC 路由/解除屏蔽不在这里做。
     *    旧 init.cpp 中 RemapIRQ 位于 smp_init() 之后、PIT 初始化之后；
     *    组件化后由 kernel.arch.irq 组件负责（deps: apic+time.pit+arch.smp），
     *    否则会出现"IRQ0 已解除屏蔽但 PIT handler 尚未注册"的窗口：
     *    引导期中断一旦触发会进入 Uncaught IRQ 打印路径并在 kprintf 锁上
     *    自死锁（QEMU 串口停在随机组件处、无异常输出的根因）。 */

    g_apic_ok = true;
    kprintf_("[comp.arch.apic] init: LAPIC/IOAPIC ready, PIC masked "
             "(IRQ0 remap deferred to kernel.arch.irq)\n");
    return KMOD_OK;
}

static void arch_apic_exit(void)
{
    g_apic_ok = false;
    kprintf_("[comp.arch.apic] exit (LAPIC/IOAPIC hardware left as configured)\n");
}

KCOMPONENT_EXPORT_CONTRACT(arch_apic, KMOD_CONTRACT_APIC_V1, g_apic_ops,
                           KMOD_APIC_ABI_V1)
KCOMPONENT_END(arch_apic)
