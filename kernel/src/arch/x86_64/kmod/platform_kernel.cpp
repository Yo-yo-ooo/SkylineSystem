//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * platform_kernel.cpp —— kmod 平台抽象层的内核实现
 * ----------------------------------------------------------------------------
 * 只依赖内核公共设施：kmalloc/kfree、kprintf、PIT、中断开关、自旋锁。
 * 宿主测试使用 tests/kmod/host_platform.cpp 的替代实现（不编译本文件）。
 * ==========================================================================*/
#include <kmod/kmod_platform.h>

#include <stdarg.h>
#include <klib/klib.h>
#include <klib/kprintf.h>
#include <mem/heap.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>

extern "C" {

void *kmod_alloc(uint64_t size)
{
    if (size == 0)
        return nullptr;
    /* 16 字节对齐足够满足 ELF 段的 sh_addralign（加载器内部再按段对齐切分）；
       模块代码和数据都在 HHDM 直接映射里，页属性 RW 且未置 NX（见 VMM::Init），
       因此加载后可直接执行 —— 这是"低隔离内核态"的既有事实，治理层用
       capability/审计/回滚补足可控性，而不是假装存在硬件隔离。 */
    return kmalloc_aligned(size, 16);
}

void kmod_free(void *ptr, uint64_t size)
{
    (void)size;
    if (ptr)
        kfree(ptr);
}

void kmod_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf_(fmt, ap);
    va_end(ap);
}

uint64_t kmod_uptime_ms(void)
{
    /* PIT 未初始化或 IRQ 尚未开启时 TicksSinceBoot 为 0：预算判定会读到 0，
       不会误判超时。真正的"init 卡死"由 stallwatch/看门狗在别处兜底。 */
    return PIT::TimeSinceBootMS();
}

void kmod_wait_ms(uint32_t ms)
{
    if (ms == 0u) {
        __asm__ volatile("pause" ::: "memory");
        return;
    }
    /* 引导期（interrupts disabled / 调度器未上线）：不能 PIT::Sleep（没有
       定时器中断推进等待队列）。此时用 PIT 单调毫秒忙等兜底；组件图/自检
       在 sti 之前单线程执行，等待窗口只可能来自 quiesce 排空且时长极短。 */
    if (!Interrupt::State()) {
        uint64_t deadline = PIT::MonotonicMS() + (uint64_t)ms;
        while (PIT::MonotonicMS() < deadline)
            __asm__ volatile("pause" ::: "memory");
        return;
    }
    /* 热替换/排空只在调度器已运行的线程上下文发生（引导期加载不做等待），
       所以这里可以直接用真睡眠；不要把本函数用于 bootstrap 上下文。 */
    PIT::Sleep((uint64_t)ms);
}

uint64_t kmod_lock_acquire(volatile int32_t *lock)
{
    bool was = Interrupt::SaveMask();
    spinlock_lock((int32_t *)lock);
    return was ? 1ull : 0ull;
}

void kmod_lock_release(volatile int32_t *lock, uint64_t flags)
{
    spinlock_unlock((int32_t *)lock);
    Interrupt::Restore(flags != 0ull);
}

uint32_t kmod_cpu_id(void)
{
    return this_cpu()->id;
}

} /* extern "C" */
