//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_cycle_a.cpp —— kmod 示例：依赖环 A→B→A 的一侧（负向用例，非 BOOT）
 * ----------------------------------------------------------------------------
 * A 通过 KMOD_DEPENDS 声明模块级依赖 "vendor.demo.cycleb"（纯图边，无符号
 * 回填）；cycleb 又依赖 A。治理层必须在拓扑排序阶段检测出环并拒绝整批
 * 加载（KMOD_ERR_CYCLE），两者的 init 都不得执行。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 纯依赖边：provider = b，tag = cycleb（仅用于生成变量名）。 */
KMOD_DEPENDS("vendor.demo.cycleb", cycleb);

static int32_t cycle_a_init(void);
static void cycle_a_exit(void);

static int32_t cycle_a_init(void)
{
    kprintf_("[demo.cyclea] init SHOULD NOT HAPPEN (cycle)\n");
    return KMOD_OK;
}

static void cycle_a_exit(void)
{
}

KMOD_MANIFEST_FULL("vendor.demo.cyclea", "1.0.0", KMOD_TIER_IMPL, 0,
                   KMOD_CAP_LOG, 0,
                   "demo: dependency cycle A -> B", cycle_a_init,
                   cycle_a_exit, 0, 0, 0u, 0u);
