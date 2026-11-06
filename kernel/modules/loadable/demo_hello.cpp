//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_hello.cpp —— kmod 示例：Tier2 实现模块 + 私有契约提供者（BOOT）
 * ----------------------------------------------------------------------------
 * 只用模块作者契约头 <kmod/kmod_module.h>（不得 include 内核内部头）：
 *   - manifest:         "vendor.demo.hello" v1.0.0 / TIER_IMPL / MF_BOOT / CAP_LOG
 *   - 导入:             kernel.kprintf_ ("kernel", "kprintf_", kernel.log.v1)
 *   - 导出函数:         hello_fn  (contract "vendor.demo.hello.v1", abi 1)
 *   - 导出数据:         hello_exit_count（退出计数，自检/观测用）
 *
 * 消费者 demo_greeter.cpp 以 KMOD_IMPORT 函数指针 typedef 导入 hello_fn，
 * 调用 hello_fn(1) 必须得到 1001 —— 私有契约完全由两个模块约定，内核只回填。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

/* 内核日志出口（Tier0 固定契约 kernel.log.v1）；名字 kprintf_ 由内核导出表提供。 */
KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 导出函数：1000 + 入参。签名由提供者/消费者双方私下约定（本 demo 为 int32）。 */
static int32_t hello_fn(int32_t x)
{
    return (int32_t)(1000 + x);
}

/* 导出数据：退出回调调用次数（应恒为 0/1；失败回滚时用于观测是否误调 exit）。 */
static uint64_t hello_exit_count = 0;

static int32_t hello_init(void);
static void hello_exit(void);

static int32_t hello_init(void)
{
    /* 自检：导出函数本模块内可调用，且语义固定 1000+x */
    if (hello_fn(1) != 1001)
        return KMOD_ERR_INVAL;
    kprintf_("[demo.hello] init\n");
    return KMOD_OK;
}

static void hello_exit(void)
{
    hello_exit_count++;
    kprintf_("[demo.hello] exit\n");
}

KMOD_EXPORT_FN(hello_fn, "vendor.demo.hello.v1", 1u);
KMOD_EXPORT_DATA(hello_exit_count, "vendor.demo.hello.v1", 1u,
                 sizeof(hello_exit_count));

KMOD_MANIFEST_FULL("vendor.demo.hello", "1.0.0", KMOD_TIER_IMPL,
                   (uint32_t)(KMOD_MF_BOOT | KMOD_MF_UNLOADABLE),
                   KMOD_CAP_LOG, 0,
                   "demo: hello provider (boot)", hello_init, hello_exit,
                   0, 0, 0u, 0u);
