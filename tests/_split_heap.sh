#!/bin/bash
# 拆分 kernel/src/mem/heap.cpp → slab.cpp + slub.cpp + heap.cpp(kmalloc 胶水)
set -e
cd /mnt/c/ZSY/SkylineSystem/kernel/src/mem || exit 1
cp heap.cpp /tmp/heap_backup.cpp

INCLUDES='#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>

#include "heap_internal.h"'

SPDX='//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only'

# ---- slab.cpp: 行 1-59 (SPDX+includes+defines+globals, 跳过 60-89 已迁入 heap_internal.h) + 90-783 ----
{
  echo "$SPDX"
  sed -n '3,9p' heap.cpp
  echo ''
  echo '#include "heap_internal.h"'
  echo ''
  sed -n '12,59p' heap.cpp
  echo ''
  sed -n '90,783p' heap.cpp
} > slab.cpp

# ---- slub.cpp: includes + 785-1228 (SLUB 结构/命名空间, 含 kmalloc 融合) ----
{
  echo "$SPDX"
  sed -n '3,9p' heap.cpp
  echo ''
  echo '#include "heap_internal.h"'
  echo ''
  sed -n '785,1228p' heap.cpp
} > slub.cpp

# ---- heap.cpp: includes + 1229-1344 (全局 kmalloc/kfree/krealloc/kcalloc 胶水) ----
{
  echo "$SPDX"
  sed -n '3,9p' heap.cpp
  echo ''
  echo '#include "heap_internal.h"'
  echo ''
  sed -n '1229,1344p' heap.cpp
} > heap_new.cpp
mv heap_new.cpp heap.cpp

echo "=== 行数 ==="
wc -l slab.cpp slub.cpp heap.cpp heap_internal.h
