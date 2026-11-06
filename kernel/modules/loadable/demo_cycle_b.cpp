//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_cycle_b.cpp —— kmod 示例：依赖环 A→B→A 的另一侧（负向用例，非 BOOT）
 * ----------------------------------------------------------------------------
 * B 通过 KMOD_DEPENDS 依赖 "vendor.demo.cyclea"，与 cycle_a 构成环。
 * 治理层拓扑排序必须返回 KMOD_ERR_CYCLE 并整体拒绝（不执行任何 init）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 纯依赖边：provider = a，tag = cyclea（仅用于生成变量名）。 */
KMOD_DEPENDS("vendor.demo.cyclea", cyclea);

static int32_t cycle_b_init(void);
static void cycle_b_exit(void);

static int32_t cycle_b_init(void)
{
    kprintf_("[demo.cycleb] init SHOULD NOT HAPPEN (cycle)\n");
    return KMOD_OK;
}

static void cycle_b_exit(void)
{
}

KMOD_MANIFEST_FULL("vendor.demo.cycleb", "1.0.0", KMOD_TIER_IMPL, 0,
                   KMOD_CAP_LOG, 0,
                   "demo: dependency cycle B -> A", cycle_b_init,
                   cycle_b_exit, 0, 0, 0u, 0u);
