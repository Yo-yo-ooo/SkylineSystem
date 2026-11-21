//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_inject.cpp —— DynModule 注入示范（DLL 式）
 * ----------------------------------------------------------------------------
 * 1. 通过 .in 依赖 vendor.demo.late：加载本模块时，治理层自动注入 late
 *    （KMOD_MF_DEMAND，平时不在启动图里）；
 * 2. init 里用 kernel.dynlink.v1 的 inject() 再注入 vendor.demo.hello，
 *    并用 resolve() 做 GetProcAddress 式符号解析；
 * 3. exit 里 eject() 释放 hello 的 PIN —— 若 hello 没有其他消费者且可卸载，
 *    会真正卸载。
 *
 * 自检：KMOD_SELFTEST 的 dynamic-inject 用例按名注入本模块，检查 late 被
 * 自动拉起、magic 已设置，然后 eject 两级模块。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 数据导入：late 模块导出的 uint64_t magic。 */
KMOD_IMPORT(late_magic, const uint64_t *, "vendor.demo.late", "late_magic",
            "vendor.demo.late.v1");

/* 统一 DynModule 注入服务（LoadLibrary/GetProcAddress）。 */
KMOD_IMPORT_DYNLINK(dynlink);

#define LATE_MAGIC 0x4C4154455F5631ull
#define INJECT_MAGIC 0x494E4A45435431ull /* "INJECT1" */

static uint64_t inject_magic = 0u;

static int32_t inject_init(void)
{
    if (!late_magic || *late_magic != LATE_MAGIC)
        return KMOD_ERR_CONTRACT;
    if (!dynlink)
        return KMOD_ERR_UNRESOLVED;

    int32_t rc = dynlink->inject("vendor.demo.hello",
                                 KMOD_INJ_PIN | KMOD_INJ_SILENT);
    if (rc != KMOD_OK) {
        kprintf_("[demo.inject] inject hello failed rc=%d\n", (int)rc);
        return rc;
    }
    void *sym = dynlink->resolve("vendor.demo.hello", "hello_fn");
    if (!sym) {
        (void)dynlink->eject("vendor.demo.hello");
        return KMOD_ERR_NOTFOUND;
    }

    inject_magic = INJECT_MAGIC;
    kprintf_("[demo.inject] ready: late auto-injected, hello sym=%p "
             "magic=0x%llx\n", sym, (unsigned long long)inject_magic);
    return KMOD_OK;
}

static void inject_exit(void)
{
    if (dynlink)
        (void)dynlink->eject("vendor.demo.hello");
    inject_magic = 0u;
    kprintf_("[demo.inject] ejected\n");
}

KMOD_EXPORT_DATA(inject_magic, "vendor.demo.inject.v1", 1u,
                 sizeof(inject_magic));

KMOD_MANIFEST_FULL("vendor.demo.inject", "1.0.0", KMOD_TIER_DYN,
                   (uint32_t)(KMOD_MF_UNLOADABLE | KMOD_MF_DEMAND),
                   (uint64_t)(KMOD_CAP_LOG | KMOD_CAP_MODULE_LOAD |
                              KMOD_CAP_MODULE_UNLOAD),
                   0, "DynModule injection demo (LoadLibrary/GetProcAddress)",
                   inject_init, inject_exit, 0, 0, 0u, 0u);
