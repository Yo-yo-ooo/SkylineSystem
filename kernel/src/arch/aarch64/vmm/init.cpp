#include <arch/aarch64/asm/mair.h>
#include <arch/aarch64/vmm/vmm.h>
#include <klib/kprintf.h>

void setup_mair() {
    // Device nGnRnE
    const uint64_t device_uncacheable_encoding = 0b00000000;
    // Device GRE
    const uint64_t device_write_combining_encoding = 0b00001100;
    // Normal memory, inner and outer non-cacheable
    const uint64_t memory_uncacheable_encoding = 0b01000100;
    // Normal memory inner and outer writethrough, non-transient
    const uint64_t memory_writethrough_encoding = 0b10111011;
    // Normal memory, inner and outer write-back, non-transient
    const uint64_t memory_write_back_encoding = 0b11111111;

    const uint64_t mair_value =
        memory_write_back_encoding
        | device_write_combining_encoding << 8
        | memory_writethrough_encoding << 16
        | device_uncacheable_encoding << 24
        | memory_uncacheable_encoding << 32;

    write_mair_el1(mair_value);
}

namespace VMM
{
    void Init(){
        setup_mair();
        kpokln("Setup Mair!");
        //TODO: VMM::INIT
    }
} // namespace VMM
