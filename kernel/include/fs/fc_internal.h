// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* fc_internal.h — fc.cpp(核心生命周期/API) 与 fc_idle.cpp(后台维护/fsync)
   共享的内部声明 (拆分自 fc.cpp) */
#pragma once

#include <fs/fc.h>

/* 全局注册表 */
extern file_cache_cpu_t *g_fc_cpus[FC_MAX_CPUS];
extern uint32_t g_num_active_cpus;

/* Oscillate 池 */
extern void fc_oscillate_free(file_cache_cpu_t *s, fc_oscillate_t *osc);

/* 统计与启发式 */
typedef struct {
    uint64_t count;
    uint64_t sum_freq;
    uint64_t sum_io;
} fc_stats_ctx_t;
extern int  fc_collect_stats_cb(void *data, const uint8_t *key, uint32_t key_len, void *value);
extern void fc_update_averages_internal(file_cache_cpu_t *s);
extern bool file_cache_should_evict(file_cache_cpu_t *s, file_cache_entry_t *cur);

/* LRU 与内存管理 */
extern bool fc_bad_ptr(const void *p);
extern void fc_lru_remove(file_cache_cpu_t *s, file_cache_entry_t *e);
extern void fc_entry_free(file_cache_entry_t *e);
extern void fc_update_oscillate(file_cache_cpu_t *s, file_cache_entry_t *v);
