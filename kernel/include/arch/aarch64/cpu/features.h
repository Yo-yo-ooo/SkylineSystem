#pragma once
#include <arch/aarch64/asm/regs.h>

#define CPU_FEAT_NONE 0

enum cpu_feat_aes : uint8_t {
    CPU_FEAT_AES_NONE,
    CPU_FEAT_AES,
    CPU_FEAT_AES_PMULL
};

enum cpu_feat_sha2 : uint8_t {
    CPU_FEAT_SHA2_NONE,
    CPU_FEAT_SHA256,
    CPU_FEAT_SHA512
};

enum cpu_feat_atomic : uint8_t {
    CPU_FEAT_ATOMIC_NONE,
    CPU_FEAT_ATOMIC_LSE,
    CPU_FEAT_ATOMIC_LSE128
};

enum cpu_feat_ts : uint8_t {
    CPU_FEAT_TS_NONE,
    CPU_FEAT_TS_FLAG_M,
    CPU_FEAT_TS_FLAG_M2
};

enum cpu_feat_tlb : uint8_t {
    CPU_FEAT_TLB_NONE,
    CPU_FEAT_TLBIOS,
    CPU_FEAT_TLB_IRANGE,
};

enum cpu_feat_pauth : uint8_t {
    CPU_FEAT_PAUTH_NONE,
    CPU_FEAT_PAUTH,
    CPU_FEAT_PAUTH_EPAC,
    CPU_FEAT_PAUTH2,
    CPU_FEAT_PAUTH_FPACCOMBINE,
};

enum cpu_feat_lrcpc : uint8_t {
    CPU_FEAT_LRCPC_NONE,
    CPU_FEAT_LRCPC,
    CPU_FEAT_LRCPC2,
    CPU_FEAT_LRCPC3
};

enum cpu_feat_specres : uint8_t {
    CPU_FEAT_SPECRES_NONE,
    CPU_FEAT_SPECRES,
    CPU_FEAT_SPECRES2,
};

enum cpu_feat_bf16 : uint8_t {
    CPU_FEAT_BF16_NONE,
    CPU_FEAT_BF16,
    CPU_FEAT_EBF16,
};

enum cpu_feat_ls64 : uint8_t {
    CPU_FEAT_LS64_NONE,
    CPU_FEAT_LS64,
    CPU_FEAT_LS64V,
    CPU_FEAT_LS64_ACCDATA,
};

enum cpu_feat_fp : uint8_t {
    CPU_FEAT_FP_NONE,
    CPU_FEAT_FP_PARTIAL,
    CPU_FEAT_FP_FULL
};

enum cpu_feat_dpb : uint8_t {
    CPU_FEAT_DPB_NONE,
    CPU_FEAT_DPB,
    CPU_FEAT_DPB2,
};

enum cpu_feat_adv_simd : uint8_t {
    CPU_FEAT_ADV_SIMD_NONE,
    CPU_FEAT_ADV_SIMD_PARTIAL,
    CPU_FEAT_ADV_SIMD_FULL
};

enum cpu_feat_gic : uint8_t {
    CPU_FEAT_GIC_NONE,
    CPU_FEAT_GIC_V4,
    CPU_FEAT_GIC_V4p1,
};

enum cpu_feat_ras : uint8_t {
    CPU_FEAT_RAS_NONE,
    CPU_FEAT_RAS,
    CPU_FEAT_RASv1p1,
    CPU_FEAT_RASv2,
};

enum mpan_version : uint8_t {
    MPAN_VERSION_NONE,
    MPAN_VERSION_V0p1,
    MPAN_VERSION_V1p0,
    MPAN_VERSION_V1p1,
};

enum cpu_feat_amu : uint8_t {
    CPU_FEAT_AMU_NONE,
    CPU_FEAT_AMU_AMUv1,
    CPU_FEAT_AMU_AMUv1p1,
};

enum cpu_feat_csv2 : uint8_t {
    CPU_FEAT_CSV2_NONE,
    CPU_FEAT_CSV2,
    CPU_FEAT_CSV2_2,
    CPU_FEAT_CSV2_3,
};

enum cpu_feat_ssbs : uint8_t {
    CPU_FEAT_SSBS_NONE,
    CPU_FEAT_SSBS,
    CPU_FEAT_SSBS2
};

enum cpu_feat_mte : uint8_t {
    CPU_FEAT_MTE_NONE,
    CPU_FEAT_MTE,
    CPU_FEAT_MTE2,
    CPU_FEAT_MTE3,
    CPU_FEAT_MTEX
};

enum cpu_feat_sme : uint8_t {
    CPU_FEAT_SME_NONE,
    CPU_FEAT_SME,
    CPU_FEAT_SME2,
    CPU_FEAT_SME2p1
};

enum csv2_version : uint8_t {
    CSV2_VERSION_NONE,
    CSV2_VERSION_V0P1,
    CSV2_VERSION_V1P0,
    CSV2_VERSION_V1P1,
};

enum cpu_feat_fgt : uint8_t {
    CPU_FEAT_FGT_NONE,
    CPU_FEAT_FGT,
    CPU_FEAT_FGT2,
};

enum cpu_feat_ecv : uint8_t {
    CPU_FEAT_ECV_NONE,
    CPU_FEAT_ECV_PARTIAL,
    CPU_FEAT_ECV_FULL
};

enum cpu_feat_hafdbs : uint8_t {
    CPU_FEAT_HAFDBS_NONE,
    CPU_FEAT_HAFDBS_ONLY_ACCESS_BIT,
    CPU_FEAT_HAFDBS_ACCESS_AND_DIRTY_BIT,
    CPU_FEAT_HAFDBS_FULL,
};

enum cpu_feat_hpds : uint8_t {
    CPU_FEAT_HPDS_NONE,
    CPU_FEAT_HPDS,
    CPU_FEAT_HPDS2,
};

enum cpu_feat_pan : uint8_t {
    CPU_FEAT_PAN_NONE,
    CPU_FEAT_PAN,
    CPU_FEAT_PAN2,
    CPU_FEAT_PAN3,
};

enum cpu_feat_nv : uint8_t {
    CPU_FEAT_NV_NONE,
    CPU_FEAT_NV,
    CPU_FEAT_NV2
};

enum cpu_feat_bbm : uint8_t {
    CPU_FEAT_BBM_LVL0,
    CPU_FEAT_BBM_LVL1,
    CPU_FEAT_BBM_LVL2,
};

enum cpu_feat_evt : uint8_t {
    CPU_FEAT_EVT1,
    CPU_FEAT_EVT2
};

enum cpu_feat_sve : uint8_t {
    CPU_FEAT_SVE,
    CPU_FEAT_SVE2,
    CPU_FEAT_SVE2p1,
};

enum cpu_feat_sve_aes : uint8_t {
    CPU_FEAT_SVE_AES_NONE,
    CPU_FEAT_SVE_AES,
    CPU_FEAT_SVE_AES_PMULL128,
};

enum cpu_feat_sve_bf16 : uint8_t {
    CPU_FEAT_SVE_BF16_NONE,
    CPU_FEAT_SVE_BF16,
    CPU_FEAT_SVE_EBF16,
};

enum cpu_feat_brbe : uint8_t {
    CPU_FEAT_BRBE_NONE,
    CPU_FEAT_BRBE,
    CPU_FEAT_BRBEv1p1
};

enum cpu_feat_debug : uint8_t {
    CPU_FEAT_DEBUG_DEFAULT,
    CPU_FEAT_DEBUG_VHE,
    CPU_FEAT_DEBUG_v8p1 = CPU_FEAT_DEBUG_VHE,
    CPU_FEAT_DEBUG_v8p2,
    CPU_FEAT_DEBUG_v8p4,
    CPU_FEAT_DEBUG_v8p8,
    CPU_FEAT_DEBUG_v8p9,
};

enum cpu_feat_pmu : uint8_t {
    CPU_FEAT_PMU_FEAT_PMUv3,
    CPU_FEAT_PMU_FEAT_PMUv3p1,
    CPU_FEAT_PMU_FEAT_PMUv3p4,
    CPU_FEAT_PMU_FEAT_PMUv3p5,
    CPU_FEAT_PMU_FEAT_PMUv3p7,
    CPU_FEAT_PMU_FEAT_PMUv3p8,
    CPU_FEAT_PMU_FEAT_PMUv3p9,
};

enum cpu_feat_mtpmu : uint8_t {
    CPU_FEAT_MTPMU,
    CPU_FEAT_MTPMU_3_MAYBE,
};

struct cpu_features {
    // 2022 Architecture Extensions
    bool able : 1;                // Address Breakpoint Linking extension
    // Physical Fault Address Extension [NOTE: not yet listed]
    bool aderr : 1;
    // Hardware managed Access Flag for Table descriptors
    bool anerr : 1;
    bool aie : 1;                 // Memory Attribute Index Enhancement
    // Imposes restrictions on branch history speculation around exceptions
    bool b16b16 : 1;
    // A new instruction CLRBHB is added in HINT space
    bool clrbhb : 1;
    bool ebep : 1;                // Exception-based event profiling
    // Non-widening BFloat16 to BFloat16 arithmetic for SVE2.1 and SME2.1
    bool ecbhb : 1;
    bool gcs : 1;                 // Guarded Control Stack Extension
    // RASv2 Additional Error syndrome reporting, for Normal memory
    bool haft : 1;
    bool ite : 1;                 // Instrumentation trace extension
    bool mec : 1;                 // Memory Encryption Contexts
    bool s1pie : 1;               // Permission model enhancements
    bool s2pie : 1;               // Permission model enhancements
    bool s1poe : 1;               // Permission model enhancements
    bool s2poe : 1;               // Permission model enhancements
    bool pmuv3_icntr : 1;         // PMU instruction counter
    bool pmuv3_ss : 1;            // PMU snapshot
    bool prfmslc : 1;             // Prefetching enhancements
    // RASv2 Additional Error syndrome reporting, for Device memory
    bool pfar : 1;
    bool sebep : 1;               // Synchronous Exception-based event profiling
    bool spmu : 1;                // System PMU
    bool sysinstr128 : 1;         // 128-bit System instructions
    bool sysreg128 : 1;           // 128-bit System registers
    bool the : 1;                 // Translation Hardening Extension
    bool trbe_ext : 1;            // Represents TRBE external mode
    // 2021 Architecture Extensions
    bool cmow : 1;          // Control for cache maintenance permission
    bool hbc : 1;           // Hinted conditional branches
    bool hpmn0 : 1;         // Setting of MDCR_EL2.HPMN to zero
    bool nmi : 1;           // Non-maskable Interrupts
    bool mops : 1;          // Standardization of memory operations
    bool pacqarma3 : 1;     // Pointer authentication - QARMA3 algorithm
    bool rndr_trap : 1;     // Trapping support for RNDR and RNDRRS
    bool tidcp1 : 1;        // EL0 use of IMPLEMENTATION DEFINED functionality
    // 2020 Architecture Extensions
    bool afp : 1;          // Alternate floating-point behavior
    bool hcx : 1;          // Support for the HCRX_EL2 register
    // Increased precision of Reciprocal Estimate and Reciprocal Square Root
    // Estimate
    bool lpa2 : 1;
    // Larger physical address for 4KB and 16KB translation granules
    bool rpres : 1;
    bool rme : 1;          // Realm Management Extension
    bool sme_fa64 : 1;     // Additional instructions for the SME Extension
    bool sme_f64f64 : 1;   // Additional instructions for the SME Extension
    bool sme_i16i64 : 1;   // Additional instructions for the SME Extension
    bool sme_f16f16 : 1;   // Additional instructions for the SME Extension
    bool wfxt : 1;         // WFE and WFI instructions with timeout
    bool xs : 1;           // XS attribute

    enum cpu_feat_dpb dpb : 2;
    enum cpu_feat_adv_simd adv_simd : 2; // Advanced SIMD Extension
    enum cpu_feat_aes aes : 2;
    enum cpu_feat_sha2 sha2 : 2;
    enum cpu_feat_atomic atomic : 2;
    enum cpu_feat_ts ts : 2;
    enum cpu_feat_tlb tlb : 2;
    enum cpu_feat_pauth pauth : 3;
    enum cpu_feat_lrcpc lrcpc : 2;
    enum cpu_feat_fgt fgt : 2;
    enum cpu_feat_ls64 ls64 : 2;
    enum cpu_feat_fp fp : 2;
    enum cpu_feat_gic gic : 2;
    enum cpu_feat_ras ras : 2;
    enum cpu_feat_sve sve : 2;
    enum cpu_feat_sve_aes sve_aes : 2;
    enum cpu_feat_sve_bf16 sve_bf16 : 2;
    enum cpu_feat_amu amu : 2;
    enum cpu_feat_csv2 csv2 : 2;
    enum cpu_feat_specres specres : 2;
    enum cpu_feat_ssbs ssbs : 2;
    enum cpu_feat_mte mte : 3;
    enum cpu_feat_sme sme : 2;
    enum id_aa64mmfr0_pa_range pa_range : 3;
    enum cpu_feat_ecv ecv : 2;
    enum cpu_feat_hafdbs hafdbs : 3;
    enum cpu_feat_hpds hpds : 2;
    enum cpu_feat_pan pan : 2;
    enum cpu_feat_nv nv : 2;
    enum cpu_feat_bbm bbm : 2;
    enum cpu_feat_debug debug : 4;
    enum cpu_feat_pmu pmu : 3;
    enum cpu_feat_mtpmu mtpmu : 2;
    enum cpu_feat_brbe brbe : 2;
    enum cpu_feat_bf16 bf16 : 2;

    bool csv3 : 1;          // Cache Speculation Variant 3
    bool dgh : 1;           // Data Gathering Hint
    bool double_lock : 1;   // Double Lock
    bool ets : 1;           // Enhanced Translation Synchronization
    bool sb : 1;            // Speculation barrier
    bool sha1 : 1;          // Advanced SIMD SHA1 instructions
    bool crc32 : 1;         // CRC32 instructions
    bool ntlbpa : 1;        // No intermediate caching by output address in TLB
    bool lor : 1;           // Limited ordering regions
    bool rdm : 1;           // Rounding double multiply accumulate
    bool vhe : 1;           // Virtualization Host Extensions
    bool vmid16 : 1;        // 16-bit VMID
    bool dot_product : 1;   // Advanced SIMD Int8 dot product instructions
    bool evt : 1;           // Enhanced Virtualization Traps
    bool fhm : 1;           // Half-precision floating-point FMLAL instructions
    bool fp16 : 1;          // Half-precision floating-point data processing
    bool i8mm : 1;          // Int8 Matrix Multiplication
    bool iesb : 1;          // Implicit Error synchronization event
    bool lpa : 1;           // Large PA and IPA support
    // Pointer authentication - IMPLEMENTATION DEFINED algorithm
    bool lsmaoc : 1;
    bool lva : 1;           // Large VA support
    bool mpam : 1;          // Memory Partitioning and Monitoring
    bool pcsrv8p2 : 1;      // PC Sample-based profiling version 8.2
    // Execute-never control distinction by Exception level at stage 2
    bool sha3 : 1;
    // Advanced SIMD SM4 instructions; Split into SM3 and SM4
    bool sm3 : 1;
    // Advanced SIMD SM3 instructions; Split into SM3 and SM4
    bool sm4 : 1;
    bool ttcnp : 1;         // Common not private translations
    // Advanced SIMD EOR3, RAX1, XAR, and BCAX instructions; Split ARMv8.2-SHA
    // into SHA-256, SHA-512 and SHA-3
    bool xnx : 1;
    bool uao : 1;           // Unprivileged Access Override control
    bool ccidx : 1;         // Extended cache index
    bool fcma : 1;          // Floating-point FCMLA and FCADD instructions
    bool jscvt : 1;         // JavaScript FJCVTS conversion instruction
    bool pacqarma5 : 1;     // Pointer authentication - QARMA5 algorithm
    // Load/Store instruction multiple atomicity and ordering controls
    bool pacimp : 1;
    bool dit : 1;           // Data Independent Timing instructions
    bool idst : 1;          // ID space trap handling
    bool lse2 : 1;          // Large System Extensions version 2
    bool s2fwb : 1;         // Stage 2 forced write-back
    bool sel2 : 1;          // Secure EL2
    bool trf : 1;           // Self hosted Trace Extensions
    bool ttl : 1;           // Translation Table Level
    bool ttst : 1;          // Small translation tables
    bool bti : 1;           // Branch target identification
    // FRINT32Z, FRINT32X, FRINT64Z, and FRINT64X instructions
    bool exs : 1;
    bool e0pd : 1;          // Preventing EL0 access to halves of address maps
    // Disabling context synchronizing exception entry and exit
    bool frintts : 1;
    bool rndr : 1;          // Random number generator
    bool twed : 1;          // Delayed trapping of WFE
    // Note: cf. https://developer.arm.com/documentation/ihi0069/h/?lang=en
    bool ete : 1;          // Embedded Trace Extension

    // SVE PMULL instructions; SVE2-AES is split into AES and PMULL support
    bool sve_pmull128 : 1;
    bool sve_bitperm : 1;  // SVE Bit Permute
    bool sve_sha3 : 1;     // SVE SHA-3 instructions
    bool sve_sm4 : 1;      // SVE SM4 instructions
    bool tme : 1;          // Transactional Memory Extension
    bool trbe : 1;         // Trace Buffer Extension

    bool mixed_endian_support : 1;
    bool granule_16k_supported : 1;
    bool granule_64k_supported : 1;
};

extern struct cpu_features g_cpu_features;
void CollectCPUFeatures();