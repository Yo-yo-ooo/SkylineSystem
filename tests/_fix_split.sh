#!/bin/bash
cat >> /mnt/c/ZSY/SkylineSystem/kernel/src/mem/slab.cpp << 'EOF'

// 诊断访问器 (拆分自 heap.cpp 胶水: 引用本文件全局)
extern "C" uint64_t slab_page_pool_count(void) { return g_pool_count; }
extern "C" uint64_t slab_lock_acquires(uint32_t idx) {
    return (idx < MAX_SLAB_ORDER) ? g_cache_lock_acquires[idx] : 0;
}
EOF
cd /mnt/c/ZSY/SkylineSystem/tests
sed -i 's/\r$//' Makefile
make -B slub 2>&1 | grep -E '错误|error:' | head -5
./bin/slub_test > /tmp/slub_split2.out 2>&1
echo "SLUB_EXIT=$?"
grep -aE 'RESULT' /tmp/slub_split2.out | tail -1
