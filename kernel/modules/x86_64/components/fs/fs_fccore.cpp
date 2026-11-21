//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* fs_fccore.cpp —— kernel.fs.fccore：文件缓存核心/后台维护独立 .ko。
 *
 * fc.cpp + fc_idle.cpp 从 Tier0 移出；内核只保留 fc_hook.cpp 的稳定跳板。
 * fccore 不依赖 sched/dev，因此可先于 fs.cache/smp 加载，避免
 * fs.cache -> fs.vfs -> dev.core -> sched.core -> smp 的依赖环。 */
#include <kmod/kmod_component.h>
#include <fs/fc.h>
#include <fs/fc_internal.h>

static int32_t fs_fccore_init(void);

KCOMPONENT_BEGIN(fs_fccore, "kernel.fs.fccore", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_LOG), 0,
                 "file cache core + idle maintenance",
                 fs_fccore_init, nullptr, 0, 0, 2000u, 200u)

extern "C" void fc_set_hooks(int32_t (*fsync_fn)(file_cache_cpu_t *, uint64_t),
                             void (*tick_fn)(file_cache_cpu_t *),
                             void (*idle_fn)(file_cache_cpu_t *),
                             void (*cpu_init_fn)(file_cache_cpu_t *, uint32_t,
                                 int32_t (*)(uint64_t, const uint8_t *, uint32_t, void *, size_t)),
                             void (*limits_fn)(file_cache_cpu_t *, uint64_t, uint64_t));

static int32_t fs_fccore_init(void)
{
    fc_set_hooks(&file_cache_fsync, &file_cache_tick, &file_cache_idle_handler,
                 &file_cache_cpu_init, &file_cache_set_limits);
    return KMOD_OK;
}

#define FC_EXPORT(tag, name, expr) \
    KCOMPONENT_EXPORT_RAW(fs_fccore, tag, name, expr, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u)

FC_EXPORT(fc_next_uid, "fc_next_file_uid", &fc_next_file_uid);
FC_EXPORT(fc_check_load, "file_cache_check_load", &file_cache_check_load);
FC_EXPORT(fc_cpu_destroy, "file_cache_cpu_destroy", &file_cache_cpu_destroy);
FC_EXPORT(fc_cpu_init, "file_cache_cpu_init", &file_cache_cpu_init);
FC_EXPORT(fc_fsync, "file_cache_fsync", &file_cache_fsync);
FC_EXPORT(fc_get, "file_cache_get", &file_cache_get);
FC_EXPORT(fc_get_block, "file_cache_get_block", &file_cache_get_block);
FC_EXPORT(fc_idle, "file_cache_idle_handler", &file_cache_idle_handler);
FC_EXPORT(fc_invalidate, "file_cache_invalidate", &file_cache_invalidate);
FC_EXPORT(fc_invalidate_block, "file_cache_invalidate_block", &file_cache_invalidate_block);
FC_EXPORT(fc_invalidate_file, "file_cache_invalidate_file", &file_cache_invalidate_file);
FC_EXPORT(fc_promote, "file_cache_promote", &file_cache_promote);
FC_EXPORT(fc_promote_block, "file_cache_promote_block", &file_cache_promote_block);
FC_EXPORT(fc_put, "file_cache_put", &file_cache_put);
FC_EXPORT(fc_readahead, "file_cache_readahead", &file_cache_readahead);
FC_EXPORT(fc_record_io, "file_cache_record_io", &file_cache_record_io);
FC_EXPORT(fc_set_limits, "file_cache_set_limits", &file_cache_set_limits);
FC_EXPORT(fc_tick, "file_cache_tick", &file_cache_tick);
FC_EXPORT(fc_entry_free, "_Z13fc_entry_freeP16file_cache_entry", &fc_entry_free);
FC_EXPORT(fc_lru_remove, "_Z13fc_lru_removeP14file_cache_cpuP16file_cache_entry", &fc_lru_remove);
FC_EXPORT(fc_oscillate_free, "_Z17fc_oscillate_freeP14file_cache_cpuP12fc_oscillate", &fc_oscillate_free);
FC_EXPORT(fc_update_oscillate, "_Z19fc_update_oscillateP14file_cache_cpuP16file_cache_entry", &fc_update_oscillate);
FC_EXPORT(fc_should_evict, "_Z23file_cache_should_evictP14file_cache_cpuP16file_cache_entry", &file_cache_should_evict);
FC_EXPORT(fc_update_averages, "_Z27fc_update_averages_internalP14file_cache_cpu", &fc_update_averages_internal);

KCOMPONENT_END(fs_fccore)
