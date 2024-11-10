#include "pit.h"
#include "../lapic/lapic.h"
#include "../../../klib/klib.h"

u64 pit_ticks = 0;

void PIT_Handler(registers* r) {
    pit_ticks++;
    
    lapic_eoi();
}

namespace PIT{
    
    

    void Init() {
        outb(0x43, 0x36);
        u16 div = (u16)(1193180 / PIT_FREQ);
        outb(0x40, (u8)div);
        outb(0x40, (u8)(div >> 8));
        irq_register(0, (void *)PIT_Handler);
    }

    void Sleep(u64 ms) {
        u64 start = pit_ticks;
        while (pit_ticks - start < ms) {
            __asm__ volatile ("nop");
        }
    }
}