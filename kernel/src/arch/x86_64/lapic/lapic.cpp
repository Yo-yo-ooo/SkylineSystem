#include "lapic.h"

void lapic_write(u32 reg, u32 val) {
    *((volatile u32*)(HIGHER_HALF(0xfee00000) + reg)) = val;
}

u32 lapic_read(u32 reg) {
    return *((volatile u32*)(HIGHER_HALF(0xfee00000) + reg));
}

u32 lapic_get_id() {
  return lapic_read(0x0020) >> LAPIC_ICDESTSHIFT;
}

void lapic_eoi() {
    lapic_write((u8)0xb0, 0x0);
}