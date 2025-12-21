#include <arch/aarch64/asm/mair.h>
#include <arch/aarch64/asm/regs.h>
#include <arch/aarch64/vmm/vmm.h>
#include <klib/kprintf.h>
#include <pdef.h>
#include <klib/kio.h>

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

    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr){
        // Get Current Exception Level
        uint16_t *high = (uint16_t*)&vaddr;
        uint64_t ttbr_;
        if(high == 0b1111111111111111)
            read_ttbr_el1(1,ttbr_);
        else if(high == 0b0000000000000000)
            read_ttbr_el1(0,ttbr_);
        else
            return -1;
        uint64_t pa;
        ((uint16_t*)&pa)[4] = high[4];
        
    }
} // namespace VMM
