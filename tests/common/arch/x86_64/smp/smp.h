// tests/common/arch/x86_64/smp/smp.h — 宿主测试 shim
// 覆盖 kernel/include/arch/x86_64/smp/smp.h（内核堆只用到 cpu_t.id / cpu_t.cslab / this_cpu）
// 多线程模式: 每个宿主线程一个线程局部 cpu_t (test_cpu_set_id 设 id),
// SLUB 的 per-CPU active slab 与 SLAB 的 cslab magazine 按 id 分离 → 真实并发路径。
#pragma once
#include <stdint.h>

#define MAX_SLAB_ORDER 7

typedef struct {
    void *freelist[MAX_SLAB_ORDER];
    uint32_t count[MAX_SLAB_ORDER];
} cpu_slab_t;

typedef struct cpu_t {
    uint32_t id;
    cpu_slab_t cslab;
} cpu_t;

extern cpu_t *this_cpu(void);
extern cpu_t *get_cpu(uint32_t i);
extern void test_cpu_set_id(uint32_t id);
