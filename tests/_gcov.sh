#!/bin/bash
cd /mnt/c/ZSY/SkylineSystem/tests
echo "gcda files:"
ls *.gcda 2>/dev/null
echo "== gcov summaries =="
for f in *.gcda; do
  base=$(basename "$f" .gcda)
  gcov -b -c "$f" >/dev/null 2>&1
  # 每个源文件的 .gcov 里找头部摘要
  for g in *.gcov; do
    if [ -f "$g" ]; then
      head -6 "$g" | grep -aE 'Lines executed|Branches executed|Taken at least' | sed "s/^/[$g] /"
    fi
  done
done 2>/dev/null | head -24
