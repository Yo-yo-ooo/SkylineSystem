//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * arch_irq.cpp —— 内建组件 kernel.arch.irq (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：IOAPIC::RemapIRQ(bsp_cpu->lapic_id, 0, 32, false);
 *
 * 为什么单独成组件（而不是塞进 kernel.arch.apic）：
 *   旧 init.cpp 的顺序是 PIT::InitPIT() -> ... -> smp_init() -> RemapIRQ，
 *   即"先注册 PIT IRQ0 handler，再解除 IRQ0 屏蔽，且 bsp_lapic_id 已由 SMP
 *   响应确定"。若在 apic 组件里提前 RemapIRQ，引导期会出现 IRQ0 已通但
 *   cpu->handlers[32]==NULL 的窗口，进入 Uncaught IRQ 打印路径并在 kprintf
 *   锁上自死锁（实测：串口随机停在某个组件日志中间、无异常输出）。
 *   本组件显式表达这条顺序依赖：apic + time.pit + arch.smp -> irq。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/smp/smp.h>

extern cpu_t *bsp_cpu;

static int32_t arch_irq_init(void);
static void arch_irq_exit(void);

KCOMPONENT_BEGIN(arch_irq, "kernel.arch.irq", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_LOG), 0,
                 "legacy PIT IRQ0/GSI0 -> vector 32 IOAPIC unmask (after PIT handler + SMP)",
                 arch_irq_init, arch_irq_exit, 0, 0, 2000u, 500u)

KCOMPONENT_IMPORT_FN(arch_irq, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(arch_irq, "kernel.arch.apic", apic)
KCOMPONENT_DEPENDS(arch_irq, "kernel.time.pit", time_pit)
KCOMPONENT_DEPENDS(arch_irq, "kernel.arch.smp", smp)

static int32_t arch_irq_init(void)
{
    uint32_t bsp_id = 0;
    if (bsp_cpu != nullptr)
        bsp_id = bsp_cpu->lapic_id;
    /* 顺序保证：kernel.time.pit 已注册 IRQ0 handler、SMP 已确定 BSP LAPIC id，
       此处才解除屏蔽；PIC 早已由 arch.apic 全屏蔽，不存在 legacy 竞争。 */
    IOAPIC::RemapIRQ(bsp_id, 0, 32, false);
    kprintf_("[comp.arch.irq] init: IRQ0/GSI0 -> vec32 unmasked (bsp=%u)\n",
             (unsigned)bsp_id);
    return KMOD_OK;
}

static void arch_irq_exit(void)
{
    /* IRQ 路由不可逆，无需撤销（审计即可）。 */
}

KCOMPONENT_END(arch_irq)
