#!/bin/bash
# tests/ci.sh — 一键复现: 全新 Ubuntu 24.04 应可运行
# 用法: bash ci.sh   (退出码 0 = 全部通过; 金样比对见 ci_golden/)
set -e
cd "$(dirname "$0")"

echo "== build =="
make -B slub slub-poison fc fc-reg sched 2>&1 | tail -3

echo "== run slub =="
./bin/slub_test > ci_slub.log 2>&1

echo "== run slub-poison (红区越界负向) =="
./bin/slub_poison > ci_poison.log 2>&1

echo "== run fc chaos+分路径 =="
./bin/fc_test > ci_fc.log 2>&1

echo "== run fc 三缺陷回归 =="
./bin/fc_reg > ci_fcreg.log 2>&1

echo "== run sched 模型 =="
./bin/sched_test > ci_sched.log 2>&1

echo "== 汇总 =="
grep -hE 'RESULT|FAIL|MISSING' ci_slub.log ci_poison.log ci_fc.log ci_fcreg.log ci_sched.log || true
echo "CI DONE (日志: ci_*.log)"
