#include <arch/aarch64/asm/mair.h>
#include <arch/aarch64/asm/regs.h>
#include <arch/aarch64/vmm/vmm.h>
#include <mem/pmm.h>
#include <klib/kprintf.h>
#include <pdef.h>
#include <klib/kio.h>
#include <klib/klib.h>
#include <conf.h>



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

uint64_t V2P_4K(uint64_t vaddr){
    uint64_t ttbr;
    if(vaddr <= 0x0000FFFFFFFFFFFF)
        read_ttbr_el1(0,ttbr);
    else
        read_ttbr_el1(1,ttbr);
    Translate4K t;t.vaddr = vaddr;
    __tdesc_4K *lv0 = (__tdesc_4K*)
                    (ttbr + (uint64_t)t.addrdesc.LV0_TblIndex * sizeof(__tdesc_4K));
    TableDesc4KLV1 *lv1 = (TableDesc4KLV1*)
                    ((uint64_t)lv0->NextLvl + t.addrdesc.LV1_TblIndex * sizeof(__tdesc_4K));
    if(lv1->desc.Type == 0){
        lv1->lv1desc.OutputAddr | t.addrdesc.BlockOffset;
    }
    TableDesc4KLV2 *lv2 = (TableDesc4KLV2*)
                    ((uint64_t)lv1->desc.NextLvl + t.addrdesc.LV2_TblIndex * sizeof(__tdesc_4K));
    if(lv2->desc.Type == 0){
        
    }
    PageDescriptor4K *Page = (PageDescriptor4K*)
                    ((uint64_t)lv2->desc.NextLvl + t.addrdesc.LV3_TblIndex * sizeof(PageDescriptor4K));

    return (Page->fields.OutputAddr << 12) | t.addrdesc.BlockOffset;
}

uint64_t V2P_16K(uint64_t vaddr){
    uint64_t ttbr;
    if(vaddr <= 0x0000FFFFFFFFFFFF)
        read_ttbr_el1(0,ttbr);
    else
        read_ttbr_el1(1,ttbr);
    Translate16K t;t.vaddr = vaddr;
    __tdesc_16K *lv0 = (__tdesc_16K*)
                        (ttbr + t.addrdesc.LV0_TblIndex * sizeof(__tdesc_16K));
    __tdesc_16K *lv1 = (__tdesc_16K*)
                        ((uint64_t)lv0->NextLvl + t.addrdesc.LV1_TblIndex * sizeof(__tdesc_16K));
    __tdesc_16K *lv2 = (__tdesc_16K*)
                        ((uint64_t)lv1->NextLvl + t.addrdesc.LV2_TblIndex * sizeof(__tdesc_16K));
    
    PageDescriptor64K *Page = (PageDescriptor64K*)
                    ((uint64_t)lv2->NextLvl + t.addrdesc.LV3_TblIndex * sizeof(PageDescriptor64K));
    return (Page->fields.OutputAddr << 14) | t.addrdesc.BlockOffset;
}

uint64_t V2P_64K(uint64_t vaddr){
    uint64_t ttbr;
    if(vaddr <= 0x0000FFFFFFFFFFFF)
        read_ttbr_el1(0,ttbr);
    else
        read_ttbr_el1(1,ttbr);
    Translate64K t;t.vaddr = vaddr;
    __tdesc_64K *lv1 = (__tdesc_64K*)
                        (ttbr + t.addrdesc.LV1_TblIndex * sizeof(__tdesc_64K));
    __tdesc_64K *lv2 = (__tdesc_64K*)
                        (lv1->NextLvl + t.addrdesc.LV2_TblIndex * sizeof(__tdesc_64K));
    PageDescriptor64K *Page = (PageDescriptor64K*)
                    (lv2->NextLvl + t.addrdesc.LV3_TblIndex * sizeof(PageDescriptor64K));
    return (Page->fields.OutputAddr << 16) | t.addrdesc.BlockOffset;
}

typedef struct PageMap{
    uint64_t *LowerRoot; 
    uint64_t *HigherRoot;
}PageMap;

namespace VMM
{
    volatile PageMap KernelPageMap;
    void Init(){
        setup_mair();
        kpokln("Setup Mair!");
        //TODO: VMM::INIT
        const uint64_t lower_root = PMM::Request();
        const uint64_t higher_root = PMM::Request();
        KernelPageMap.LowerRoot = HIGHER_HALF(lower_root);
        KernelPageMap.HigherRoot = HIGHER_HALF(higher_root);

        
    }

    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr){
        
    }
} // namespace VMM
