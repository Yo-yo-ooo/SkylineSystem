#!/bin/bash
cd /mnt/c/ZSY/SkylineSystem/tests/bin
for f in fc_cov-fc fc_cov-heap art_cov; do
  echo "== $f =="
  gcov -b -c "$f.gcda" 2>/dev/null | grep -aE 'Lines executed|Branches executed|Taken at least|No executable' | head -4
done
# 源名重映射后查看 .gcov 头部摘要
ls *.gcov 2>/dev/null | head -6
