#include <arch/x86_64/allin.h>
#include <klib/kio.h>



namespace LAPIC{

    u64 apic_ticks = 0;

    uint64_t SupportFlag;

    void Init() {
        LAPIC::Write(0xf0, 0x1ff);
        /*
        void* pg = PMM::Alloc(1);
        if(pg == nullptr)
            hcf();

        outb(0x21, 0xff);
        outb(0xa1, 0xff);
        
        outb(0x22, 0x70);
        outb(0x23, 0x01);

        
        wrmsr(0x1b, (uint64_t)pg | (rdmsr(0x1b) & ((1ull<< 12) - 1))); 
        
        uint32_t a, b, c, d;
	    cpuid(1, 0, &a, &b, &c, &d);
        if (c & (1ull<< 21))
		    LAPIC::SupportFlag |= LAPIC_SUPPORT_FLAG_X2Apic;
        if (*(u64 *)HIGHER_HALF(0xfee00030) & (1ull<< 24))
		    LAPIC::SupportFlag |= LAPIC_SUPPORT_FLAG_EOIBroadcase;

        
        u64 val = rdmsr(0x1b);
        bit_set1(&val, 11);
        if (LAPIC::SupportFlag & LAPIC_SUPPORT_FLAG_X2Apic) bit_set1(&val, 10);
        wrmsr(0x1b, val);
        
        u64 x, y;
        x = rdmsr(0x1b);
        

        // enable SVR[8] and SVR[12]
        {
            u64 val = (LAPIC::SupportFlag & LAPIC_SUPPORT_FLAG_X2Apic ? rdmsr(0x80f) : *(u64 *)HIGHER_HALF(0xfee000f0));
            bit_set1(&val, 8);
            if (LAPIC::SupportFlag & LAPIC_SUPPORT_FLAG_EOIBroadcase) bit_set1(&val, 12);
            if (LAPIC::SupportFlag & LAPIC_SUPPORT_FLAG_X2Apic) wrmsr(0x80f, val);
            else {
                *(u64 *)HIGHER_HALF(0xfee000f0) = val;
                *(u64 *)HIGHER_HALF(0xfee002f0) = 0x100000;
            }
        }

        outd(0xcf8, 0x8000f8f0);
	    mfence();
        */

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

    void IPI(u32 id, u8 dat) {
        LAPIC::Write(LAPIC_ICRHI, id << LAPIC_ICDESTSHIFT);
        LAPIC::Write(LAPIC_ICRLO, dat);
    }

}