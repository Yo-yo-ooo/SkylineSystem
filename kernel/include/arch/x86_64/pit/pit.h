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
        实现 (round 97 修复): TicksSinceBoot (IRQ0 累加) + **直接读 PIT
        channel 0 硬件计数器**得到的节拍内分数, 因此:
          - 某个核收不到 / 被屏蔽 IRQ0 时, 复读硬件计数器仍能看到它在走;
          - deadline 不会再因为"本核墙钟冻结"而变成永旋 (实测症状: 卡在
            AHCI 轮询 55 秒不退出, 整机停摆);
          - 另保留 PIT 停滞 -> TSC 接管的兜底, 并带单调地板。
        新代码做 deadline 仍推荐 MonotonicMS() (纯 TSC, 语义更直白)。 */
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
