#!/bin/bash
# tests/golden.sh — 金样门禁: 跑全量 CI 后与 golden.txt 逐行比对
# 用法: bash golden.sh   (退出码 0 = 与金样一致)
set -e
cd "$(dirname "$0")"

bash ci.sh > /tmp/golden_ci.out 2>&1 || { echo "GOLDEN: CI 失败 (见 ci_*.log)"; exit 1; }

grep -ahE 'RESULT|redzone-oob' ci_slub.log ci_poison.log ci_fc.log ci_fcreg.log ci_sched.log ci_slubmt.log ci_fcmt.log 2>/dev/null > /tmp/golden_actual.txt

if diff -u golden.txt /tmp/golden_actual.txt; then
  echo "GOLDEN: PASS (与金样一致)"
  exit 0
else
  echo "GOLDEN: FAIL (输出与金样不一致 —— 若为有意变更, 更新 golden.txt)"
  exit 1
fi
