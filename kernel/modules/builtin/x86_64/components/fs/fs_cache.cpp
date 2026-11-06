//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * fs_cache.cpp —— 内建组件 kernel.fs.cache (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init() 原 line 177-196 的整段：
 *   bsp_cpu->file_cache = kmalloc(sizeof(file_cache_cpu_t));
 *   file_cache_cpu_init(bsp_cpu->file_cache, bsp_cpu->id,
 *                       file_cache_writeback_callback);
 *   // 上限 = 总内存/32，每核钳制 [512 KiB, 4 MiB]；hard = soft*2
 *   file_cache_set_limits(bsp_cpu->file_cache, per_cpu, per_cpu * 2);
 *
 * 为什么必须在 SMP 之前：smp_init() 给每个 AP 建 file_cache 时参考 BSP 的
 * 预算策略；旧 init.cpp 同样顺序（fs cache line 177-196 -> SMP line 197）。
 * 因此 arch_smp 组件显式依赖本组件。
 *
 * 导出契约 kernel.fscache.v1（v1 内联固定布局，只读观测）：
 *   abi/ops_size + soft_limit() + hard_limit()
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <fs/fc.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>   /* cpu_t */

#define KMOD_CONTRACT_FSCACHE_V1 "kernel.fscache.v1"
#define KMOD_FSCACHE_ABI_V1      1u

/* bsp_cpu 定义在 smp.cpp（Tier0），与旧 init.cpp 的声明方式一致。 */
extern cpu_t *bsp_cpu;
extern "C" int32_t file_cache_writeback_callback(
    uint64_t file_id, const uint8_t *key, uint32_t key_len, void *data,
    size_t data_len);

static int32_t fs_cache_init(void);
static void fs_cache_exit(void);

KCOMPONENT_BEGIN(fs_cache, "kernel.fs.cache", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_LOG), 0,
                 "BSP file cache init + per-CPU soft/hard limits (mem/32, clamp 512K..4M)",
                 fs_cache_init, fs_cache_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(fs_cache, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")

namespace {

typedef struct kmod_fscache_ops {
    uint32_t abi_version;   /* = KMOD_FSCACHE_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_fscache_ops_t) */
    uint64_t (*soft_limit)(void);
    uint64_t (*hard_limit)(void);
} kmod_fscache_ops_t;

uint64_t g_soft_limit = 0u;
uint64_t g_hard_limit = 0u;

uint64_t fscache_soft_limit(void)
{
    return g_soft_limit;
}

uint64_t fscache_hard_limit(void)
{
    return g_hard_limit;
}

const kmod_fscache_ops_t g_fscache_ops = {
    KMOD_FSCACHE_ABI_V1,
    (uint32_t)sizeof(kmod_fscache_ops_t),
    fscache_soft_limit,
    fscache_hard_limit,
};

} /* namespace */

static int32_t fs_cache_init(void)
{
    if (!bsp_cpu) {
        kprintf_("[comp.fs.cache] init FAILED: bsp_cpu not set (Tier0 ordering)\n");
        return KMOD_ERR_STATE;
    }
    bsp_cpu->file_cache = (file_cache_cpu_t *)kmalloc(sizeof(file_cache_cpu_t));
    if (!bsp_cpu->file_cache) {
        kprintf_("[comp.fs.cache] init FAILED: kmalloc(file_cache_cpu_t)\n");
        return KMOD_ERR_NOMEM;
    }
    file_cache_cpu_init(bsp_cpu->file_cache, bsp_cpu->id,
                        file_cache_writeback_callback);

    /* 文件缓存必须有上限（详见旧 init.cpp 注释：soft_limit==0 会无界缓存）。 */
    uint64_t free_bytes = PMM::FreePages() << 12;
    uint64_t per_cpu = free_bytes / 32u;
    if (per_cpu < (512u << 10))
        per_cpu = (512u << 10);
    if (per_cpu > (4u << 20))
        per_cpu = (4u << 20);
    file_cache_set_limits(bsp_cpu->file_cache, per_cpu, per_cpu * 2u);
    g_soft_limit = per_cpu;
    g_hard_limit = per_cpu * 2u;
    kprintf_("[comp.fs.cache] init: bsp file_cache soft=%llu KiB hard=%llu KiB\n",
             (unsigned long long)(per_cpu >> 10),
             (unsigned long long)(g_hard_limit >> 10));
    return KMOD_OK;
}

static void fs_cache_exit(void)
{
    /* 缓存本身由 Tier0 文件系统路径持有；组件只负责预算策略，退出不清数据。 */
    kprintf_("[comp.fs.cache] exit: limits kept (cache owned by fs path)\n");
}

KCOMPONENT_EXPORT_CONTRACT(fs_cache, KMOD_CONTRACT_FSCACHE_V1, g_fscache_ops,
                           KMOD_FSCACHE_ABI_V1)
KCOMPONENT_END(fs_cache)
