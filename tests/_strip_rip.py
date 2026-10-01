#!/usr/bin/env python3
import re
p = '/mnt/c/ZSY/SkylineSystem/kernel/src/arch/x86_64/schedule/sched_rip.cpp'
s = open(p, encoding='utf-8').read()

# 删除已移入 sched_internal.h 的定义
blocks = [
    r'struct alignas\(64\) rip_fast_weight_ctx \{[^}]*\};',
    r'struct alignas\(64\) rip_stats_ctx \{[^}]*\};',
    r'struct alignas\(64\) sched_steal_throttle \{[^}]*\};',
    r'struct alignas\(64\) sched_padded_u32 \{[^}]*\};',
    r'static inline uint64_t sced_rdtsc\(\) \{[^}]*\}',
    r'struct alignas\(64\) dyn_adjust_ctx \{[^}]*\};',
]
for b in blocks:
    s2 = re.sub(b + r'\n?', '', s, count=1)
    assert s2 != s, b
    s = s2
open(p, 'w', encoding='utf-8').write(s)
print('stripped')
