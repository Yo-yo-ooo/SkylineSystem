//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* heap_internal.h — SLAB/SLUB/kmalloc 三个编译单元共享的内部辅助
   (拆分自 heap.cpp: irq 关断包装、自旋锁+关中断、slab_fatal) */
#pragma once

#include <klib/klib.h>
#include <mem/pmm.h>
#include <pdef.h>
#ifdef __x86_64__
#include <arch/x86_64/vmm/vmm.h>
#endif

/* 共享宏 (拆分自 heap.cpp) */
#define SLAB_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))

#define SLAB_PAGES 1
#define SLAB_SIZE (SLAB_PAGES * PAGE_SIZE)

#ifndef MAX_SLAB_ORDER
#define MAX_SLAB_ORDER 7
#endif
static_assert(MAX_SLAB_ORDER >= 1 && MAX_SLAB_ORDER <= 16, "bad MAX_SLAB_ORDER");
#define MAX_SLAB_SIZE (16u << (MAX_SLAB_ORDER - 1))

// Per-CPU 水位线调优：当前回流阈值为 2 * SLAB_BATCH。
#define SLAB_BATCH 16
#define DRAIN_HIGH_WATERMARK (SLAB_BATCH * 4)
#define DRAIN_LOW_WATERMARK (SLAB_BATCH * 2)
#define EMPTY_CACHE_LIMIT 4

// 页池水位：池内 ≥16 页时归还到 4 页
#define PAGE_POOL_HIGH_WATERMARK 16
#define PAGE_POOL_LOW_WATERMARK  4

#define SLAB_PAGE_MAGIC  0x50414745
#define LARGE_PAGE_MAGIC 0x51424D55

// 调试开关：页毒化与红区越界检测
#ifdef SLAB_DEBUG_POISON
#define SLAB_POISON_ALLOC 0xAA
#define SLAB_POISON_FREE  0xDD
#define SLAB_REDZONE_MAGIC 0xDEADBEEFCAFEBABEULL
#define REDZONE_SIZE 8
#define REDZONE_MIN_OBJ_SIZE 64 // 仅对象 >= 64 字节时启用红区，避免小对象开销过大
#else
#define REDZONE_SIZE 0
#endif

static inline uint64_t irq_save() {
    uint64_t flags;
#ifdef __KERNEL_TEST_HOST__   /* 宿主测试: 用户态不能执行 cli, 仅读 flags */
    asm volatile("pushfq\n\tpop %0" : "=r"(flags) :: "memory");
#else
    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(flags) :: "memory");
#endif
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    asm volatile("push %0\n\tpopfq" :: "r"(flags) : "memory");
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = irq_save();
    spinlock_lock(lock);
    return flags;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_unlock(lock);
    irq_restore(flags);
}

static void slab_fatal(const char *msg) __attribute__((noreturn));
static void slab_fatal(const char *msg) {
    Panic(msg);
    for (;;) asm volatile("cli; hlt");
}
