//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_late.cpp —— DynModule 按需注入的 provider（非 BOOT）
 * ----------------------------------------------------------------------------
 * 只被 demo_inject.ko 通过私有契约 vendor.demo.late.v1 依赖；启动图不加载它。
 * KdllInject("vendor.demo.inject") 时，治理层按 .in 的 provider 自动把它
 * 一起注入 —— 这就是 DLL 式"依赖随 LoadLibrary 拉起"的最小证据。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

#define LATE_MAGIC 0x4C4154455F5631ull /* "LATE_V1" */

static uint64_t late_magic = 0u;

static int32_t late_init(void)
{
    late_magic = LATE_MAGIC;
    kprintf_("[demo.late] injected (magic=0x%llx)\n",
             (unsigned long long)late_magic);
    return KMOD_OK;
}

static void late_exit(void)
{
    late_magic = 0u;
    kprintf_("[demo.late] ejected\n");
}

KMOD_EXPORT_DATA(late_magic, "vendor.demo.late.v1", 1u, sizeof(late_magic));

KMOD_MANIFEST_FULL("vendor.demo.late", "1.0.0", KMOD_TIER_DYN,
                   (uint32_t)(KMOD_MF_UNLOADABLE | KMOD_MF_DEMAND),
                   KMOD_CAP_LOG, 0, "DynModule injected on demand by demo_inject",
                   late_init, late_exit, 0, 0, 0u, 0u);
