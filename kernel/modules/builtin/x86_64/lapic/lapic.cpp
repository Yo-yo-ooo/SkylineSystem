//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <klib/kio.h>
#include <conf.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <arch/x86_64/lapic/lapic.h>
#include <klib/kio.h>
#include <acpi/madt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/dosvm/dosvm_contract.h>

static volatile uint64_t lapic_address = 0;


namespace LAPIC{

    uint64_t apic_ticks = 0;
    bool x2apic = false;


    void Init() {
        uint64_t apic_msr = rdmsr(0x1B);
        lapic_address = HIGHER_HALF(madt_apic_address);
        apic_msr |= 0x800; // Enable flag
        uint32_t a = 1, b = 0, c = 0, d = 0;
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a));
        if (c & (1 << 21)) {
            apic_msr |= 0x400; // Enable x2apic
            x2apic = true;
        }
        wrmsr(0x1B, apic_msr);
        LAPIC::Write(0xF0, LAPIC::Read(0xF0) | 0x100); // Spurious interrupt vector
    }

    void Write(uint32_t reg, uint64_t val) {
        if (x2apic) {
            reg = (reg >> 4) + 0x800;
            wrmsr(reg, val);
            return;
        }
        uint64_t addr = lapic_address + reg;
        *((volatile uint32_t*)addr) = val;
        return;
    }

    uint64_t Read(uint32_t reg) {
        if (x2apic) {
            reg = (reg >> 4) + 0x800;
            return rdmsr(reg);
        }
        uint64_t addr = lapic_address + reg;
        return *((volatile uint32_t*)addr);
    }


    uint32_t GetID() {
        if(!x2apic) return LAPIC::Read(0x0020) >> LAPIC_ICDESTSHIFT;
        else return LAPIC::Read(0x0020);
    }

    void EOI() {
        LAPIC::Write(0xb0, 0x0);
    }

    void StopTimer() {
        // We do this to avoid overlapping oneshots
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0);
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
    }

    void Oneshot(uint32_t vec, uint64_t ms) {
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0);
        LAPIC::Write(LAPIC_TIMER_DIV,0x3);
        LAPIC::Write(LAPIC_TIMER_INITCNT, ms);
        LAPIC::Write(LAPIC_TIMER_LVT, vec);
    }

    uint64_t InitTimer() {
        uint64_t rflags;
        asm volatile("pushfq\n\t""pop %0\n\t""cli" : "=r"(rflags) :: "memory");
        LAPIC::Write(LAPIC_TIMER_DIV, 0x3); // Divide by 16
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);
        //PIT::Sleep(1); // 1 ms
        HPET::SleepNS(1000000);
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
        uint32_t ticks = 0xFFFFFFFF - LAPIC::Read(LAPIC_TIMER_CURCNT);
        asm volatile("push %0\n\t""popfq" :: "r"(rflags) : "memory");
        if (ticks < 1000) {
            // 如果 1ms 内滴答数过低，说明校准失败或频率异常
            return 1000000; // 返回一个合理的假定值（假设 100MHz）
        }
        return ticks;
    }

    void IPI(uint32_t id, uint32_t dat) {
        /* Tier0 / dosvm 生死线: 离线核已清 CR0.PG 跑在 real/compat 模式,
           任何按 64 位 IDT 语义投递的 IPI 都会让它跳进不存在的 64 位门
           -> triple fault。所有 IPI 发送点最终都汇聚到这里, 因此在此做
           统一兜底过滤 (xAPIC 与 x2APIC 分支共用, 必须在分支之前)。
           0xF0 = DOSVM_IPI_VEC 是专门唤醒该核做 long->real->long 往返的
           向量, 由 Tier1 用 16 位安全的 handler 处理, 必须放行。 */
        const uint32_t vector = dat & 0xFFu;
        if (vector != DOSVM_IPI_VEC && dosvm_lapic_offline(id)) return;

        if (x2apic) {
            LAPIC::Write(0x300, ((uint64_t)id << 32) | dat);
            return;
        }
        LAPIC::WaitICR(); // 发送前必须等待上一个完成
        LAPIC::Write(LAPIC_ICRHI, id << LAPIC_ICDESTSHIFT);
        LAPIC::Write(LAPIC_ICRLO, dat);
    }

    void IPIAll(uint32_t lapic_id, uint32_t vector) {
        /* 旧实现用 destination shorthand(0x80000 = all including self)
           一次性广播 —— 硬件层无法排除单个 offline 核。改为遍历
           smp_cpu_list[] 逐核发 IPI(), 让 IPI() 里的离线过滤真正生效。
           lapic_id 仅为兼容旧签名保留 (shorthand 时代的目的地字段)。
           引导期 (init.cpp) 掩码恒为 0, 行为与原来逐位一致。 */
        (void)lapic_id;
        int32_t last = smp_last_cpu;
        if (last < 0) last = 0;
        if (last >= MAX_CPU) last = MAX_CPU - 1;
        for (int32_t i = 0; i <= last; i++) {
            cpu_t *c = smp_cpu_list[i];
            if (!c) continue;
            LAPIC::IPI(c->lapic_id, vector);
        }
    }

    void WaitICR() {
        if (x2apic) return; // x2APIC 不需要检查 Busy 位
        // 检查 ICR_LOW (0x300) 的第 12 位 (Delivery Status)
        while (LAPIC::Read(0x300) & (1 << 12)) {
            asm volatile("pause"); 
        }
    }

    void IPIOthers(uint32_t lapic_id, uint32_t vector) {
        /* 同 IPIAll: shorthand(0xC4000 = all excluding self) 无法排除
           单个 offline 核, 改为逐核发送 + 跳过发送者 + 底层统一过滤。
           init.cpp:187 在 SMP 启动完成后调用, 此时 smp_cpu_list[]、
           smp_last_cpu、this_cpu() 均已就绪且掩码恒为 0, 行为不变。 */
        int32_t last = smp_last_cpu;
        if (last < 0) last = 0;
        if (last >= MAX_CPU) last = MAX_CPU - 1;
        cpu_t *self = this_cpu();
        const uint32_t self_lapic = self ? self->lapic_id : lapic_id;
        for (int32_t i = 0; i <= last; i++) {
            cpu_t *c = smp_cpu_list[i];
            if (!c) continue;
            if (self ? (c == self) : (c->lapic_id == self_lapic)) continue;
            LAPIC::IPI(c->lapic_id, vector);
        }
    }

}