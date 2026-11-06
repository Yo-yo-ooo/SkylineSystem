//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_badcap.cpp —— kmod 示例：声明 Tier0 保留能力位 → 必须被治理层拒绝
 * ----------------------------------------------------------------------------
 * required_caps 含 KMOD_CAP_RESERVED_CORE(bit63)：能力策略在 ELF 加载/init
 * 之前就必须拒绝（KMOD_ERR_CAP），因此本模块的 init 永远不会被执行。
 * init 故意返回 KMOD_OK：如果某天它真的跑起来了，说明能力治理被绕过。
 * 不导入任何内核符号（尽可能少依赖，保证即使加载链有问题，拒绝原因也只会
 * 是能力而非未解析符号）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

static int32_t badcap_init(void);
static void badcap_exit(void);

static int32_t badcap_init(void)
{
    return KMOD_OK;   /* 不应被执行；执行即治理失效 */
}

static void badcap_exit(void)
{
}

KMOD_MANIFEST_FULL("vendor.demo.badcap", "1.0.0", KMOD_TIER_IMPL, 0,
                   KMOD_CAP_RESERVED_CORE, 0,
                   "demo: reserved-core capability must be rejected",
                   badcap_init, badcap_exit, 0, 0, 0u, 0u);
