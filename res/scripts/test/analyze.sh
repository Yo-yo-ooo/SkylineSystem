#!/bin/bash
# res/scripts/test/analyze.sh — 静态分析: clang --analyze 核心文件 (round 98)
# 迁移 (用户清理): 原根目录 _tmp_analyze.sh → 此处, 硬编码路径改为相对解析
cd "$(dirname "$0")/../../.."
ANALYZE_DIRS="kernel/src/klib kernel/src/mem kernel/src/fs"
OUT=/tmp/static_analysis.txt
echo "== clang --analyze core files ==" > $OUT
for f in \
  kernel/src/klib/klib.cpp \
  kernel/src/klib/mem.cpp \
  kernel/src/mem/slub.cpp \
  kernel/src/fs/fd.cpp \
  kernel/src/fs/fc.cpp \
  kernel/src/arch/x86_64/vmm/vmm.cpp \
  kernel/src/arch/x86_64/schedule/task.cpp \
  kernel/src/arch/x86_64/schedule/sched.cpp \
  kernel/src/arch/x86_64/schedule/mutex.cpp ; do
  echo "--- $f ---" >> $OUT
  clang++ --analyze -Xanalyzer -analyzer-output=text \
    -std=gnu++20 -nostdinc -ffreestanding -fno-exceptions -fno-rtti \
    -I kernel/include -I ablib -I kernel/include/arch/x86_64 \
    -I ablib/freestndchdrs/x86_64/include \
    $f >> $OUT 2>&1
done
grep -cE "warning:" $OUT
echo "ANALYSIS_DONE"
