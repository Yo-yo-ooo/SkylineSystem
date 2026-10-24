#!/bin/bash
# res/scripts/test/golden.sh — 金样门禁: 跑全量 CI 后与 golden.txt 逐行比对
# 用法: bash res/scripts/test/golden.sh   (退出码 0 = 与金样一致)
# 迁移 (用户清理): 原 tests/golden.sh → res/scripts/test/, ci.sh 同目录
set -e
# 先把脚本目录解析成绝对路径再接下去用: 下面会 cd 到 tests/, 之后再用
# 相对形式的 "$0" 就会解析成 tests/res/... 从而找不到 ci.sh (退出码 127,
# 表现为莫名其妙的 "GOLDEN: CI 失败")。用绝对路径可以同时支持相对/绝对调用。
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../../../tests"

bash "$HERE/ci.sh" > /tmp/golden_ci.out 2>&1 || { echo "GOLDEN: CI 失败 (见 ci_*.log)"; exit 1; }

grep -ahE 'RESULT|redzone-oob' ci_slub.log ci_poison.log ci_fc.log ci_fcreg.log ci_sched.log ci_slubmt.log ci_fcmt.log 2>/dev/null > /tmp/golden_actual.txt

if diff -u "$HERE/golden.txt" /tmp/golden_actual.txt; then
  echo "GOLDEN: PASS (与金样一致)"
  exit 0
else
  echo "GOLDEN: FAIL (输出与金样不一致 —— 若为有意变更, 更新 golden.txt)"
  exit 1
fi
