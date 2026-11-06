//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_badinit.cpp —— kmod 示例：init 失败 → 事务回滚（负向用例，非 BOOT）
 * ----------------------------------------------------------------------------
 * 期望治理层行为：加载事务中 init 返回 KMOD_ERR_INVAL，
 *   - 模块最终状态 FAILED/REMOVED，不进入 READY；
 *   - 本次事务新加载的依赖模块全部逆序回滚；
 *   - exit_hook **不得**被调用（只有成功提交过的模块卸载才走 exit）。
 * 本文件把 exit 调用次数导出成数据对象，供人工/自检核对（正常应为 0）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* exit 若被误调用则 +1；预期恒为 0（rollback 不调用 exit）。 */
static uint32_t badinit_exit_calls = 0;

static int32_t badinit_init(void);
static void badinit_exit(void);

static int32_t badinit_init(void)
{
    kprintf_("[demo.badinit] init -> KMOD_ERR_INVAL (expected rejection)\n");
    return KMOD_ERR_INVAL;
}

static void badinit_exit(void)
{
    badinit_exit_calls++;
    kprintf_("[demo.badinit] exit SHOULD NOT BE CALLED\n");
}

KMOD_EXPORT_DATA(badinit_exit_calls, "vendor.demo.badinit.v1", 1u,
                 sizeof(badinit_exit_calls));

KMOD_MANIFEST_FULL("vendor.demo.badinit", "1.0.0", KMOD_TIER_IMPL, 0,
                   KMOD_CAP_LOG, 0,
                   "demo: init failure rollback case", badinit_init,
                   badinit_exit, 0, 0, 500u, 200u);
