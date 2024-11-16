#include "lapic.h"

namespace LAPIC{
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
}