//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <klib/klib.h>
#include <stdint.h>
#include <stddef.h>
#include <atomic/atomic.h>
#include <klib/stallwatch.h>

/* 整机致命错误标志: 见 klib.h 的说明 */
extern "C" volatile int g_kernel_panic = 0;

#ifdef __x86_64__
#define KLIB_HALT_THIS_CPU() for (;;) { asm volatile("cli; hlt"); }
#elif defined(__aarch64__)
#define KLIB_HALT_THIS_CPU() for (;;) { asm volatile("msr daifset, #0xf; wfi"); }
#elif defined(__riscv)
#define KLIB_HALT_THIS_CPU() for (;;) { asm volatile("csrci mstatus, 0x8; wfi"); }
#else
#define KLIB_HALT_THIS_CPU() for (;;) { asm volatile("idle 0"); }
#endif

extern "C" void Panic(const char* message){
    kerrorln("Panic!");
    /* 可诊断性: 消息同时进串口 (此前只走 E9 调试端口, 串口只见
       "Panic!" 看不到原因 —— 挂载断言等曾因此漏报) */
    if (message) kerrorln("%s", message);
    e9_printf(message);
    hcf();
}

void Panic(bool halt, const char* message){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

void Panic(const char* message,bool halt){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

// Halt and catch fire function.
void hcf(void) {
    /* 停机前输出诊断 (原实现直接 hlt, 引导失败时串口无任何
       可诊断信息; headless 调试场景串口即唯一现场) */
    kerrorln("SYSTEM HALTED (hcf): no usable root filesystem or fatal init error");
    /* 在进入死循环**之前**置位 (打印期间仍允许其它核正常抢锁, 否则
       诊断日志可能因为抢不到串口锁而丢掉)。此后其余核一旦撞上锁等待
       就自行停摆 —— 否则它们会抱着"持锁者已停机"的锁永远自旋, 整机
       表现为假死 (实测: CPU0 file_cache_get / CPU3 file_cache_fsync
       双双卡死, 桌面再也拿不到 CPU)。 */
    __atomic_store_n(&g_kernel_panic, 1, __ATOMIC_RELEASE);
    for (;;) {
#ifdef __x86_64__
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
#endif
    }
}


/* P0 修复 (SMP 内存序): 解锁原来是 atomic_store_4(l,0,ATOMIC_RELAXED),
   展开为一条无屏障的 movl。x86 硬件 TSO 不会重排 store-store, 但
   relaxed 对【编译器】不构成屏障 —— 临界区里的写可以被合法地下沉到解锁
   store 之后, 于是另一个 CPU 拿到锁时仍可能看到尚未提交的临界区修改。
   改 ATOMIC_RELEASE (解锁前插编译器屏障), 与加锁侧 xchgb (隐含 lock 前缀
   + "memory" clobber, acquire 语义) 配对。

   注意: 加锁侧不要用 "关中断 + 无限自旋" 来当保护措施 —— BSP 在关中断
   自旋期间收不到 PIT 的 IRQ0, 全局墙钟 (TicksSinceBoot) 会冻结, 把所有
   以 TimeSinceBootMS() 为 deadline 的等待变成永旋 (已实测整机挂死)。
   需要 deadline 的地方一律用 PIT::MonotonicMS()。 */
extern "C" void spinlock_lock(spinlock_t* l) {
    /* 停滞诊断: 记录待锁现场 (锁地址 + 加锁点)。只有两条 store, 热路径可接受;
       CPU 停在这把锁上时, 看门狗 (stallwatch.cpp) 会把它打出来。 */
    stall_note_lock_wait((uint64_t)l, (uint64_t)__builtin_return_address(0));
    while(atomic_test_and_set(l,ATOMIC_ACQUIRE)) {
        /* 整机已判定致命错误: 这把锁的持有者很可能已经停机, 再自旋下去
           就是把一个 CPU 的 panic 放大成整机假死。本核就地停摆, 保留
           已经打出的现场 (谁崩的、卡在哪把锁)。 */
        if (__builtin_expect(__atomic_load_n(&g_kernel_panic, __ATOMIC_ACQUIRE) != 0, 0))
            KLIB_HALT_THIS_CPU();
#ifdef __x86_64__
        asm volatile("pause");
#elif defined(__aarch64__)
        asm volatile("yield");
#elif defined (__riscv)
        asm volatile("pause");
#endif
    }
    stall_note_lock_acquired((uint64_t)l, (uint64_t)__builtin_return_address(0));
}

extern "C" void spinlock_unlock(spinlock_t* l) {
    stall_note_lock_released();
    atomic_store_4(l,0,ATOMIC_RELEASE);
}


extern "C" void *__memcpy(void * d, const void * s, uint64_t n) { 
    _memcpy((void*)s, d, n);
    return d;
}

// 设置某一位为 1
void bitmap_set(u8* bitmap, u64 bit) {
    // bit >> 3 等价于 bit / 8
    // bit & 7 等价于 bit % 8
    bitmap[bit >> 3] |= (1U << (bit & 7));
}

// 清除某一位（设为 0）
void bitmap_clear(u8* bitmap, u64 bit) {
    bitmap[bit >> 3] &= ~(1U << (bit & 7));
}

// 获取某一位的值
bool bitmap_get(u8* bitmap, u64 bit) {
    return (bitmap[bit >> 3] & (1U << (bit & 7))) != 0;
}

uint16_t kld_16 (const uint8_t* ptr)	/*	 Load a 2-byte little-endian word */
{
	uint16_t rv;

	rv = ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

uint32_t kld_32 (const uint8_t* ptr)	/* Load a 4-byte little-endian word */
{
	uint32_t rv;

	rv = ptr[3];
	rv = rv << 8 | ptr[2];
	rv = rv << 8 | ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

uint64_t kld_64 (const uint8_t* ptr)	/* Load an 8-byte little-endian word */
{
	uint64_t rv;

	rv = ptr[7];
	rv = rv << 8 | ptr[6];
	rv = rv << 8 | ptr[5];
	rv = rv << 8 | ptr[4];
	rv = rv << 8 | ptr[3];
	rv = rv << 8 | ptr[2];
	rv = rv << 8 | ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

#if defined(__x86_64__)
#include <arch/x86_64/pit/pit.h>
#endif

/* lwIP 端口时间源: 必须为单调毫秒 (原实现是 RTC 日期编码, 跨日回绕会
   破坏 timeouts.c 的超时数学)。 */
extern "C" uint32_t sys_now(void){
#if defined(__x86_64__)
    return (uint32_t)PIT::TimeSinceBootMS();
#else
    return 0;
#endif

}
