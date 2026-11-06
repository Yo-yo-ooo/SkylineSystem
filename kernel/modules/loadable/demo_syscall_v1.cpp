//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_syscall_v1.cpp —— kmod 示例：syscall 实现 v1（非 BOOT，池内）
 * ----------------------------------------------------------------------------
 * 两类导出（契约固定为 Tier0 的 kernel.syscall.v1）：
 *   1) 动态命名空间槽 200：返回 KMOD_SYSCALL_PROBE_MAGIC | 0x11110000
 *      （动态槽只允许 [KMOD_SYSCALL_DYNAMIC_MIN, KMOD_SYSCALL_DYNAMIC_MAX]，
 *        本 demo 归还属于私有命名空间 "vendor.demo.syscall" 的实现）；
 *   2) 固定探针槽 KMOD_SYSCALL_PROBE(30) 的 override 实现：
 *      返回值格式固定 = KMOD_SYSCALL_PROBE_MAGIC | (代次 << 32)，v1 代次 1。
 * 实现函数签名必须与内核 dispatch ABI 一致（7 个参数，最后是 frame）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 动态命名空间槽 200 的实现（签名 = 固定 dispatch ABI）。 */
static uint64_t demo_syscall_fn(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5,
                                void *frame)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | 0x11110000ull;
}

/* 固定探针槽 KMOD_SYSCALL_PROBE(30) 的可替换实现：MAGIC | (代次<<32)。 */
static uint64_t demo_probe_fn(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5,
                              void *frame)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (1ull << 32);
}

static int32_t syscall_init(void);
static void syscall_exit(void);

static int32_t syscall_init(void)
{
    kprintf_("[demo.syscall] v1 active (slot 200 + probe override)\n");
    return KMOD_OK;
}

static void syscall_exit(void)
{
    kprintf_("[demo.syscall] exit\n");
}

KMOD_EXPORT_SYSCALL(200u, demo_syscall_fn);
KMOD_EXPORT_SYSCALL(KMOD_SYSCALL_PROBE, demo_probe_fn);
KMOD_EXPORT_NAMESPACE("vendor.demo.syscall", syscall_ns);

KMOD_MANIFEST_FULL("vendor.demo.syscall", "1.0.0", KMOD_TIER_IMPL,
                   (uint32_t)(KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE),
                   (uint64_t)(KMOD_CAP_SYSCALL_IMPL |
                              KMOD_CAP_SYSCALL_OVERRIDE |
                              KMOD_CAP_HOT_REPLACE | KMOD_CAP_LOG),
                   0, "demo: syscall impl v1 (dynamic slot + probe override)",
                   syscall_init, syscall_exit, 0, 0, 500u, 200u);
