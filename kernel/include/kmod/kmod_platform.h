//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_platform.h —— kmod 子系统的宿主抽象层（内核实现 / 宿主测试实现）
 * ============================================================================
 * kmod 核心逻辑（加载器/拓扑/治理）不直接依赖 arch 或内核基础设施，
 * 只通过以下原语访问外部世界：
 *   - kernel: src/arch/x86_64/kmod/platform_kernel.cpp (kmalloc/kprintf/PIT/spinlock)
 *   - host  : tests/kmod/host_platform.cpp (malloc/printf/clock)
 * 这样同一份治理代码可以在宿主上做混沌/负向/ASAN 测试（tests/kmod）。
 * ==========================================================================*/
#ifndef _KMOD_PLATFORM_H_
#define _KMOD_PLATFORM_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 分配/释放：返回 16 字节对齐内存；失败返回 NULL。size 会原样传给释放。 */
void *kmod_alloc(uint64_t size);
void kmod_free(void *ptr, uint64_t size);

/* 内核日志出口（宿主实现为 printf；内核实现为 kprintf_） */
void kmod_log(const char *fmt, ...);

/* 单调毫秒（看门狗预算判定用）；不可用时返回 0 */
uint64_t kmod_uptime_ms(void);

/* 短等待（看门狗排空轮询用）；ms 为 0 时只让出一次 */
void kmod_wait_ms(uint32_t ms);

/* 关中断自旋锁：acquire 返回需要回填给 release 的标志字。
 * 宿主实现可直接用 pthread_mutex，返回 0。 */
uint64_t kmod_lock_acquire(volatile int32_t *lock);
void kmod_lock_release(volatile int32_t *lock, uint64_t flags);

/* 当前 CPU id（观测/审计用；宿主恒 0） */
uint32_t kmod_cpu_id(void);

#ifdef __cplusplus
}
#endif

#endif /* _KMOD_PLATFORM_H_ */
