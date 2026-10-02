#!/bin/bash
# res/scripts/test/ci.sh — 一键复现: 全新 Ubuntu 24.04 应可运行
# 用法: bash res/scripts/test/ci.sh   (退出码 0 = 全部通过; 金样比对见 golden.sh)
# 迁移 (用户清理): 原 tests/ci.sh → res/scripts/test/, 工作目录解析到 tests/
set -e
cd "$(dirname "$0")/../../../tests"

echo "== build =="
make -B slub slub-poison fc fc-reg sched slub-mt fc-mt 2>&1 | tail -3

echo "== run slub =="
./bin/slub_test > ci_slub.log 2>&1

echo "== run slub-poison (红区越界负向) =="
./bin/slub_poison > ci_poison.log 2>&1

echo "== run fc chaos+分路径+混合负载 =="
./bin/fc_test > ci_fc.log 2>&1

echo "== run fc 三缺陷回归 =="
./bin/fc_reg > ci_fcreg.log 2>&1

echo "== run sched 模型 =="
./bin/sched_test > ci_sched.log 2>&1

echo "== run slub 多线程 (4 虚拟 CPU) =="
./bin/slub_mt > ci_slubmt.log 2>&1

echo "== run fc 多线程 (4 线程 × 4 实例) =="
./bin/fc_mt > ci_fcmt.log 2>&1

echo "== 汇总 =="
grep -ahE 'RESULT|FAIL|MISSING|redzone-oob' ci_slub.log ci_poison.log ci_fc.log ci_fcreg.log ci_sched.log ci_slubmt.log ci_fcmt.log || true
echo "CI DONE (日志: ci_*.log)"
