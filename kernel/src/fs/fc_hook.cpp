//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* fc_hook.cpp —— file cache 的 Tier0 跳板。
 *
 * fc.cpp/fc_idle.cpp 已迁入 kernel.fs.vfs.ko；PIT 的 tick 与 fd.cpp 的
 * fsync 必须在内核侧有稳定符号，未注册时 no-op。 */
#include <fs/fc.h>

static int32_t (*g_fsync)(file_cache_cpu_t *, uint64_t) = nullptr;
static void (*g_tick)(file_cache_cpu_t *) = nullptr;
static void (*g_idle)(file_cache_cpu_t *) = nullptr;
static void (*g_cpu_init)(file_cache_cpu_t *, uint32_t,
                          int32_t (*)(uint64_t, const uint8_t *, uint32_t, void *, size_t)) = nullptr;
static void (*g_limits)(file_cache_cpu_t *, uint64_t, uint64_t) = nullptr;

extern "C" void fc_set_hooks(int32_t (*fsync_fn)(file_cache_cpu_t *, uint64_t),
                             void (*tick_fn)(file_cache_cpu_t *),
                             void (*idle_fn)(file_cache_cpu_t *),
                             void (*cpu_init_fn)(file_cache_cpu_t *, uint32_t,
                                 int32_t (*)(uint64_t, const uint8_t *, uint32_t, void *, size_t)),
                             void (*limits_fn)(file_cache_cpu_t *, uint64_t, uint64_t))
{
    __atomic_store_n(&g_fsync, fsync_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tick, tick_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_idle, idle_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cpu_init, cpu_init_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_limits, limits_fn, __ATOMIC_RELEASE);
}

int32_t file_cache_fsync_hook(file_cache_cpu_t *s, uint64_t file_id)
{
    auto fn = __atomic_load_n(&g_fsync, __ATOMIC_ACQUIRE);
    return fn ? fn(s, file_id) : 0;
}

void file_cache_tick_hook(file_cache_cpu_t *s)
{
    auto fn = __atomic_load_n(&g_tick, __ATOMIC_ACQUIRE);
    if (fn)
        fn(s);
}

void file_cache_idle_handler_hook(file_cache_cpu_t *s)
{
    auto fn = __atomic_load_n(&g_idle, __ATOMIC_ACQUIRE);
    if (fn)
        fn(s);
}

void file_cache_cpu_init_hook(file_cache_cpu_t *s, uint32_t cpu_id,
                              int32_t (*writeback_cb)(uint64_t, const uint8_t *, uint32_t, void *, size_t))
{
    auto fn = __atomic_load_n(&g_cpu_init, __ATOMIC_ACQUIRE);
    if (fn)
        fn(s, cpu_id, writeback_cb);
}

void file_cache_set_limits_hook(file_cache_cpu_t *s, uint64_t soft_limit, uint64_t hard_limit)
{
    auto fn = __atomic_load_n(&g_limits, __ATOMIC_ACQUIRE);
    if (fn)
        fn(s, soft_limit, hard_limit);
}
