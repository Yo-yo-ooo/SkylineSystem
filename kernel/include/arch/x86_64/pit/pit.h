//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <klib/kio.h>
#include <arch/x86_64/interrupt/idt.h>

namespace PIT
{
    extern uint64_t TicksSinceBoot;
    extern uint64_t freq;
    extern uint16_t Divisor;
    static const uint64_t BaseFrequency = 1193182;
    extern bool Inited;
    extern int32_t FreqAdder;
    extern uint16_t NonMusicDiv;
    extern void (*TickHandle)();

    void Handler(registers *r);

    void Sleepd(uint64_t seconds);
    void Sleep(uint64_t milliseconds);
    void InitPIT();

    void SetDivisor(uint16_t divisor);

    /* 开机毫秒数 (记账/定时器轮/uptime 语义)。
       注意: TicksSinceBoot 只在 BSP 的 PIT 中断上下文递增 (IOAPIC 把 IRQ0
       只路由到 BSP), 所以任一核——尤其 BSP——在关中断自旋期间它会冻结。
       TimeSinceBootMS() 现已内置冻结保护: PIT 落后 TSC 超过 250ms 时由 TSC
       顶上, 并带单调地板, 因此做 deadline 是安全的。
       但仍建议新生代码直接用 MonotonicMS(): 它恒以 TSC 为源, 不经过
       PIT/地板这套切换逻辑, 语义更直白。 */
    uint64_t TimeSinceBootMS();
    uint64_t TimeSinceBootMicroS();

    /* 超时/deadline 专用单调毫秒时钟: 已校准则恒走 TSC (随指令执行推进,
       不受 IF / 中断路由影响), 未校准退回 TimeSinceBootMS()。 */
    uint64_t MonotonicMS();

    uint64_t GetFrequency();
    void SetFrequency(uint64_t frequency);
    void Tick();
    void Tick_();
    void Tick__();
}
