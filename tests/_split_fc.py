#!/usr/bin/env python3
import re
p = '/mnt/c/ZSY/SkylineSystem/kernel/src/fs/fc.cpp'
lines = open(p, encoding='utf-8').read().split('\n')

# 共享项去 static (定义留在 fc.cpp, 声明进 fc_internal.h)
subs = [
    ('static file_cache_cpu_t *g_fc_cpus[FC_MAX_CPUS];', 'file_cache_cpu_t *g_fc_cpus[FC_MAX_CPUS];'),
    ('static uint32_t g_num_active_cpus = 0;', 'uint32_t g_num_active_cpus = 0;'),
    ('static void fc_oscillate_free(', 'void fc_oscillate_free('),
    ('static int fc_collect_stats_cb(', 'int fc_collect_stats_cb('),
    ('static void fc_update_averages_internal(', 'void fc_update_averages_internal('),
    ('static bool file_cache_should_evict(', 'bool file_cache_should_evict('),
    ('static inline bool fc_bad_ptr(', 'bool fc_bad_ptr('),
    ('static inline void fc_lru_remove(', 'void fc_lru_remove('),
    ('static inline void fc_entry_free(', 'void fc_entry_free('),
    ('static void fc_update_oscillate(', 'void fc_update_oscillate('),
]
body = '\n'.join(lines)
for a, b in subs:
    assert a in body, a
    body = body.replace(a, b, 1)

# 切出 idle/后台段 (行 1280 起, 0-based 1279)
parts = body.split('\n')
idle_body = '\n'.join(parts[1279:])
core_body = '\n'.join(parts[:1279])

idle_src = '''// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// fc_idle.cpp - 文件缓存后台维护与 fsync (拆分自 fc.cpp)
//   idle_handler / tick / 写回冲刷 / fsync / 淘汰扫描
#include <fs/fc.h>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include <pdef.h>

#include "fc_internal.h"

extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern void  spinlock_lock(spinlock_t* lock);
extern void  spinlock_unlock(spinlock_t* lock);

''' + idle_body + '\n'

# fc.cpp 头部插入内部头
anchor = '#include <pdef.h>'
core_body = core_body.replace(anchor, anchor + '\n#include "fc_internal.h"', 1)

open('/mnt/c/ZSY/SkylineSystem/kernel/src/fs/fc_idle.cpp', 'w', encoding='utf-8').write(idle_src)
open(p, 'w', encoding='utf-8').write(core_body)
print('split done')
