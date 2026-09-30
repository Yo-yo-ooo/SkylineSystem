// tests/common/arch/x86_64/smp/smp.h — 宿主测试 shim
// 覆盖 kernel/include/arch/x86_64/smp/smp.h（内核堆只用到 cpu_t.id / cpu_t.cslab / this_cpu）
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

extern cpu_t g_test_cpu;

static inline cpu_t *this_cpu() { return &g_test_cpu; }
static inline cpu_t *get_cpu(uint32_t i) { (void)i; return &g_test_cpu; }
