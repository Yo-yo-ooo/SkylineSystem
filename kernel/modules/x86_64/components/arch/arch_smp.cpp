//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * arch_smp.cpp —— 内建组件 kernel.arch.smp (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：InitFunc("SMP", smp_init());（原 line 197）
 *
 * 依赖（顺序敏感，全部显式声明）：
 *   acpi    —— MP 表/CPU 枚举来自 ACPI MADT
 *   apic    —— AP 启动 IPI / LAPIC 就绪
 *   time.pit/hpet —— AP 时钟与超时
 *   fs.cache —— SMP 初始化期间 AP 的 file_cache 预算已由 BSP 组件建立
 *             （旧 init.cpp 在 SMP 之前初始化 BSP 文件缓存，保持同一顺序）
 *
 * 导出契约 kernel.smp.v1（无独立头文件；v1 内联固定布局）：
 *   abi/ops_size + cpu_count() + bsp_lapic_id() + started()
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <arch/x86_64/smp/smp.h>

#define KMOD_CONTRACT_SMP_V1 "kernel.smp.v1"
#define KMOD_SMP_ABI_V1      1u

static int32_t arch_smp_init(void);
static void arch_smp_exit(void);

KCOMPONENT_BEGIN(arch_smp, "kernel.arch.smp", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_MEM_ALLOC |
                                          KMOD_CAP_LOG),
                 0, "SMP bring-up (APs online); exports cpu count / bsp id",
                 arch_smp_init, arch_smp_exit, 0, 0, 30000u, 2000u)

KCOMPONENT_IMPORT_FN(arch_smp, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(arch_smp, "kernel.arch.acpi", acpi)
KCOMPONENT_DEPENDS(arch_smp, "kernel.arch.apic", apic)
KCOMPONENT_DEPENDS(arch_smp, "kernel.time.pit", time_pit)
KCOMPONENT_DEPENDS(arch_smp, "kernel.time.hpet", time_hpet)
KCOMPONENT_DEPENDS(arch_smp, "kernel.fs.cache", fs_cache)

namespace {

typedef struct kmod_smp_ops {
    uint32_t abi_version;   /* = KMOD_SMP_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_smp_ops_t) */
    uint32_t (*cpu_count)(void);
    uint32_t (*bsp_lapic_id)(void);
    int32_t (*started)(void);
} kmod_smp_ops_t;

uint32_t smp_cpu_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_CPU; i++) {
        if (smp_cpu_list[i])
            n++;
    }
    return n;
}

uint32_t smp_bsp_lapic_id(void)
{
    if (smp_bsp_cpu < MAX_CPU && smp_cpu_list[smp_bsp_cpu])
        return smp_cpu_list[smp_bsp_cpu]->lapic_id;
    return 0;
}

int32_t smp_started_flag(void)
{
    return smp_started ? 1 : 0;
}

const kmod_smp_ops_t g_smp_ops = {
    KMOD_SMP_ABI_V1,
    (uint32_t)sizeof(kmod_smp_ops_t),
    smp_cpu_count,
    smp_bsp_lapic_id,
    smp_started_flag,
};

} /* namespace */

static int32_t arch_smp_init(void)
{
    smp_init();
    kprintf_("[comp.arch.smp] init: cpus=%u bsp_lapic_id=%u started=%d\n",
             (unsigned)smp_cpu_count(), (unsigned)smp_bsp_lapic_id(),
             (int)smp_started_flag());
    return KMOD_OK;
}

static void arch_smp_exit(void)
{
    /* AP 下线不支持（Tier0 机制），旧实现也无 teardown；这里只记日志。 */
    kprintf_("[comp.arch.smp] exit: APs stay online (no hot-unplug)\n");
}

KCOMPONENT_EXPORT_CONTRACT(arch_smp, KMOD_CONTRACT_SMP_V1, g_smp_ops,
                           KMOD_SMP_ABI_V1)
KCOMPONENT_END(arch_smp)
