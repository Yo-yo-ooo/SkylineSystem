#!/bin/bash
# 对照实验: 用 git HEAD(旧版) 的 x86mem 源码跑同一套 memops 测试,
# 用于确认新实现修掉的缺陷在旧版上确实可复现。
# 用法: bash tests/memops/run_old.sh
set -u
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cd "$(dirname "$0")/../.." || exit 1

OLD=/tmp/old_x86mem
OUT=tests/bin/old
mkdir -p "$OUT"

MEM_INC="-I ablib/arch/x86_64/x86mem -isystem ablib/freestndchdrs/x86_64/include"
CF="-O3 -g --std=gnu17 -Wall -Wno-unused-parameter -Wno-unused-function \
    -fno-merge-all-constants -ffunction-sections -fomit-frame-pointer"

tier() {
    local name=$1; shift
    for f in memcpy memmove memset memcmp; do
        gcc $CF $MEM_INC "$@" -c "$OLD/$f.c" -o "$OUT/${f}_$name.o" 2>/dev/null || \
            gcc $CF $MEM_INC "$@" -c "$OLD/$f.c" -o "$OUT/${f}_$name.o" 2>&1 | head -5
    done
    ld -r "$OUT/memcpy_$name.o" "$OUT/memmove_$name.o" "$OUT/memset_$name.o" \
          "$OUT/memcmp_$name.o" -o "$OUT/memops_$name.o"
}

echo "== 编译旧版 (git HEAD) x86mem =="
tier base   -msse4.2 -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512
tier avx    -mavx    -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512
tier avx2   -mavx -mavx2 -DX86MEM_NOT_COMPILE_AVX512
tier avx512 -mavx -mavx2 -mavx512f -mprefer-vector-width=512

echo "== 链接旧版测试二进制 =="
g++ -std=gnu++17 -O2 -g -I ablib/arch/x86_64/x86mem \
    tests/memops/main.cpp tests/bin/memops_shim.o \
    "$OUT/memops_base.o" "$OUT/memops_avx.o" "$OUT/memops_avx2.o" "$OUT/memops_avx512.o" \
    -o "$OUT/memops_test_old" -pthread || exit 1

g++ -std=gnu++17 -O2 -g -I ablib/arch/x86_64/x86mem \
    tests/memops/bench.cpp tests/bin/memops_shim.o \
    "$OUT/memops_base.o" "$OUT/memops_avx.o" "$OUT/memops_avx2.o" "$OUT/memops_avx512.o" \
    -o "$OUT/memops_bench_old" -pthread || exit 1

echo
"$OUT/memops_test_old" 2>&1 | tail -4
echo
echo "== 旧版吞吐 (GB/s) =="
"$OUT/memops_bench_old" 2>&1 | tail -50
