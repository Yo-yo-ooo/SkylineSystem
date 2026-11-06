//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_klib_v1.cpp —— 示例 .ko：Tier0 共享服务（klib/sync）消费者
 * ----------------------------------------------------------------------------
 * 名称 vendor.demo.klib / 1.0.0 / TIER_IMPL
 * flags: KMOD_MF_UNLOADABLE（非 BOOT：由 KMOD_SELFTEST 的 shared-klib 用例加载）
 * caps : LOG
 *
 * 演示**两种**共享方式同时可用：
 *   1) 直接符号：strlen/strcmp/snprintf_ 是普通 ELF 未定义引用，加载期由内核
 *      .kout 导出表按名字解析（模块源码不需要 .in 声明，覆盖 `kernel.klib.v1`
 *      的直接符号子集）。
 *   2) 契约 ops：KMOD_IMPORT_KLIB/SYNC 导入 kernel.klib.v1 / kernel.sync.v1，
 *      治理层校验 contract/abi/expect_size；覆盖 C++ 链接工具（bitmap/qsort/
 *      RND/中断状态）。
 *
 * init 中全部自检通过后写导出数据 klib_selfcheck_magic（selftest 读取并回读
 * 校验），任一失败写 0。magic 同时编码 v1 以便未来扩展不影响旧断言。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_services.h>

#include <stdint.h>
#include <stddef.h>

/* 直接符号共享：C 链接 klib 函数（不声明 .in；加载器按名字解析） */
extern "C" size_t strlen(const char *s);
extern "C" int32_t strcmp(const char *a, const char *b);
extern "C" int32_t snprintf_(char *buf, size_t n, const char *fmt, ...);

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1")
KMOD_IMPORT_KLIB(klib);
KMOD_IMPORT_SYNC(sync);

#define DEMO_KLIB_MAGIC 0x4B4C49425F5631ull   /* "KLIB_V1" */

static uint64_t klib_selfcheck_magic = 0u;

static int32_t klib_selfcheck(void)
{
    if (!klib || !sync)
        return 0;
    if (klib->abi_version != KMOD_KLIB_ABI_V1 ||
        klib->ops_size != (uint32_t)sizeof(kmod_klib_ops_t))
        return 0;
    if (sync->abi_version != KMOD_SYNC_ABI_V1 ||
        sync->ops_size != (uint32_t)sizeof(kmod_sync_ops_t))
        return 0;

    /* ---- 1) 直接符号路径 ---- */
    if (strlen("kernel.klib") != 11u)
        return 0;
    if (strcmp("klib", "klib") != 0 || strcmp("a", "b") == 0)
        return 0;
    char buf[48];
    if (snprintf_(buf, sizeof(buf), "%s-%d", "v1", 11) <= 0)
        return 0;
    if (strlen(buf) != 5u)   /* "v1-11" */
        return 0;

    /* ---- 2) 契约 ops 路径 ---- */
    if (klib->strlen("ops") != 3u || klib->strcmp(buf, "v1-11") != 0)
        return 0;

    uint8_t bitmap[4] = {0, 0, 0, 0};
    klib->bitmap_set(bitmap, 3u);
    if (!klib->bitmap_get(bitmap, 3u) || klib->bitmap_get(bitmap, 4u))
        return 0;
    klib->bitmap_clear(bitmap, 3u);
    if (klib->bitmap_get(bitmap, 3u))
        return 0;

    int32_t nums[3] = {3, 1, 2};
    klib->qsort(nums, 3u, sizeof(int32_t), [](const void *a, const void *b) -> int32_t {
        int32_t x = *(const int32_t *)a;
        int32_t y = *(const int32_t *)b;
        return (x > y) - (x < y);
    });
    if (nums[0] != 1 || nums[1] != 2 || nums[2] != 3)
        return 0;

    if (klib->kld32((const uint8_t *)"\x01\x02\x03\x04") != 0x04030201u)
        return 0;
    uint64_t r = klib->random_range(10u, 20u);
    if (r < 10u || r > 20u)
        return 0;

    int32_t lock = 0;
    sync->spinlock_lock(&lock);
    sync->spinlock_unlock(&lock);
    uint64_t flags = sync->irq_save();
    int32_t st = sync->irq_state();
    sync->irq_restore(flags);
    (void)st;

    if (!klib->panic)
        return 0;
    return 1;
}

static int32_t demo_klib_init(void)
{
    if (!klib_selfcheck()) {
        klib_selfcheck_magic = 0u;
        kprintf_("[demo.klib] shared klib selfcheck FAILED\n");
        return KMOD_ERR_INVAL;
    }
    klib_selfcheck_magic = DEMO_KLIB_MAGIC;
    kprintf_("[demo.klib] shared services ok: klib.v1 + sync.v1 + direct symbols\n");
    return KMOD_OK;
}

static void demo_klib_exit(void)
{
    klib_selfcheck_magic = 0u;
    kprintf_("[demo.klib] exit (klib/sync imports released)\n");
}

/* 读回点：selftest 通过模块导出表读取该地址，验证 init 的自检结果。 */
KMOD_EXPORT_DATA(klib_selfcheck_magic, "vendor.demo.klib.v1", 1u,
                 (uint32_t)sizeof(uint64_t))

KMOD_MANIFEST("vendor.demo.klib", "1.0.0", KMOD_TIER_IMPL,
              KMOD_MF_UNLOADABLE,
              (uint64_t)(KMOD_CAP_LOG),
              demo_klib_init, demo_klib_exit);
