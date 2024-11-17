#include "lapic.h"
#include "../ioapic/ioapic.h"
#include "../pit/pit.h"
namespace LAPIC{

    u64 apic_ticks = 0;

    void Init() {
        LAPIC::Write(0xf0, 0x1ff);
        kinfo("    LAPIC::Init(): LAPIC Initialised.\n");
    }

    void Write(u32 reg, u32 val) {
        *((volatile u32*)(HIGHER_HALF(0xfee00000) + reg)) = val;
    }

    u32 Read(u32 reg) {
        return *((volatile u32*)(HIGHER_HALF(0xfee00000) + reg));
    }

    u32 GetID() {
    return LAPIC::Read(0x0020) >> LAPIC_ICDESTSHIFT;
    }

    void EOI() {
        LAPIC::Write((u8)0xb0, 0x0);
    }
    void StopTimer() {
        // We do this to avoid overlapping oneshots
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0);
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
    }

    void Oneshot(u8 vec, u64 ms) {
        LAPIC::StopTimer();
        LAPIC::Write(LAPIC_TIMER_DIV, 0);
        LAPIC::Write(LAPIC_TIMER_LVT, vec);
        LAPIC::Write(LAPIC_TIMER_INITCNT, apic_ticks * ms);
    }

    void CalibrateTimer() {
        LAPIC::StopTimer();
        LAPIC::Write(LAPIC_TIMER_DIV, 0);
        LAPIC::Write(LAPIC_TIMER_LVT, (1 << 16) | 0xff);
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);
        PIT::Sleep(1); // 1 ms
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
        u32 ticks = 0xFFFFFFFF - LAPIC::Read(LAPIC_TIMER_CURCNT);
        apic_ticks = ticks;
        LAPIC::StopTimer();
    }
}