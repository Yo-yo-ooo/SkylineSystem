// tests/common/arch/x86_64/vmm/vmm.h — 宿主测试 shim
// 覆盖 kernel/include/arch/x86_64/vmm/vmm.h（内核堆只需要页分配原语）
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct pagemap_t { int dummy; } pagemap_t;
extern pagemap_t *kernel_pagemap;

namespace VMM {
    // 页池由 kernel_shim.cpp 提供（对齐 4096、记账泄漏）
    void *Alloc(pagemap_t *pm, uint64_t npages, bool user);
    void  Free(pagemap_t *pm, void *ptr);
    uint64_t LivePages();
    // 宿主测试辅助: 判断指针是否落在页池分配范围内(供完整性巡检防御)
    bool HeapContains(const void *p);
}

#ifdef __cplusplus
extern "C" {
#endif
// 故障注入: 之后 n 次页分配返回 NULL; set_fail_all=1 则持续失败(测 OOM 路径)
void vmm_fail_next_alloc(uint32_t n);
void vmm_set_fail_all(int on);
// 打印当前存活页及其分配站点 RA(解释 live_pages 构成)
void dump_live_pages(void);
#ifdef __cplusplus
}
#endif
