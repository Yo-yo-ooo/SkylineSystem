#pragma once
#include <stdint.h>

enum tcr_shifts : uint8_t {
    TCR_INNER_CACHEABILITY_TTBR0_SHIFT = 8,
    TCR_OUTER_CACHEABILITY_TTBR0_SHIFT = 10,
    TCR_SHAREABILITY_ATTR_TTBR0_SHIFT = 12,
    TCR_TG0_SHIFT = 14,
    TCR_TTBR1_SIZE_OFFSET_SHIFT = 16,
    TCR_INNER_CACHEABILITY_TTBR1_SHIFT = 24,
    TCR_OUTER_CACHEABILITY_TTBR1_SHIFT = 26,
    TCR_SHAREABILITY_ATTR_TTBR1_SHIFT = 28,
    TCR_TG1_SHIFT = 30,
    TCR_IPS_SHIFT = 30,
};

enum tcr_inner_cacheability : uint8_t {
    TCR_INNER_CACHEABILITY_NONE,
    TCR_INNER_CACHEABILITY_WRITEBACK_READALLOC_WRITEALLOC,
    TCR_INNER_CACHEABILITY_WRITETHROUGH_READALLOC_NO_WRITEALLOC,
    TCR_INNER_CACHEABILITY_WRITEBACK_READALLOC_NO_WRITEALLOC,
};

enum tcr_outer_cacheability : uint8_t {
    TCR_OUTER_CACHEABILITY_NONE,
    TCR_OUTER_CACHEABILITY_WRITEBACK_READALLOC_WRITEALLOC,
    TCR_OUTER_CACHEABILITY_WRITETHROUGH_READALLOC_NO_WRITEALLOC,
    TCR_OUTER_CACHEABILITY_WRITEBACK_READALLOC_NO_WRITEALLOC,
};

enum tcr_shareability_attribute : uint8_t {
    TCR_SHAREABILITY_ATTR_NONE,
    TCR_SHAREABILITY_ATTR_OUTER,
    TCR_SHAREABILITY_ATTR_INNER,
};

enum tcr_el0_granule_size : uint8_t {
    TCR_EL0_GRANULE_16KIB = 1,
    TCR_EL0_GRANULE_4KIB,
    TCR_EL0_GRANULE_64KIB,
};

enum tcr_tg0_size : uint8_t {
    TCR_TG0_4KIB,
    TCR_TG0_64KIB,
    TCR_TG0_16KIB,
};

enum tcr_tg1_size : uint8_t {
    TCR_TG1_16KIB,
    TCR_TG1_4KIB,
    TCR_TG1_64KIB,
};

enum tcr_phys_addrspace_size : uint8_t {
    TCR_PHYS_ADDRSPACE_32BIT_4GIB,
    TCR_PHYS_ADDRSPACE_36BIT_64GIB,
    TCR_PHYS_ADDRSPACE_40BIT_1TIB,
    TCR_PHYS_ADDRSPACE_42BIT_4TIB,
    TCR_PHYS_ADDRSPACE_44BIT_16TIB,
    TCR_PHYS_ADDRSPACE_48BIT_256TIB,
    TCR_PHYS_ADDRSPACE_52BIT_4PIB,
};

enum tcr_flags : uint64_t {
    // The size offset of the memory region addressed by TTBR0_EL1.
    // The region size is 2^(64-T0SZ) bytes.
    __TCR_T0SZ = 0b11111ull,

    // A TLB miss on an address that is translated using TTBR0_EL1 generates a
    // Translation fault. No translation table walk is performed.
    __TCR_EPD0 = 1ull << 7,

    __TCR_IRGN0 = 0b11ull << TCR_INNER_CACHEABILITY_TTBR0_SHIFT,
    __TCR_ORGN0 = 0b11ull << TCR_OUTER_CACHEABILITY_TTBR0_SHIFT,
    __TCR_SH0 = 0b11ull << TCR_SHAREABILITY_ATTR_TTBR0_SHIFT,

    __TCR_TG0 = 0b11ull << TCR_TG0_SHIFT,

    // The size offset of the memory region addressed by TTBR1_EL1.
    // The region size is 2^(64-T0SZ) bytes.
    __TCR_T1SZ = 0b11111ull << TCR_TTBR1_SIZE_OFFSET_SHIFT,

    // Selects whether TTBR0_EL1 (0) or TTBR1_EL1 (1) defines the ASID
    __TCR_ASID_DEFINED_BY_EL1 = 1ull << 22,

    // Translation table walk disable for translations using TTBR1_EL1. This bit
    // controls whether a translation table walk is performed on a TLB miss, for
    // an address that is translated using TTBR1_EL1.
    // A TLB miss on an address that is translated using TTBR1_EL1 generates a
    // Translation fault. No translation table walk is performed.

    __TCR_EPD1 = 1ull << 23,
    __TCR_IRGN1 = 0b11ull << TCR_INNER_CACHEABILITY_TTBR1_SHIFT,
    __TCR_ORGN1 = 0b11ull << TCR_OUTER_CACHEABILITY_TTBR1_SHIFT,
    __TCR_SH1 = 0b11ull << TCR_SHAREABILITY_ATTR_TTBR1_SHIFT,

    __TCR_TG1 = 0b11ull << TCR_TG1_SHIFT,
    __TCR_IPS = 0b111ull << 32,

    // 16 bit - the upper 16 bits of TTBR0_EL1 and TTBR1_EL1 are used for
    // allocation and matching in the TLB.
    __TCR_AS = 1ull << 36,

    // Top Byte ignored. Indicates whether the top byte of an address is used
    // for address match for the TTBR0_EL1
    // region, or ignored and used for tagged addresses.
    __TCR_TBI_EL0 = 1ull << 37,

    // Top Byte ignored. Indicates whether the top byte of an address is used
    // for address match for the TTBR1_EL1 region, or ignored and used for
    // tagged addresses.
    __TCR_TBI_EL1 = 1ull << 38,

    // When FEAT_HAFDBS is implemented:
    // Hardware Access flag update in stage 1 translations from EL0 and EL1.
    // Stage 1 Access flag update enabled.
    __TCR_HW_ACCESS_FLAG_ENABLE = 1ull << 39,

    // When FEAT_HAFDBS is implemented:
    // Hardware management of dirty state in stage 1 translations from EL0 and
    // EL1.
    // Stage 1 hardware management of dirty state enabled, only if the HA bit is
    // also set to 1.
    __TCR_HW_DIRTY_FLAG_ENABLE = 1ull << 40,

    // When FEAT_HPDS is implemented:
    // Hierarchical Permission Disables. This affects the hierarchical control
    // bits, APTable, PXNTable, and UXNTable, except NSTable, in the translation
    // tables pointed to by TTBR0_EL1.
    // Hierarchical permissions are disabled.
    __TCR_HIERARCHIAL_PERMS_DISABLE_EL0 = 1ull << 41,

    // When FEAT_HPDS is implemented:
    // Hierarchical Permission Disables. This affects the hierarchical control
    // bits, APTable, PXNTable, and UXNTable, except NSTable, in the translation
    // tables pointed to by TTBR1_EL1
    // Hierarchical permissions are disabled.
    __TCR_HIERARCHIAL_PERMS_DISABLE_EL1 = 1ull << 42,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[59] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR0_EL1.

    // For translations using TTBR0_EL1, bit[59] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD0 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD0 is 0.
    __TCR_HWU059 = 1ull << 43,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[60] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR0_EL1.

    // For translations using TTBR0_EL1, bit[60] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD0 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD0 is 0.
    __TCR_HWU060 = 1ull << 44,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[61] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR0_EL1.

    // For translations using TTBR0_EL1, bit[61] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD0 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD0 is 0.
    __TCR_HWU061 = 1ull << 45,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[62] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR0_EL1.

    // For translations using TTBR0_EL1, bit[62] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD0 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD0 is 0.
    __TCR_HWU062 = 1ull << 46,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[59] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR1_EL1.

    // For translations using TTBR1_EL1, bit[59] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD1 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD1 is 0.
    __TCR_HWU159 = 1ull << 47,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[60] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR1_EL1.

    // For translations using TTBR1_EL1, bit[60] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD1 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD1 is 0.
    __TCR_HWU160 = 1ull << 48,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[61] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR1_EL1.

    // For translations using TTBR1_EL1, bit[61] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD1 is 1.

    // The Effective value of this field is 0 if the value of TCR_EL1.HPD1 is 0.
    __TCR_HWU161 = 1ull << 49,

    // When FEAT_HPDS2 is implemented:
    // Hardware Use. Indicates IMPLEMENTATION DEFINED hardware use of bit[62] of
    // the stage 1 translation table Block or Page entry for translations using
    // TTBR1_EL1.

    // For translations using TTBR1_EL1, bit[62] of each stage 1 translation
    // table Block or Page entry can be used by hardware for an IMPLEMENTATION
    // DEFINED purpose if the value of TCR_EL1.HPD1 is 1.
    __TCR_HWU162 = 1ull << 50,

    // When FEAT_PAuth is implemented:
    // Controls the use of the top byte of instruction addresses for address
    // matching.
    // For the purpose of this field, all cache maintenance and address
    // translation instructions that perform address translation are treated as
    // data accesses.

    // For more information, see 'Address tagging in AArch64 state'.
    // TCR_EL1.TBI0 applies to Data accesses only if 1, otherwise,
    // TCR_EL1.TBI0 applies to Instruction and Data accesses.

    // This affects addresses where the address would be translated by tables
    // pointed to by TTBR0_EL1.
    __TCR_TBID0 = 1ull << 51,

    // When FEAT_PAuth is implemented:
    // Controls the use of the top byte of instruction addresses for address
    // matching.

    // For the purpose of this field, all cache maintenance and address
    // translation instructions that perform address translation are treated as
    // data accesses.

    // For more information, see 'Address tagging in AArch64 state'.
    // TCR_EL1.TBI1 applies to Data accesses only if 1, otherwise,
    // TCR_EL1.TBI1 applies to Instruction and Data accesses.
    // This affects addresses where the address would be translated by tables
    // pointed to by TTBR1_EL1.
    __TCR_TBID1 = 1ull << 52,

    // When FEAT_SVE is implemented:
    // Non-fault translation table walk disable for stage 1 translations using
    // TTBR0_EL1.
    // This bit controls whether to perform a stage 1 translation table walk
    // in response to a non-fault unprivileged access for a virtual address
    // that is translated using TTBR0_EL1.
    // If SVE is implemented, the affected access types include:
    //   All accesses due to an SVE non-fault contiguous load instruction.
    //   Accesses due to an SVE first-fault gather load instruction that are not
    //      for the First active element. Accesses due to an SVE first-fault
    //      contiguous load instruction are not affected.
    //   Accesses due to prefetch instructions might be affected, but the effect
    //      is not architecturally visible.
    // For more information, see 'The Scalable Vector Extension (SVE)'.

    // A TLB miss on a virtual address that is translated using TTBR0_EL1 due to
    // the specified access types causes the access to fail without taking an
    // exception. No stage 1 translation table walk is performed.
    __TCR_NFD0 = 1ull << 53,

    // When FEAT_SVE is implemented:
    // Non-fault translation table walk disable for stage 1 translations using
    // TTBR1_EL1.
    // This bit controls whether to perform a stage 1 translation table walk in
    // response to a non-fault unprivileged access for a virtual address that is
    // translated using TTBR1_EL1.
    // If SVE is implemented, the affected access types include:
    //  All accesses due to an SVE non-fault contiguous load instruction.
    //  Accesses due to an SVE first-fault gather load instruction that are not
    //      for the First active element. Accesses due to an SVE first-fault
    //      contiguous load instruction are not affected.
    //  Accesses due to prefetch instructions might be affected, but the effect
    //      is not architecturally visible.
    // For more information, see 'The Scalable Vector Extension (SVE)'.

    // A TLB miss on a virtual address that is translated using TTBR1_EL1 due to
    // the specified access types causes the access to fail without taking an
    // exception. No stage 1 translation table walk is performed.
    __TCR_NFD1 = 1ull << 54,

    // When FEAT_E0PD is implemented:
    // Faulting control for Unprivileged access to any address translated by
    // TTBR0_EL1.

    // Unprivileged access to any address translated by TTBR0_EL1 will generate
    // a level 0 Translation fault.

    // Level 0 Translation faults generated as a result of this field are not
    // counted as TLB misses for performance monitoring. The fault should take
    // the same time to generate, whether the address is present in the TLB or
    // not, to mitigate attacks that use fault timing.
    __TCR_E0PD0 = 1ull << 55,

    // When FEAT_E0PD is implemented:
    // Faulting control for Unprivileged access to any address translated by
    // TTBR1_EL1.
    // Unprivileged access to any address translated by TTBR1_EL1 will generate
    // a level 0 Translation fault.
    // Level 0 Translation faults generated as a result of this field are not
    // counted as TLB misses for performance
    // monitoring. The fault should take the same time to generate, whether the
    // address is present in the TLB or not, to mitigate attacks that use fault
    // timing.
    __TCR_E0PD1 = 1ull << 56,

    // When FEAT_MTE2 is implemented:
    // Controls the generation of Unchecked accesses at EL1, and at EL0 if
    // HCR_EL2.{E2H,TGE}!={1,1}, when address[59:55] = 0b00000.
    // All accesses at EL1 and EL0 are Unchecked.
    // Software may change this control bit on a context switch.
    __TCR_TCMA0 = 1ull << 57,

    // When FEAT_MTE2 is implemented:
    // Controls the generation of Unchecked accesses at EL1, and at EL0 if
    // HCR_EL2.{E2H,TGE}!={1,1}, when address[59:55] = 0b11111.
    // This control has no effect on the generation of Unchecked accesses at EL1
    // or EL0.
    __TCR_TCMA1 = 1ull << 58,

    // When FEAT_LPA2 is implemented:
    // This field affects 52-bit output addressing when using 4KB and 16KB
    // translation granules in stage 1 of the EL1&0 translation regime.
    // This field is RES0 for a 64KB translation granule.

    // If 0:
    //  Bits[49:48] of translation descriptors are RES0.
    //  Bits[9:8] in Block and Page descriptors encode shareability information
    //  in the SH[1:0] field. Bits[9:8] in Table descriptors are ignored by
    //  hardware.
    //  The minimum value of the TCR_EL1.{T0SZ, T1SZ} fields is 16. Any memory
    //  access using a smaller value generates a stage 1 level 0 translation
    //  table fault.
    //  Output address[51:48] is 0b0000.
    //  Bits[49:48] of translation descriptors hold output address[49:48].
    //  Bits[9:8] of Translation table descriptors hold output address[51:50].
    //  The shareability information of Block and Page descriptors for cacheable
    //  locations is determined by:
    //  TCR_EL1.SH0 if the VA is translated using tables pointed to by TTBR0_EL1.
    //  TCR_EL1.SH1 if the VA is translated using tables pointed to by TTBR1_EL1.
    //  The minimum value of the TCR_EL1.{T0SZ, T1SZ} fields is 12. Any memory
    //  access using a smaller value generates a stage 1 level 0 translation
    //  table fault.
    // If 1:
    //  Bits[5:2] of TTBR0_EL1 or TTBR1_EL1 are used to hold bits[51:48] of the
    //  output address in all cases.
    //  As FEAT_LVA must be implemented if TCR_EL1.DS == 1, the minimum value of
    //  the TCR_EL1.{T0SZ, T1SZ} fields is 12, as determined by that extension.
    //  For the TLBI Range instructions affecting VA, the format of the argument
    //  is changed so that bits[36:0] hold BaseADDR[52:16]. For the 4KB
    //  translation granule, bits[15:12] of BaseADDR are treated as 0b0000. For
    //  the 16KB translation granule, bits[15:14] of BaseADDR are treated as
    //  0b00. This forces alignment of the ranges used by the TLBI range
    //  instructions.

    // All calculations of the stage 1 base address are modified for tables of
    // fewer than 8 entries so that the table is aligned to 64 bytes.
    __TCR_DS = 1ull << 59,
};

static inline uint64_t tcr_el1_read() {
    uint64_t result = 0;
    asm volatile ("mrs %0, tcr_el1" : "=r"(result));

    return result;
}

static inline void tcr_el1_write(const uint64_t value) {
    asm volatile ("msr tcr_el1, %0" :: "r"(value));
}