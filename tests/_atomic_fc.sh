#!/bin/bash
# 将 fc.cpp 的 total_entries 全部访问转原子 (TSAN 干净 + 跨核统计一致)
F=/mnt/c/ZSY/SkylineSystem/kernel/src/fs/fc.cpp
sed -i \
 -e 's/\([A-Za-z_][A-Za-z0-9_]*\)->total_entries--/__atomic_fetch_sub(\&\1->total_entries, 1, __ATOMIC_RELAXED)/g' \
 -e 's/\([A-Za-z_][A-Za-z0-9_]*\)->total_entries++/__atomic_fetch_add(\&\1->total_entries, 1, __ATOMIC_RELAXED)/g' \
 -e 's/s->total_entries = 0;/__atomic_store_n(\&s->total_entries, 0, __ATOMIC_RELAXED);/g' \
 -e 's/\([A-Za-z_][A-Za-z0-9_]*\)->total_entries \/ \([0-9][0-9]*\)/__atomic_load_n(\&\1->total_entries, __ATOMIC_RELAXED) \/ \2/g' \
 -e 's/scanned < src->total_entries/scanned < __atomic_load_n(\&src->total_entries, __ATOMIC_RELAXED)/g' \
 "$F"
grep -n 'total_entries' "$F" | head -30
