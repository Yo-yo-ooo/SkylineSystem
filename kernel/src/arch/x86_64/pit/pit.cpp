//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/interrupt/idt.h>
#include <klib/kio.h>
#include <arch/x86_64/rtc/rtc.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/smp/smp.h>
#include <fs/fc.h>

extern uint64_t mktime (uint32_t year, uint32_t mon,
    uint32_t day, uint32_t hour,
    uint32_t min, uint32_t sec);



/* tsc_cal.cpp 的强符号 (未校准返回 0)。必须在 namespace PIT 之外声明 ——
   写在命名空间里会被名字修饰成 PIT::sched_tsc_per_ms, 与全局定义不匹配。 */
extern uint64_t sched_tsc_per_ms(void);

namespace PIT
{
    int32_t roughCount = (BaseFrequency/200) / 2;
    uint64_t TicksSinceBoot = 0;
    uint64_t MicroSecondOffset = 0;

    uint16_t Divisor = 65535;

    uint16_t NonMusicDiv = 5000;//65535;//2000; // 596.591 Hz

    bool Inited = false;

    uint64_t freq = GetFrequency();
    int32_t FreqAdder = 1;
    void (*TickHandle)();

    
    int32_t tempus = 0;
    void Tick__(){
        TicksSinceBoot++;
        
        if (tempus++ > roughCount){
            tempus = 0;
            RTC::UpdateTimeIfNeeded();
        }
        
    }

    void Tick_(){
        TicksSinceBoot++;
        
        if (tempus++ > roughCount)
        {
            tempus = 0;
            RTC::UpdateTimeIfNeeded();
        }
        Schedule::Tick();
    }

    void InitPIT()
    {
        TicksSinceBoot = 0;
        SetDivisor(NonMusicDiv /*65535*/);
        freq = GetFrequency();
        //irq_register(0, PIT::Handler);
        idt_install_irq(32,(void*)PIT::Handler);
        Inited = true;
        RTC::InitRTC();
        RTC::read_rtc();
        RTC_INIT_DAY = RTC::Day;
        RTC_INIT_MONTH = RTC::Month;
        RTC_INIT_YEAR = RTC::Year;
        RTC_INIT_HOUR = RTC::Hour;
        RTC_INIT_MINUTE = RTC::Minute;
        RTC_INIT_SECOND = RTC::Second;
        TIME_SINCE_RTC_INITED_SECOND = mktime(RTC_INIT_YEAR,RTC_INIT_MONTH,RTC_INIT_DAY,
            RTC_INIT_HOUR,RTC_INIT_MINUTE,RTC_INIT_SECOND);
        TickHandle = PIT::Tick__;
    }

    void Handler(registers *r){
        Tick();
        
        LAPIC::EOI();
    }

    void SetDivisor(uint16_t divisor)
    {
        if (divisor < 5)
            divisor = 5;

        if (Inited)
        {
            MicroSecondOffset += (TicksSinceBoot * 1000000) / freq;
            TicksSinceBoot = 0;
        }

        Divisor = divisor;
        roughCount = (BaseFrequency/divisor) / 2;
        outb(0x43, 0x36);
        outb(0x40, (uint8_t)(divisor & 0x00ff));
        io_wait();
        outb(0x40, (uint8_t)((divisor & 0xff00) >> 8));
        io_wait();
        freq = GetFrequency();
        FreqAdder = 1000000/(BaseFrequency / divisor);
    }

    void Sleep(uint64_t milliseconds)
    {
        if (!Inited)
            return;
        /* P2-62 接活 (round 96): 调度器就绪 (有线程上下文) 时走真睡眠
           (Schedule::Sleep 的定时器轮 + THREAD_SLEEPING + Yield), 释放
           本核给其他线程; 引导早期 (驱动探测, 无线程上下文) 保持忙等
           —— 忙等语义在无调度器时必需 */
        if (smp_started && Schedule::this_thread()) {
            Schedule::Sleep(milliseconds);
            return;
        }
        uint64_t endTime = TimeSinceBootMS() + milliseconds;
        while (TimeSinceBootMS() < endTime)
            asm("pause");
    }

    void Sleepd(uint64_t seconds)
    {
        /* 修复: 原递归调用自身(无限递归)且先乘后转; 应调用 Sleep(ms) */
        Sleep(seconds * 1000);
    }

    uint64_t GetFrequency()
    {
        return BaseFrequency / Divisor;
    }

    void SetFrequency(uint64_t frequency)
    {
        SetDivisor(BaseFrequency / frequency);
        freq = GetFrequency();
    }

    


    void Tick(){
        /* P2-57: 原子加载函数指针 —— 写入侧 (sched Install) 用
           atomic_store, 读取侧必须匹配, 否则跨核初始化窗口 = 数据竞争 */
        uint64_t fn = __atomic_load_n((uint64_t*)&TickHandle, __ATOMIC_ACQUIRE);
        ((void(*)(void))(uintptr_t)fn)();
        cpu_t *cpu = this_cpu();
        if (cpu && cpu->file_cache) {
            file_cache_tick(cpu->file_cache);
        }
    }
    
    static inline uint64_t tsc_now_raw(void) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    /* PIT 冻结判定阈值 (毫秒)。
       判据是"PIT 读数本身停滞", 而不是"TSC 领先 PIT" —— 后者对 TSC 校准
       误差不免疫 (TSC 快一倍就会永久误触发), 前者只在 PIT 真的不再递增时
       才成立, 因此健康机器上本分支永远不会命中。 */
    #define PIT_STALL_GUARD_MS 250ULL

    /* 单调地板: 保证在 PIT 冻结 -> TSC 接管 -> PIT 恢复追上来 的切换过程中
       返回值永不回退 (定时器轮与 vruntime 都假设 now 单调)。 */
    static uint64_t g_ms_floor = 0;
    static uint64_t g_last_pit_ms = 0;      /* 上次看到的 PIT 读数 */
    static uint64_t g_last_move_tsc = 0;    /* 该读数变化时对应的 TSC 毫秒 */

    static inline uint64_t ms_floor_raise(uint64_t v) {
        uint64_t f = __atomic_load_n(&g_ms_floor, __ATOMIC_ACQUIRE);
        for (;;) {
            if (v <= f) return f;
            if (__atomic_compare_exchange_n(&g_ms_floor, &f, v, false,
                                            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
                return v;
        }
    }

    uint64_t TimeSinceBootMS()
    {
        if (!Inited)
            return 0;
        uint64_t pit_ms = (TicksSinceBoot*1000)/freq + MicroSecondOffset / 1000;
        uint64_t cpm = sched_tsc_per_ms();
        if (!cpm)
            return pit_ms;                  /* TSC 未校准: 维持原语义 */

        uint64_t tsc_ms = tsc_now_raw() / cpm;
        uint64_t lp = __atomic_load_n(&g_last_pit_ms, __ATOMIC_RELAXED);
        uint64_t lm = __atomic_load_n(&g_last_move_tsc, __ATOMIC_RELAXED);
        if (pit_ms != lp) {
            /* PIT 走了一步: 记录新的读数与它发生时的 TSC 时刻 */
            __atomic_store_n(&g_last_pit_ms, pit_ms, __ATOMIC_RELAXED);
            __atomic_store_n(&g_last_move_tsc, tsc_ms, __ATOMIC_RELAXED);
            lm = tsc_ms;
        }
        uint64_t v = pit_ms;
        if (tsc_ms > lm && tsc_ms - lm > PIT_STALL_GUARD_MS) {
            /* PIT 读数很久没动而 TSC 一直在走 = 节拍被中断屏蔽 (BSP 关中断
               自旋)。冻结期间改用"冻结点 + 此后 TSC 走过的时间"继续推进,
               否则所有以本函数为 deadline 的等待会立刻变成永旋。 */
            v = pit_ms + (tsc_ms - lm);
        }
        return ms_floor_raise(v);
    }

    uint64_t TimeSinceBootMicroS()
    {
        if (!Inited)
            return 0;
        return (TicksSinceBoot*1000000)/freq + MicroSecondOffset;
    }

    uint64_t MonotonicMS()
    {
        if (!Inited)
            return 0;
        uint64_t cpm = sched_tsc_per_ms();
        if (likely(cpm))
            return tsc_now_raw() / cpm;
        /* 未校准: 退回 PIT 墙钟 —— 调用方必须另有自旋次数上限兜底 */
        return TimeSinceBootMS();
    }

}