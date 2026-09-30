#!/bin/bash
cd /mnt/c/ZSY/SkylineSystem/tests
echo "== ci logs =="
grep -aE 'RESULT|ACTIVE|MISSING' ci_slub.log ci_poison.log 2>/dev/null | tail -3
echo "== fc-reg =="
./bin/fc_reg 2>/dev/null | grep -a RESULT
echo "== sched tail =="
./bin/sched_test 2>/dev/null | tail -1
