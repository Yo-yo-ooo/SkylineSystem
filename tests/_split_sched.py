#!/usr/bin/env python3
import re
p = '/mnt/c/ZSY/SkylineSystem/kernel/src/arch/x86_64/schedule/sched.cpp'
lines = open(p, encoding='utf-8').read().split('\n')

# 切出: 行 36-116 (RIP 常量/结构/数组/TSC钩子/SMP游标) 与 136-515 (rdtsc/dyn_ctx/量子层/riprate_update)
# (0-based: 35..115 与 135..514)。128-133 (need_resched/yield 标志) 与 118-126 (prio 表) 留在核心。
rip_lines = lines[35:116] + [''] + lines[135:515]

def to_rip(lines_):
    s = '\n'.join(lines_)
    s = s.replace('static rip_fast_weight_ctx rip_fast_weight[MAX_CPU];',
                  'rip_fast_weight_ctx rip_fast_weight[MAX_CPU];')
    s = s.replace('static rip_stats_ctx rip_stats[MAX_CPU];',
                  'rip_stats_ctx rip_stats[MAX_CPU];')
    s = s.replace('__attribute__((weak)) uint64_t sched_tsc_per_ms(void) { return 0; }',
                  '__attribute__((weak)) uint64_t sched_tsc_per_ms(void) { return 0; }')
    s = s.replace('static sched_steal_throttle per_cpu_steal_throttle[MAX_CPU];',
                  'sched_steal_throttle per_cpu_steal_throttle[MAX_CPU];')
    s = s.replace('static sched_padded_u32 per_cpu_steal_cursor[MAX_CPU];',
                  'sched_padded_u32 per_cpu_steal_cursor[MAX_CPU];')
    s = s.replace('static dyn_adjust_ctx dyn_ctx[MAX_CPU];',
                  'dyn_adjust_ctx dyn_ctx[MAX_CPU];')
    s = s.replace('static void dynamic_adjust_quantum(', 'void dynamic_adjust_quantum(')
    s = s.replace('static inline uint64_t get_dynamic_quantum(', 'uint64_t get_dynamic_quantum(')
    s = s.replace('static inline uint64_t eligibility_capped_quantum(', 'uint64_t eligibility_capped_quantum(')
    s = s.replace('static inline void riprate_update(', 'void riprate_update(')
    # 删除已移入 sched_internal.h 的定义 (常量块/结构/数组/rdtsc 等)
    return s

rip_src = to_rip(rip_lines)

# 构建 sched_rip.cpp
header_inc = '''// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sched_rip.cpp - Rate-aware 反馈层 + 量子层 (拆分自 sched.cpp)
//   RIP 四层闭环 (信号/基线/控制/统计) + get_dynamic_quantum +
//   eligibility_capped_quantum (EEVDF eligible 预算截断)
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>
#include <atomic/atomic.h>
#include <fs/fc.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <pdef.h>

#include "sched_internal.h"

'''
open('/mnt/c/ZSY/SkylineSystem/kernel/src/arch/x86_64/schedule/sched_rip.cpp', 'w', encoding='utf-8').write(header_inc + rip_src + '\n')

# 从 sched.cpp 删除这两段, 并在 includes 后插入内部头
core = lines[:35] + lines[116:135] + lines[515:]
core_s = '\n'.join(core)
anchor = '#include <pdef.h>'
core_s = core_s.replace(anchor, anchor + '\n\n#include "sched_internal.h"', 1)
open(p, 'w', encoding='utf-8').write(core_s)
print('split done')
