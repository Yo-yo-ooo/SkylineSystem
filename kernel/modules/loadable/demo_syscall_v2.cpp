//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_syscall_v2.cpp —— kmod 示例：syscall 实现 v2（非 BOOT，池内）
 * ----------------------------------------------------------------------------
 * 与 v1 同模块名 "vendor.demo.syscall"，版本 2.0.0：
 *   - 动态槽 200 返回 MAGIC | 0x22220000
 *   - 固定探针槽 override 返回 MAGIC | (2ull << 32)（代次 2）
 * 用于热替换演示：v1 生效 → hotreplace → v2 生效，返回值机械可断言。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

static uint64_t demo_syscall_fn(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5,
                                void *frame)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | 0x22220000ull;
}

static uint64_t demo_probe_fn(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5,
                              void *frame)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (2ull << 32);
}

static int32_t syscall_init(void);
static void syscall_exit(void);

static int32_t syscall_init(void)
{
    kprintf_("[demo.syscall] v2 active (slot 200 + probe override)\n");
    return KMOD_OK;
}

static void syscall_exit(void)
{
    kprintf_("[demo.syscall] exit\n");
}

KMOD_EXPORT_SYSCALL(200u, demo_syscall_fn);
KMOD_EXPORT_SYSCALL(KMOD_SYSCALL_PROBE, demo_probe_fn);
KMOD_EXPORT_NAMESPACE("vendor.demo.syscall", syscall_ns);

KMOD_MANIFEST_FULL("vendor.demo.syscall", "2.0.0", KMOD_TIER_IMPL,
                   (uint32_t)(KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE),
                   (uint64_t)(KMOD_CAP_SYSCALL_IMPL |
                              KMOD_CAP_SYSCALL_OVERRIDE |
                              KMOD_CAP_HOT_REPLACE | KMOD_CAP_LOG),
                   0, "demo: syscall impl v2 (dynamic slot + probe override)",
                   syscall_init, syscall_exit, 0, 0, 500u, 200u);
