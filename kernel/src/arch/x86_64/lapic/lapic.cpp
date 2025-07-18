#include <arch/x86_64/allin.h>
#include <klib/kio.h>
#include <conf.h>


namespace LAPIC{

    u64 apic_ticks = 0;
    u64 lapic_address = 0;
    bool x2apic = false;

    uint64_t SupportFlag;

    void Init() {
        
#if SET_TEST_x2Apic == 1
        uint64_t apic_msr = rdmsr(0x1B);
        lapic_address = HIGHER_HALF(lapic_addr);
        apic_msr |= 0x800; // Enable flag
        uint32_t a = 1, b = 0, c = 0, d = 0;
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a));
        if (c & (1 << 21)) {
            apic_msr |= 0x400; // Enable x2apic
            x2apic = true;
        }
        wrmsr(0x1B, apic_msr);
        LAPIC::Write(0xF0, LAPIC::Read(0xF0) | 0x100); // Spurious interrupt vector
#else
        LAPIC::Write(0xf0, 0x1ff);
#endif

        kinfo("    LAPIC::Init(): LAPIC Initialised.\n");
    }

    void Write(u32 reg, u32 val) {
        if (x2apic) {
            reg = (reg >> 4) + 0x800;
            wrmsr(reg, val);
            return;
        }
         *((volatile u32*)(HIGHER_HALF(0xfee00000) + reg)) = val;
        //*((volatile u32*)(HIGHER_HALF(0xfee00000) + reg)) = val;
    }

    u32 Read(u32 reg) {
        if (x2apic) {
            reg = (reg >> 4) + 0x800;
            return rdmsr(reg);
        }
        return *((volatile u32*)(HIGHER_HALF(0xfee00000) + reg));
    }


    u32 GetID() {
        if(smp_cpu_started == 0) return 0; 
        uint32_t id = LAPIC::Read(0x20);
        if(!x2apic) id >>= LAPIC_ICDESTSHIFT;
        return id;
        //return LAPIC::Read(0x0020) >> LAPIC_ICDESTSHIFT;
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

    void IPI(u32 id, u8 dat) {
        if (x2apic) {
            LAPIC::Write(0x300, ((uint64_t)id << 32) | dat);
            return;
        }
        LAPIC::Write(LAPIC_ICRHI, id << LAPIC_ICDESTSHIFT);
        LAPIC::Write(LAPIC_ICRLO, dat);
    }

}