#pragma once
#include <stdint.h>
#include <conf.h>
#include <pdef.h>

enum id_aa64isar0_el1_shifts : uint8_t {
    ID_AA64ISAR0_EL1_AES_SUPPORT_SHIFT = 4,
    ID_AA64ISAR0_EL1_SHA2_SUPPORT_SHIFT = 12,
    ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_SHIFT = 20,
    ID_AA64ISAR0_EL1_TS_SUPPORT_SHIFT = 52,
    ID_AA64ISAR0_EL1_TLB_SUPPORT_SHIFT = 56,
};

enum id_aa64isar0_el1_aes_support : uint8_t {
    ID_AA64ISAR0_EL1_AES_SUPPORT_NONE,
    // AESE, AESD, AESMC, and AESIMC instructions implemented.
    ID_AA64ISAR0_EL1_AES_SUPPORT_ONLY_AES,
    // As for 0b0001, plus PMULL and PMULL2 instructions operating on 64-bit
    // source elements.
    ID_AA64ISAR0_EL1_AES_SUPPORT_AES_AND_PMULL
};

enum id_aa64isar0_el1_sha2_support : uint8_t {
    ID_AA64ISAR0_EL1_SHA2_SUPPORT_NONE,
    // Implements instructions: SHA256H, SHA256H2, SHA256SU0, and SHA256SU1.
    ID_AA64ISAR0_EL1_SHA2_SUPPORT_SHA256,
    // Implements instructions:
    //   SHA256H, SHA256H2, SHA256SU0, and SHA256SU1.
    //   SHA512H, SHA512H2, SHA512SU0, and SHA512SU1.
    ID_AA64ISAR0_EL1_SHA2_SUPPORT_SHA512,
};

enum id_aa64isar0_el1_atomic_support : uint8_t {
    ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_NONE,
    // LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX, LDUMIN, CAS, CASP,
    // and SWP instructions implemented.
    ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_LSE,
    // As for 0b0010, plus 128-bit instructions LDCLRP, LDSETP and SWPP.
    ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_LSE128,
};

enum id_aa64isar0_el1_ts_support : uint8_t {
    ID_AA64ISAR0_EL1_TS_SUPPORT_NONE,
    // CFINV, RMIF, SETF16, and SETF8 instructions are implemented.
    ID_AA64ISAR0_EL1_TS_SUPPORT_FLAG_M,
    // CFINV, RMIF, SETF16, SETF8, AXFLAG, and XAFLAG instructions are
    // implemented.
    ID_AA64ISAR0_EL1_TS_SUPPORT_FLAG_M2,
};

enum id_aa64isar0_el1_tlb_support : uint64_t {
    ID_AA64ISAR0_EL1_TLB_SUPPORT_NONE,
    // Outer Shareable TLB maintenance instructions are implemented.
    ID_AA64ISAR0_EL1_TLB_SUPPORT_TLBIOS = 1ull << 56,
    // CFINV, RMIF, SETF16, SETF8, AXFLAG, and XAFLAG instructions are
    // implemented.
    ID_AA64ISAR0_EL1_TLB_SUPPORT_IRANGE,
};

enum id_aa64isar0_el1_el1_flags : uint64_t {
    __ID_AA64ISAR0_EL1_AES = 0b1111ull << 4,
    // SHA1C, SHA1P, SHA1M, SHA1H, SHA1SU0, and SHA1SU1 instructions
    // implemented.
    __ID_AA64ISAR0_EL1_SHA1 = 0b1111ull << 8,
    __ID_AA64ISAR0_EL1_SHA2 = 0b1111ull << 12,
    // CRC32B, CRC32H, CRC32W, CRC32X, CRC32CB, CRC32CH, CRC32CW, and CRC32CX
    // instructions are implemented.
    __ID_AA64ISAR0_EL1_CRC32 = 0b1111ull << 16,
    __ID_AA64ISAR0_EL1_ATOMIC = 0b1111ull << 20,
    // TCANCEL, TCOMMIT, TSTART, and TTEST instructions are implemented.
    __ID_AA64ISAR0_EL1_TME = 0b1111ull << 24,
    // SQRDMLAH and SQRDMLSH instructions implemented.
    __ID_AA64ISAR0_EL1_RDM = 0b1111ull << 28,
    // EOR3, RAX1, XAR, and BCAX instructions implemented.
    __ID_AA64ISAR0_EL1_SHA3 = 0b1111ull << 32,
    // SM3SS1, SM3TT1A, SM3TT1B, SM3TT2A, SM3TT2B, SM3PARTW1, and SM3PARTW2
    // instructions implemented.
    __ID_AA64ISAR0_EL1_SM3 = 0b1111ull << 36,
    // SM4E and SM4EKEY instructions implemented.
    __ID_AA64ISAR0_EL1_SM4 = 0b1111ull << 40,
    // UDOT and SDOT instructions implemented.
    __ID_AA64ISAR0_EL1_DP = 0b1111ull << 44,
    // FMLAL and FMLSL instructions are implemented.
    __ID_AA64ISAR0_EL1_FHM = 0b1111ull << 48,
    __ID_AA64ISAR0_EL1_TS = 0b1111ull << 52,
    __ID_AA64ISAR0_EL1_TLB = 0b1111ull << 56,
    // RNDR and RNDRRS registers are implemented.
    __ID_AA64ISAR0_EL1_RNDR = 0b1111ull << 60,
};

enum id_aa64isar1_el1_shifts : uint8_t {
    ID_AA64ISAR1_EL1_DPB_SUPPORT_SHIFT = 0,
    ID_AA64ISAR1_EL1_APA_SUPPORT_SHIFT = 4,
    ID_AA64ISAR1_EL1_API_SUPPORT_SHIFT = 8,
    ID_AA64ISAR1_EL1_LRCPC_SUPPORT_SHIFT = 20,
    ID_AA64ISAR1_EL1_SPECRES_SUPPORT_SHIFT = 40,
    ID_AA64ISAR1_EL1_BF16_SUPPORT_SHIFT = 44,
    ID_AA64ISAR1_EL1_LS64_SUPPORT_SHIFT = 60,
};

enum id_aa64isar1_el1_dpb_support : uint8_t {
    ID_AA64ISAR1_EL1_DPB_SUPPORT_NONE,
    // DC CVAP supported.
    ID_AA64ISAR1_EL1_DPB_SUPPORT_FEAT_DPB,
    // DC CVAP and DC CVADP supported.
    ID_AA64ISAR1_EL1_DPB_SUPPORT_FEAT_DPB2,
};

enum id_aa64isar1_el1_apa_support : uint8_t {
    ID_AA64ISAR1_EL1_APA_SUPPORT_NONE,
    // Address Authentication using the QARMA5 algorithm is implemented, with
    // the HaveEnhancedPAC() and HaveEnhancedPAC2() functions returning FALSE.
    ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_PAUTH,
    // Address Authentication using the QARMA5 algorithm is implemented, with
    // the HaveEnhancedPAC() function returning TRUE and the HaveEnhancedPAC2()
    // function returning FALSE.
    ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_EPAC,
    // Address Authentication using the QARMA5 algorithm is implemented, with
    // the HaveEnhancedPAC2() function returning TRUE, the HaveFPAC() function
    // returning FALSE, the HaveFPACCombined() function returning FALSE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_PAUTH2,
    // Address Authentication using the QARMA5 algorithm is implemented, with
    // the HaveEnhancedPAC2() function returning TRUE, the HaveFPAC() function
    // returning TRUE, the HaveFPACCombined() function returning FALSE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_FPAC,
    // Address Authentication using the QARMA5 algorithm is implemented, with
    // the HaveEnhancedPAC2() function returning TRUE, the HaveFPAC() function
    // returning TRUE, the HaveFPACCombined() function returning TRUE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_FPACCOMBINE
};

enum id_aa64isar1_el1_api_support : uint8_t {
    ID_AA64ISAR1_EL1_API_SUPPORT_NONE,
    // Address Authentication using an IMPLEMENTATION DEFINED algorithm is
    // implemented, with the HaveEnhancedPAC() and HaveEnhancedPAC2() functions
    // returning FALSE.
    ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_PAUTH,
    // Address Authentication using an IMPLEMENTATION DEFINED algorithm is
    // implemented, with the HaveEnhancedPAC() function returning TRUE, and the
    // HaveEnhancedPAC2() function returning FALSE.
    ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_EPAC,
    // Address Authentication using an IMPLEMENTATION DEFINED algorithm is
    // implemented, with the HaveEnhancedPAC2() function returning TRUE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_PAUTH2,
    // Address Authentication using an IMPLEMENTATION DEFINED algorithm is
    // implemented, with the HaveEnhancedPAC2() function returning TRUE, the
    // HaveFPAC() function returning TRUE, the HaveFPACCombined() function
    // returning FALSE, and the HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_FPAC,
    // Address Authentication using an IMPLEMENTATION DEFINED algorithm is
    // implemented, with the HaveEnhancedPAC2() function returning TRUE, the
    // HaveFPAC() function returning TRUE, the HaveFPACCombined() function
    // returning TRUE, and the HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_FPACCOMBINE
};

enum id_aa64isar1_el1_lrcpc_support : uint8_t {
    ID_AA64ISAR1_EL1_LRCPC_SUPPORT_NONE,
    // The no offset LDAPR, LDAPRB, and LDAPRH instructions are implemented.
    ID_AA64ISAR1_EL1_LRCPC_SUPPORT_FEAT_LRCPC,
    // As 0b0001, and the LDAPR (unscaled immediate) and STLR (unscaled
    // immediate) instructions are implemented.
    ID_AA64ISAR1_EL1_LRCPC_SUPPORT_FEAT_LRCPC2,
    // As 0b0010, and the post-index LDAPR, LDIAPP, STILP, and pre-index STLR
    // instructions are implemented.
    // If Advanced SIMD and floating-point is implemented, then the LDAPUR
    // (SIMD&FP), LDAP1 (SIMD&FP), STLUR (SIMD&FP), and STL1 (SIMD&FP)
    // instructions are implemented in Advanced SIMD and floating-point.
    ID_AA64ISAR1_EL1_LRCPC_SUPPORT_FEAT_LRCPC3,
};

enum id_aa64isar1_el1_specres_support : uint8_t {
    ID_AA64ISAR1_EL1_SPECRES_SUPPORT_NONE,
    // CFP RCTX, DVP RCTX and CPP RCTX instructions are implemented.
    ID_AA64ISAR1_EL1_SPECRES_SUPPORT_FEAT_SPECRES,
    // As 0b0001, and COSP RCTX instruction is implemented.
    ID_AA64ISAR1_EL1_SPECRES_SUPPORT_FEAT_SPECRES2,
};

enum id_aa64isar1_el1_bf16_support : uint8_t {
    ID_AA64ISAR1_EL1_BF16_SUPPORT_NONE,
    // BFCVT, BFCVTN, BFCVTN2, BFDOT, BFMLALB, BFMLALT, and BFMMLA instructions
    // are implemented.
    ID_AA64ISAR1_EL1_BF16_SUPPORT_FEAT_BF16,
    // As 0b0001, but the FPCR.EBF field is also supported.
    ID_AA64ISAR1_EL1_BF16_SUPPORT_FEAT_EBF16,
};

enum id_aa64isar1_el1_ls64_support : uint8_t {
    ID_AA64ISAR1_EL1_LS64_SUPPORT_NONE,
    // The LD64B and ST64B instructions are supported.
    ID_AA64ISAR1_EL1_LS64_SUPPORT_FEAT_LS64,
    // The LD64B, ST64B, and ST64BV instructions, and their associated traps are
    // supported.
    ID_AA64ISAR1_EL1_LS64_SUPPORT_FEAT_LS64_V,
    // The LD64B, ST64B, ST64BV, and ST64BV0 instructions, the ACCDATA_EL1
    // register, and their associated traps are supported.
    ID_AA64ISAR1_EL1_LS64_SUPPORT_FEAT_LS64_ACCDATA,
};

enum id_aa64isar1_el1_flags : uint64_t {
    __ID_AA64ISAR1_EL1_DPB = 0b1111ull << 0,
    __ID_AA64ISAR1_EL1_APA = 0b1111ull << 4,
    __ID_AA64ISAR1_EL1_API = 0b1111ull << 8,
    // The FJCVTZS instruction is implemented.
    __ID_AA64ISAR1_EL1_JSCVT = 0b1111ull << 12,
    // The FCMLA and FCADD instructions are implemented.
    __ID_AA64ISAR1_EL1_FCMA = 0b1111ull << 16,
    __ID_AA64ISAR1_EL1_LRCPC = 0b1111ull << 20,
    // Generic Authentication using the QARMA5 algorithm is implemented. This
    // includes the PACGA instruction.
    __ID_AA64ISAR1_EL1_GPA = 0b1111ull << 24,
    // Generic Authentication using an IMPLEMENTATION DEFINED algorithm is
    // implemented. This includes the PACGA instruction.
    __ID_AA64ISAR1_EL1_GPI = 0b1111ull << 28,
    // FRINT32Z, FRINT32X, FRINT64Z, and FRINT64X instructions are implemented.
    __ID_AA64ISAR1_EL1_FRINTTS = 0b1111ull << 32,
    // SB instruction is implemented.
    __ID_AA64ISAR1_EL1_SB = 0b1111ull << 36,
    // CFP RCTX, DVP RCTX and CPP RCTX instructions are implemented.
    __ID_AA64ISAR1_EL1_SPECRES = 0b1111ull << 40,
    __ID_AA64ISAR1_EL1_BF16 = 0b1111ull << 44,
    // Data Gathering Hint is implemented.
    __ID_AA64ISAR1_EL1_DGH = 0b1111ull << 48,
    // SMMLA, SUDOT, UMMLA, USMMLA, and USDOT instructions are implemented.
    __ID_AA64ISAR1_EL1_I8MM = 0b1111ull << 52,
    // The XS attribute, the TLBI and DSB instructions with the nXS qualifier,
    // and the HCRX_EL2.{FGTnXS, FnXS} fields are supported.
    __ID_AA64ISAR1_EL1_XS = 0b1111ull << 56,
    __ID_AA64ISAR1_EL1_LS64 = 0b1111ull << 60,
};

enum id_aa64isar2_el1_shifts : uint8_t {
    ID_AA64ISAR2_EL1_APA3_SUPPORT_SHIFT = 12
};

enum id_aa64isar2_el1_apa3_support : uint8_t {
    ID_AA64ISAR2_EL1_APA3_SUPPORT_NONE,
    // Address Authentication using the QARMA3 algorithm is implemented, with
    // the HaveEnhancedPAC() and HaveEnhancedPAC2() functions returning FALSE.
    ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_PAUTH,
    // Address Authentication using the QARMA3 algorithm is implemented, with
    // the HaveEnhancedPAC() function returning TRUE and the HaveEnhancedPAC2()
    // function returning FALSE.
    ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_EPAC,
    // Address Authentication using the QARMA3 algorithm is implemented, with
    // the HaveEnhancedPAC2() function returning TRUE, the HaveFPAC() function
    // returning FALSE, the HaveFPACCombined() function returning FALSE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_PAUTH2,
    // Address Authentication using the QARMA3 algorithm is implemented, with
    // the HaveEnhancedPAC2() function returning TRUE, the HaveFPAC() function
    // returning TRUE, the HaveFPACCombined() function returning FALSE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_FPAC,
    // Address Authentication using the QARMA3 algorithm is implemented, with
    // the HaveEnhancedPAC2() function returning TRUE, the HaveFPAC() function
    // returning TRUE, the HaveFPACCombined() function returning TRUE, and the
    // HaveEnhancedPAC() function returning FALSE.
    ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_FPACCOMBINE,
};

enum id_aa64isar2_el1_flags : uint64_t {
    // WFET and WFIT are supported, and the register number is reported in the
    // ESR_ELx on exceptions.
    __ID_AA64ISAR2_EL1_WFxT = 0b1111ull << 0,
    // Reciprocal and reciprocal square root estimates give 12 bits of mantissa,
    // when FPCR.AH is 1.
    __ID_AA64ISAR2_EL1_RPRES = 0b1111ull << 4,
    // Generic Authentication using the QARMA3 algorithm is implemented. This
    // includes the PACGA instruction.
    __ID_AA64ISAR2_EL1_GPA3 = 0b1111ull << 8,
    __ID_AA64ISAR2_EL1_APA3 = 0b1111ull << 12,
    // The Memory Copy and Memory Set instructions are implemented in AArch64
    // state with the following exception. If FEAT_MTE is implemented, then
    // SETGP*, SETGM* and SETGE* instructions are also supported.
    __ID_AA64ISAR2_EL1_MOPS = 0b1111ull << 16,
    // BC instruction is implemented.
    __ID_AA64ISAR2_EL1_BC = 0b1111ull << 20,
    // ConstPACField() returns TRUE.
    __ID_AA64ISAR2_EL1_PAC_frac = 0b1111ull << 24,
    // CLRBHB instruction is implemented.
    __ID_AA64ISAR2_EL1_CLRBHB = 0b1111ull << 28,
    // Instructions to access 128-bit System Registers are supported.
    __ID_AA64ISAR2_EL1_SYSREG_128 = 0b1111ull << 32,
    // System instructions that can take 128-bit inputs are supported.
    __ID_AA64ISAR2_EL1_SYSINSTR_128 = 0b1111ull << 36,
    // The PRFM instructions support the SLC target.
    __ID_AA64ISAR2_EL1_PRFMSLC = 0b1111ull << 40,
    // RPRFM hint instruction is implemented.
    __ID_AA64ISAR2_EL1_RPRFM = 0b1111ull << 48,
    // Common short sequence compression instructions are implemented.
    __ID_AA64ISAR2_EL1_CSSC = 0b1111ull << 52,
};

enum id_aa64pfr0_el1_shifts : uint8_t {
    ID_AA64PFR0_EL1_FP_SUPPORT_SHIFT = 16,
    ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_SHIFT = 20,
    ID_AA64PFR0_EL1_GIC_SUPPORT_SHIFT = 24,
    ID_AA64PFR0_EL1_RAS_SUPPORT_SHIFT = 28,
    ID_AA64PFR0_EL1_MPAM_SUPPORT_SHIFT = 40,
    ID_AA64PFR0_EL1_AMU_SUPPORT_SHIFT = 44,
    ID_AA64PFR0_EL1_CSV2_SUPPORT_SHIFT = 56,
};

enum id_aa64pfr0_el1_fp_support : uint8_t {
    // Floating-point is implemented, and includes support for:
    //   Single-precision and double-precision floating-point types.
    //   Conversions between single-precision and half-precision data types, and
    //   double-precision and half-precision data types.
    ID_AA64PFR0_EL1_FP_SUPPORT_PARTIAL,
    // As for 0b0000, and also includes support for half-precision
    // floating-point arithmetic.
    ID_AA64PFR0_EL1_FP_SUPPORT_FULL,
    // Floating-point is not implemented.
    ID_AA64PFR0_EL1_FP_SUPPORT_NONE,
};

enum id_aa64pfr0_el1_adv_simd_support : uint8_t {
    // Advanced SIMD is implemented, including support for the following SISD
    // and SIMD operations:
    //   Integer byte, halfword, word and doubleword element operations.
    //   Single-precision and double-precision floating-point arithmetic.
    //   Conversions between single-precision and half-precision data types, and
    //   double-precision and half-precision data types.
    ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_PARTIAL,
    // As for 0b0000, and also includes support for half-precision
    // floating-point arithmetic.
    ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_FULL,
    // Advanced SIMD is not implemented.
    ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_NONE,
};

enum id_aa64pfr0_el1_gic_support : uint8_t {
    ID_AA64PFR0_EL1_GIC_SUPPORT_NONE,
    // System register interface to versions 3.0 and 4.0 of the GIC CPU
    // interface is supported.
    ID_AA64PFR0_EL1_GIC_SUPPORT_V4,
    // System register interface to version 4.1 of the GIC CPU interface is
    // supported.
    ID_AA64PFR0_EL1_GIC_SUPPORT_V4p1
};

enum id_aa64pfr0_el1_ras_support : uint8_t {
    ID_AA64PFR0_EL1_RAS_SUPPORT_NONE,
    // RAS Extension implemented.
    ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RAS,
    // FEAT_RASv1p1 implemented and, if EL3 is implemented, FEAT_DoubleFault
    // implemented. As 0b0001, and adds support for:
    //   If EL3 is implemented, FEAT_DoubleFault.
    //   Additional ERXMISC<m>_EL1 System registers.
    //   Additional System registers ERXPFGCDN_EL1, ERXPFGCTL_EL1, and
    //     ERXPFGF_EL1, and the SCR_EL3.FIEN and HCR_EL2.FIEN trap controls, to
    //     support the optional RAS Common Fault Injection Model Extension.
    // Error records accessed through System registers conform to RAS System
    // Architecture v1.1, which includes simplifications to ERR<n>STATUS and
    // support for the optional RAS Timestamp and RAS Common Fault Injection
    // Model Extensions.
    ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RASv1p1,
    ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_DOUBLE_FAULT =
        ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RASv1p1,
    // FEAT_RASv2 implemented. As 0b0010 and adds support for:
    //   ERXGSR_EL1, to support System RAS agents.
    //   Additional fine-grained EL2 traps for additional error record System
    //     registers.
    //   The SCR_EL3.TWERR write control for error record System registers.
    // Error records accessed through System registers conform to RAS System
    // Architecture v2.
    ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RASv2,
};

enum id_aa64pfr0_el1_mpan_version : uint8_t {
    ID_AA64PFR0_EL1_MPAN_VERSION_NONE,
    // The major version number of the MPAM extension is 1.
    ID_AA64PFR0_EL1_MPAN_VERSION_V1,
};

enum id_aa64pfr0_el1_amu_support : uint8_t {
    // System register interface to versions 3.0 and 4.0 of the GIC CPU
    // interface is supported.
    ID_AA64PFR0_EL1_AMU_SUPPORT_NONE,
    // System register interface to version 4.1 of the GIC CPU interface is
    // supported.
    ID_AA64PFR0_EL1_AMU_SUPPORT_FEAT_AMUv1,
    // FEAT_AMUv1p1 is implemented. As 0b0001 and adds support for
    // virtualization of the activity monitor event counters.
    ID_AA64PFR0_EL1_AMU_SUPPORT_FEAT_AMUv1p1,
};

enum id_aa64pfr0_el1_csv2_support : uint8_t {
    ID_AA64PFR0_EL1_CSV2_SUPPORT_NONE,
    // FEAT_CSV2 is implemented, but FEAT_CSV2_2 and FEAT_CSV2_3 are not
    // implemented.
    // ID_AA64PFR1_EL1.CSV2_frac determines whether either or both of
    // FEAT_CSV2_1p1 or FEAT_CSV2_1p2 are implemented.
    ID_AA64PFR0_EL1_CSV2_SUPPORT_FEAT_CSV2,
    // FEAT_CSV2_2 is implemented, but FEAT_CSV2_3 is not implemented.
    ID_AA64PFR0_EL1_CSV2_SUPPORT_FEAT_CSV2_2,
    // FEAT_CSV2_3 is implemented.
    ID_AA64PFR0_EL1_CSV2_SUPPORT_FEAT_CSV2_3,
};

enum id_aa64pfr0_el1_flags : uint64_t {
    // EL0 can be executed in either AArch64 or AArch32 state.
    __ID_AA64PFR0_EL1_EL0_AA32 = 0b1111ull << 0,
    // EL1 can be executed in either AArch64 or AArch32 state.
    __ID_AA64PFR0_EL1_EL1_AA32 = 0b1111ull << 4,
    // EL2 can be executed in either AArch64 or AArch32 state.
    __ID_AA64PFR0_EL1_EL2_AA32 = 0b1111ull << 8,
    // EL3 can be executed in either AArch64 or AArch32 state.
    __ID_AA64PFR0_EL1_EL3_AA32 = 0b1111ull << 12,
    __ID_AA64PFR0_EL1_FP = 0b1111ull << 16,
    __ID_AA64PFR0_EL1_ADV_SIMD = 0b1111ull << 20,
    __ID_AA64PFR0_EL1_GIC = 0b1111ull << 24,
    __ID_AA64PFR0_EL1_RAS = 0b1111ull << 28,
    // SVE architectural state and programmers' model are implemented.
    __ID_AA64PFR0_EL1_SVE = 0b1111ull << 32,
    // Secure EL2 is implemented.
    __ID_AA64PFR0_EL1_SEL2 = 0b1111ull << 36,
    __ID_AA64PFR0_EL1_MPAM = 0b1111ull << 40,
    __ID_AA64PFR0_EL1_AMU = 0b1111ull << 44,
    // AArch64 provides the PSTATE.DIT mechanism to guarantee constant execution
    // time of certain instructions.
    __ID_AA64PFR0_EL1_DIT = 0b1111ull << 48,
    // RMEv1 is implemented.
    __ID_AA64PFR0_EL1_RME = 0b1111ull << 52,
    __ID_AA64PFR0_EL1_CSV2 = 0b1111ull << 56,
    // Data loaded under speculation with a permission or domain fault cannot be
    // used to form an address, generate condition codes, or generate SVE
    // predicate values to be used by other instructions in the speculative
    // sequence. The execution timing of any other instructions in the
    // speculative sequence is not a function of the data loaded under
    // speculation.
    __ID_AA64PFR0_EL1_CSV3 = 0b1111ull << 60
};

enum id_aa64pfr1_el1_shifts : uint8_t {
    ID_AA64PFR1_EL1_SSBS_SUPPORT_SHIFT = 4,
    ID_AA64PFR1_EL1_MTE_SUPPORT_SHIFT = 8,
    ID_AA64PFR1_EL1_SME_SUPPORT_SHIFT = 20,
    ID_AA64PFR1_EL1_CSV2_FRAC_SHIFT = 28,
};

enum id_aa64pfr1_el1_ssbs_support : uint8_t {
    ID_AA64PFR1_EL1_SSBS_SUPPORT_NONE,
    // AArch64 provides the PSTATE.SSBS mechanism to mark regions that are
    // Speculative Store Bypass Safe.
    ID_AA64PFR1_EL1_SSBS_SUPPORT_FEAT_SSBS,
    // As 0b0001, and adds the MSR and MRS instructions to directly read and
    // write the PSTATE.SSBS field.
    ID_AA64PFR1_EL1_SSBS_SUPPORT_FEAT_SSBS2,
};

// MTE = Memory Tagging Extension
enum id_aa64pfr1_el1_mte_support : uint8_t {
    ID_AA64PFR1_EL1_MTE_SUPPORT_NONE,
    // Instruction-only Memory Tagging Extension is implemented.
    ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE,
    // Full Memory Tagging Extension is implemented.
    // Support for Asynchronous Faulting is defined by ID_AA64PFR1_EL1.MTE_frac.
    ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE2,
    // Memory Tagging Extension is implemented with support for asymmetric Tag
    // Check Fault handling.
    // Support for Asynchronous Faulting and asymmetric Tag Check Fault handling
    // is defined by ID_AA64PFR1_EL1.MTE_frac.
    ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE3,
};

// SME = Scalable Matrix Extension
enum id_aa64pfr1_el1_sme_support : uint8_t {
    ID_AA64PFR1_EL1_SME_SUPPORT_NONE,
    // SME architectural state and programmers' model are implemented.
    ID_AA64PFR1_EL1_SME_SUPPORT_FEAT_SME,
    // As 0b0001, plus the SME2 ZT0 register.
    ID_AA64PFR1_EL1_SME_SUPPORT_FEAT_SME2,
};

enum id_aa64pfr1_el1_csv2_frac : uint8_t {
    ID_AA64PFR1_EL1_CSV2_FRAC_NONE,
    // FEAT_CSV2_1p1 is implemented, but FEAT_CSV2_1p2 is not implemented.
    ID_AA64PFR1_EL1_CSV2_FRAC_FEAT_CSV2_1P1,
    // FEAT_CSV2_1p2 is implemented.
    ID_AA64PFR1_EL1_CSV2_FRAC_FEAT_CSV2_1P2,
};

enum id_aa64pfr1_el1_flags : uint64_t {
    // The Branch Target Identification mechanism is implemented.
    __ID_AA64PFR1_EL1_BT = 0b1111ull << 0,
    __ID_AA64PFR1_EL1_SSBS = 0b1111ull << 4,
    __ID_AA64PFR1_EL1_MTE = 0b1111ull << 8,
    // If ID_AA64PFR0_EL1.RAS == 0b0001, as 0b0000 and adds support for:
    //   Additional ERXMISC<m>_EL1 System registers.
    //   Additional System registers ERXPFGCDN_EL1, ERXPFGCTL_EL1, and
    //     ERXPFGF_EL1, and the SCR_EL3.FIEN and HCR_EL2.FIEN trap controls, to
    //     support the optional RAS Common Fault Injection Model Extension.
    // Error records accessed through System registers conform to RAS System
    // Architecture v1.1, which includes simplifications to ERR<n>STATUS, and
    // support for the optional RAS Timestamp and RAS Common Fault Injection
    // Model Extensions.
    __ID_AA64PFR1_EL1_RAS_FRAC = 0b1111ull << 12,
    // The minor version number of the MPAM extension is 1.
    __ID_AA64PFR1_EL1_MPAM_FRAC = 0b1111ull << 16,
    __ID_AA64PFR1_EL1_SME = 0b1111ull << 20,
    // Trapping of RNDR and RNDRRS to EL3 is supported.
    // SCR_EL3.TRNDR is present.
    __ID_AA64PFR1_EL1_RNDR_TRAP = 0b1111ull << 24,
    __ID_AA64PFR1_EL1_CSV2_FRAC = 0b1111ull << 28,
    // SCTLR_ELx.{SPINTMASK, NMI} and PSTATE.ALLINT with its associated
    // instructions are supported.
    __ID_AA64PFR1_EL1_NMI = 0b1111ull << 32,
    // Asynchronous Faulting is not supported.
    // If ID_AA64PFR1_EL1.MTE >= 0b0011, asymmetric Tag Check Fault handling is
    // not supported.
    __ID_AA64PFR1_EL1_MTE_FRAC = 0b1111ull << 36,
    // Guarded Control Stack is implemented.
    __ID_AA64PFR1_EL1_GCS = 0b1111ull << 40,
    // The RCW and RCWS instructions, their associated registers and traps are
    // supported.
    // If EL2 is implemented, the AssuredOnly check, TopLevel check, and their
    // associated controls are also implemented.
    // If EL2 and FEAT_GCS are implemented, VTCR_EL2.GCSH is implemented.
    __ID_AA64PFR1_EL1_THE = 0b1111ull << 44,
    // As 0b0000, and the following additional modes are supported:
    // Canonical Tag checking, identified as FEAT_MTE_CANONICAL_TAGS.
    // Memory tagging with Address tagging disabled, identified as
    // FEAT_MTE_NO_ADDRESS_TAGS.
    __ID_AA64PFR1_EL1_MTEX = 0b1111ull << 48,
    // FEAT_DoubleFault2 is implemented. As ID_AA64PFR0_EL1.RAS == 0b0010, and
    // also includes support for routing error exceptions:
    //   Traps for masked error exceptions, HCRX_EL2.TMEA and SCR_EL3.TMEA.
    //   Additional controls for masking SError interrupts, SCTLR2_EL1.NMEA, and
    //     SCTLR2_EL2.NMEA.
    //   Additional controls for taking external aborts to the SError interrupt
    //   vector, SCTLR2_EL1.EASE and SCTLR2_EL2.EASE.
    __ID_AA64PFR1_EL1_DF2 = 0b1111ull << 52,
    // FEAT_PFAR is implemented. Includes support for the PFAR_ELx and, if EL3
    // is implemented, MFAR_EL3 registers.
    __ID_AA64PFR1_EL1_PFAR = 0b1111ull << 56,
};

enum id_aar64_pfr2_el1_flags : uint16_t {
    // FEAT_MTE_PERM is supported.
    __ID_AA64PFR2_EL1_MTEPERM = 0b1111ull << 0,
    // FEAT_MTE_STORE_ONLY is supported.
    __ID_AA64PFR2_EL1_MTESTOREONLY = 0b1111ull << 4,
    // On a synchronous exception due to a Tag Check Fault, FAR_ELx[63:60] is
    // not UNKNOWN.
    // This feature is identified as FEAT_MTE_TAGGED_FAR.
    __ID_AA64PFR2_EL1_MTEFAR = 0b1111ull << 8,
};

enum id_aa64mmfr0_shifts : uint8_t {
    ID_AA64MMFR0_PA_RANGE_SHIFT = 0,
    ID_AA64MMFR0_TGRAN16_SUPPORT_SHIFT = 20,
    ID_AA64MMFR0_TGRAN4_SUPPORT_SHIFT = 28,
    ID_AA64MMFR0_TGRAN16_STAGE_2_SUPPORT_SHIFT = 32,
    ID_AA64MMFR0_TGRAN64_STAGE_2_SUPPORT_SHIFT = 36,
    ID_AA64MMFR0_TGRAN4_STAGE_2_SUPPORT_SHIFT = 40,
    ID_AA64MMFR0_FGT_SUPPORT_SHIFT = 56,
    ID_AA64MMFR0_ECV_SUPPORT_SHIFT = 60,
};

enum id_aa64mmfr0_pa_range : uint8_t {
    ID_AA64MMFR0_PA_RANGE_32B_4GIB,
    ID_AA64MMFR0_PA_RANGE_36B_64GIB,
    ID_AA64MMFR0_PA_RANGE_40B_1TIB,
    ID_AA64MMFR0_PA_RANGE_42B_4TIB,
    ID_AA64MMFR0_PA_RANGE_44B_16TIB,
    ID_AA64MMFR0_PA_RANGE_48B_256TIB,
    // When FEAT_LPA is implemented or FEAT_LPA2 is implemented
    ID_AA64MMFR0_PA_RANGE_52B_4PIB,
    // When FEAT_D128 is implemented
    ID_AA64MMFR0_PA_RANGE_56B_64PIB,
};

enum id_aa64mmfr0_tgran16_support : uint8_t {
    ID_AA64MMFR0_TGRAN16_SUPPORT_NONE,
    ID_AA64MMFR0_TGRAN16_SUPPORTED,
    // When FEAT_LPA2 is implemented
    ID_AA64MMFR0_TGRAN16_SUPPORTED_52BIT_ADDRESS
};

enum id_aa64mmfr0_tgran4_support : uint8_t {
    ID_AA64MMFR0_TGRAN4_SUPPORTED,
    // When FEAT_LPA2 is implemented
    ID_AA64MMFR0_TGRAN4_SUPPORTED_52BIT_ADDRESS,
    ID_AA64MMFR0_TGRAN4_SUPPORT_NONE = 0b1111,
};

enum id_aa64mmfr0_tgran16_stage_2_support : uint8_t {
    // Support for 16KB granule at stage 2 is identified in the
    // ID_AA64MMFR0_EL1.TGran16 field.
    ID_AA64MMFR0_TGRAN16_STAGE_2_SUPPORTED_WITH_REG,
    // 16KB granule not supported at stage 2.
    ID_AA64MMFR0_TGRAN16_STAGE_2_SUPPORT_NONE,
    // 16KB granule supported at stage 2.
    ID_AA64MMFR0_TGRAN16_STAGE_2_SUPPORTED,
    // When FEAT_LPA2 is implemented
    ID_AA64MMFR0_TGRAN16_STAGE_2_SUPPORTED_52BIT_ADDRESS
};

enum id_aa64mmfr0_tgran64_stage_2_support : uint8_t {
    // Support for 64KB granule at stage 2 is identified in the
    // ID_AA64MMFR0_EL1.TGran64 field.
    ID_AA64MMFR0_TGRAN64_STAGE_2_SUPPORTED_WITH_REG,
    // 64KB granule not supported at stage 2.
    ID_AA64MMFR0_TGRAN64_STAGE_2_SUPPORTED,
    // 64KB granule supported at stage 2.
    ID_AA64MMFR0_TGRAN64_STAGE_2_SUPPORT_NONE,
};

enum id_aa64mmfr0_tgran4_stage_2_support : uint8_t {
    // Support for 4KB granule at stage 2 is identified in the
    // ID_AA64MMFR0_EL1.TGran4 field.
    ID_AA64MMFR0_TGRAN4_STAGE_2_SUPPORTED_WITH_REG,
    // 4KB granule not supported at stage 2.
    ID_AA64MMFR0_TGRAN4_STAGE_2_SUPPORT_NONE,
    // 4KB granule supported at stage 2.
    ID_AA64MMFR0_TGRAN4_STAGE_2_SUPPORTED,
    // 4KB granule at stage 2 supports 52-bit input addresses and can describe
    // 52-bit output addresses.
    // When FEAT_LPA2 is implemented
    // When FEAT_LPA2 is implemented
    ID_AA64MMFR0_TGRAN4_STAGE_2_SUPPORTED_52BIT_ADDRESS
};

// FGT = Fine-Grained Trap
enum id_aa64mmfr0_fgt_support : uint8_t {
    // FEAT_MTE_PERM is supported.
    ID_AA64MMFR0_FGT_SUPPORT_NONE,
    // Fine-grained trap controls are implemented. Supports:
    //  If EL2 is implemented, the HAFGRTR_EL2, HDFGRTR_EL2, HDFGWTR_EL2,
    //      HFGRTR_EL2, HFGITR_EL2 and HFGWTR_EL2 registers, and their
    //      associated traps.
    //  If EL2 is implemented, MDCR_EL2.TDCC.
    //  If EL3 is implemented, MDCR_EL3.TDCC.
    //  If both EL2 and EL3 are implemented, SCR_EL3.FGTEn.
    ID_AA64MMFR0_FGT_SUPPORT_FEAT_FGT,
    // As 0b0001, and also includes support for:
    //  If EL2 is implemented, the HDFGRTR2_EL2, HDFGWTR2_EL2, HFGITR2_EL2,
    //      HFGRTR2_EL2, and HFGWTR2_EL2 registers, and their associated traps.
    //  If both EL2 and EL3 are implemented, SCR_EL3.FGTEn2.
    ID_AA64MMFR0_FGT_SUPPORT_FEAT_FGT2,
};

// ECV = Enhanced Counter Virtualization
enum id_aa64mmfr0_ecv_support : uint8_t {
    // FEAT_MTE_PERM is supported.
    ID_AA64MMFR0_ECV_SUPPORT_NONE,
    // Enhanced Counter Virtualization is implemented. Supports
    // CNTHCTL_EL2.{EL1TVT, EL1TVCT, EL1NVPCT, EL1NVVCT, EVNTIS},
    // CNTKCTL_EL1.EVNTIS, CNTPCTSS_EL0 counter views, and CNTVCTSS_EL0 counter
    // views. Extends the PMSCR_EL1.PCT, PMSCR_EL2.PCT, TRFCR_EL1.TS, and
    // TRFCR_EL2.TS fields.
    ID_AA64MMFR0_ECV_SUPPORT_FEAT_ECV_PARTIAL,
    // As 0b0001, and also includes support for CNTHCTL_EL2.ECV and CNTPOFF_EL2.
    ID_AA64MMFR0_ECV_SUPPORT_FEAT_ECV_FULL,
};

enum id_aa64mmfr0_flags : uint64_t {
    __ID_AA64MMFR0_PARANGE = 0b1111ull << 0,
    // 16 bits if set, otherwise 8
    __ID_AA64MMFR0_ASID_BITS = 0b1111ull << 4,
    // Mixed-endian support. The SCTLR_ELx.EE and SCTLR_EL1.E0E bits can be
    // configured.
    __ID_AA64MMFR0_BIGEND = 0b1111ull << 8,
    // Does support a distinction between Secure and Non-secure Memory.
    __ID_AA64MMFR0_SNSMEM = 0b1111ull << 12,
    // Mixed-endian support at EL0. The SCTLR_EL1.E0E bit can be configured.
    __ID_AA64MMFR0_BIGENDEL0 = 0b1111ull << 16,
    __ID_AA64MMFR0_TGRAN16 = 0b1111ull << 20,
    // 64KB granule not supported if set, otherwise supported.
    __ID_AA64MMFR0_TGRAN64 = 0b1111ull << 24,
    __ID_AA64MMFR0_TGRAN4 = 0b1111ull << 28,
    __ID_AA64MMFR0_TGRAN16_STAGE_2 = 0b1111ull << 32,
    __ID_AA64MMFR0_TGRAN64_STAGE_2 = 0b1111ull << 36,
    __ID_AA64MMFR0_TGRAN4_STAGE_2 = 0b1111ull << 40,
    // Non-context synchronizing exception entry and exit are supported.
    __ID_AA64MMFR0_EXS = 0b1111ull << 44,
    // 8 Bits unused
    __ID_AA64MMFR0_FGT = 0b1111ull << 56,
    __ID_AA64MMFR0_ECV = 0b1111ull << 60,
};

enum id_aa64mmfr1_shifts : uint8_t {
    ID_AA64MMFR1_HAFDBS_SUPPORT_SHIFT = 0,
    ID_AA64MMFR1_HPDS_SUPPORT_SHIFT = 12,
    ID_AA64MMFR1_PAN_SUPPORT_SHIFT = 20,
};

// HAFDBS = Hardware updates to Access flag and Dirty state in translation
// tables
enum id_aa64mmfr1_hafdbs_support : uint8_t {
    ID_AA64MMFR1_HAFDBS_SUPPORT_NONE,
    // Support for hardware update of the Access flag for Block and Page
    // descriptors.
    ID_AA64MMFR1_HAFDBS_SUPPORT_FEAT_HAFDBS_ACCESS_BIT,
    // As 0b0001, and adds support for hardware update of the Access flag for
    // Block and Page descriptors. Hardware update of dirty state is supported.
    ID_AA64MMFR1_HAFDBS_SUPPORT_FEAT_HAFDBS_ACCESS_AND_DIRTY,
    // As 0b0010, and adds support for hardware update of the Access flag for
    // Table descriptors.
    ID_AA64MMFR1_HAFDBS_SUPPORT_FEAT_HAFT,
};

// HPDS = Hierarchical Permission Disables
enum id_aa64mmfr1_hpds_support : uint8_t {
    ID_AA64MMFR1_HPDS_SUPPORT_NONE,
    // Disabling of hierarchical controls supported with the TCR_EL1.{HPD1,
    // HPD0}, TCR_EL2.HPD or TCR_EL2.{HPD1, HPD0}, and TCR_EL3.HPD bits.
    ID_AA64MMFR1_HPDS_SUPPORT_FEAT_HPDS,
    // As for value 0b0001, and adds possible hardware allocation of bits[62:59]
    // of the Translation table descriptors from the final lookup level for
    // IMPLEMENTATION DEFINED use.
    ID_AA64MMFR1_HPDS_SUPPORT_FEAT_HPDS2,
};

// PAN = Privileged Access Never.
enum id_aa64mmfr1_pan_support : uint8_t {
    ID_AA64MMFR1_PAN_SUPPORT_NONE,
    // PAN supported.
    ID_AA64MMFR1_PAN_SUPPORT_FEAT_PAN,
    // PAN supported and AT S1E1RP and AT S1E1WP instructions supported.
    ID_AA64MMFR1_PAN_SUPPORT_FEAT_PAN2,
    // PAN supported, AT S1E1RP and AT S1E1WP instructions supported, and
    // SCTLR_EL1.EPAN and SCTLR_EL2.EPAN bits supported.
    ID_AA64MMFR1_PAN_SUPPORT_FEAT_PAN3,
};

enum id_aa64mmfr1_flags : uint64_t {
    __ID_AA64MMFR1_HAFDBS = 0b1111ull << 0,
    // 16 if set, otherwise 8
    __ID_AA64MMFR1_VMIDBITS = 0b1111ull << 4,
    // Virtualization Host Extensions supported.
    __ID_AA64MMFR1_VH = 0b1111ull << 8,
    __ID_AA64MMFR1_HPDS = 0b1111ull << 12,
    // LORegions supported.
    __ID_AA64MMFR1_LO = 0b1111ull << 16,
    __ID_AA64MMFR1_PAN = 0b1111ull << 20,
    // The PE might generate an SError interrupt due to an External abort on a
    // speculative read.
    __ID_AA64MMFR1_SPECSEI = 0b1111ull << 24,
    // Distinction between EL0 and EL1 execute-never control at stage 2
    // supported.
    __ID_AA64MMFR1_XNX = 0b1111ull << 28,
    // Configurable delayed trapping of WFE is supported.
    __ID_AA64MMFR1_TWED = 0b1111ull << 32,
    // Enhanced Translation Synchronization is supported.
    __ID_AA64MMFR1_ETS = 0b1111ull << 36,
    // HCRX_EL2 and its associated EL3 trap are supported.
    __ID_AA64MMFR1_HCX = 0b1111ull << 40,
    // The FPCR.{AH, FIZ, NEP} fields are supported.
    __ID_AA64MMFR1_AFP = 0b1111ull << 44,
    // The intermediate caching of translation table walks does not include
    // non-coherent physical translation caches.
    __ID_AA64MMFR1_NTLBPA = 0b1111ull << 48,
    // SCTLR_EL1.TIDCP bit is implemented. If EL2 is implemented,
    // SCTLR_EL2.TIDCP bit is implemented.
    __ID_AA64MMFR1_TIDCP1 = 0b1111ull << 52,
    // SCTLR_EL1.CMOW is implemented. If EL2 is implemented, SCTLR_EL2.CMOW and
    // HCRX_EL2.CMOW bits are implemented.
    __ID_AA64MMFR1_CMOW = 0b1111ull << 56,
    // The branch history information created in a context before an exception
    // to a higher Exception level using AArch64 cannot be used by code before
    // that exception to exploit control of the execution of any indirect
    // branches in code in a different context after the exception.
    __ID_AA64MMFR1_ECBHB = 0b1111ull << 60,
};

enum id_aa64mmfr2_shifts : uint8_t {
    ID_AA64MMFR2_VA_RANGE_SHIFT = 16,
    ID_AA64MMFR2_NV_SUPPORT_SHIFT = 24,
    ID_AA64MMFR2_BBM_SUPPORT_SHIFT = 52,
    ID_AA64MMFR2_EVT_SUPPORT_SHIFT = 56,
};

enum id_aa64mmfr2_va_range : uint8_t {
    ID_AA64MMFR2_VA_RANGE_48BITS,
    // VMSAv8-64 supports 52-bit VAs when using the 64KB translation granule.
    // The size for other translation granules is not defined by this field.
    ID_AA64MMFR2_VA_RANGE_52BITS,
    // VMSAv9-128 supports 56-bit VAs.
    ID_AA64MMFR2_VA_RANGE_56BITS,
};

// NV = Nested Virtualization
enum id_aa64mmfr2_nv_support : uint8_t {
    ID_AA64MMFR2_NV_SUPPORT_NONE,
    // The HCR_EL2.{AT, NV1, NV} bits are implemented.
    ID_AA64MMFR2_NV_SUPPORT_FEAT_NV,
    // The VNCR_EL2 register and the HCR_EL2.{NV2, AT, NV1, NV} bits are
    // implemented.
    ID_AA64MMFR2_NV_SUPPORT_FEAT_NV2,
};

// BBM = break-before-make sequences when changing block size for a translation
enum id_aa64mmfr2_bbm_support : uint8_t {
    // Level 0 support for changing block size is supported.
    ID_AA64MMFR2_BBM_SUPPORT_LEVEL0,
    // Level 1 support for changing block size is supported.
    ID_AA64MMFR2_BBM_SUPPORT_LEVEL1,
    // Level 2 support for changing block size is supported.
    ID_AA64MMFR2_BBM_SUPPORT_LEVEL2,
};

enum id_aa64mmfr2_evt_support : uint8_t {
    ID_AA64MMFR2_EVT_SUPPORT_NONE,
    // HCR_EL2.{TOCU, TICAB, TID4} traps are supported. HCR_EL2.{TTLBOS, TTLBIS}
    // traps are not supported.
    ID_AA64MMFR2_EVT_SUPPORT_FEAT_EVT_1,
    // HCR_EL2.{TTLBOS, TTLBIS, TOCU, TICAB, TID4} traps are supported.
    ID_AA64MMFR2_EVT_SUPPORT_FEAT_EVT_2,
};

enum id_aa64mmfr2_flags : uint64_t {
    // Common not Private translations supported.
    __ID_AA64MMFR2_CNP = 0b1111ull << 0,
    // User Access Override supported
    __ID_AA64MMFR2_UAO = 0b1111ull << 4,
    // LSMAOE and nTLSMD bits supported.
    __ID_AA64MMFR2_LSM = 0b1111ull << 8,
    // IESB bit in the SCTLR_ELx registers is supported.
    __ID_AA64MMFR2_IESB = 0b1111ull << 12,
    __ID_AA64MMFR2_VARANGE = 0b1111ull << 16,
    // 64-bit format implemented for all levels of the CCSIDR_EL1, otherwise
    // 32-bit
    __ID_AA64MMFR2_CCIDX = 0b1111ull << 20,
    __ID_AA64MMFR2_NV = 0b1111ull << 24,
    // The maximum value of the TCR_ELx.{T0SZ,T1SZ} and VTCR_EL2.T0SZ fields is
    // 48 for 4KB and 16KB granules, and 47 for 64KB granules, otherwise 39
    __ID_AA64MMFR2_ST = 0b1111ull << 28,
    // Unaligned single-copy atomicity and atomic functions with a 16-byte
    // address range aligned to 16-bytes are supported.
    __ID_AA64MMFR2_AT = 0b1111ull << 32,
    // All exceptions generated by an AArch64 read access to the feature ID
    // space are reported by ESR_ELx.EC == 0x18.
    __ID_AA64MMFR2_IDS = 0b1111ull << 36,
    // HCR_EL2.FWB bit is supported.
    __ID_AA64MMFR2_FWB = 0b1111ull << 40,
    // 4 bits unused
    // TLB maintenance instructions by address have bits[47:44] holding the TTL
    // field.
    __ID_AA64MMFR2_TTL = 0b1111ull << 48,
    __ID_AA64MMFR2_BBM = 0b1111ull << 52,
    __ID_AA64MMFR2_EVT = 0b1111ull << 56,
    // E0PDx mechanism is implemented.
    __ID_AA64MMFR2_E0PD = 0b1111ull << 60,
};

enum id_aa64mmfr3_shifts : uint8_t {
    ID_AA64MMFR3_SNERR_SUPPORT_SHIFT = 40,
    ID_AA64MMFR3_ANERR_SUPPORT_SHIFT = 44,
    ID_AA64MMFR3_SDERR_SUPPORT_SHIFT = 52,
    ID_AA64MMFR3_ADERR_SUPPORT_SHIFT = 56,
};

// SNERR = Synchronous Normal error exceptions
enum id_aa64mmfr3_snerr_support : uint8_t {
    // If FEAT_RASv2 is not implemented and ID_AA64MMFR3_EL1.ANERR is 0b0000,
    // then the behavior is not described. Otherwise, the behavior is described
    // by ID_AA64MMFR3_EL1.ANERR.
    ID_AA64MMFR3_SNERR_SUPPORT_NONE,
    // All error exceptions for Normal memory loads are taken synchronously.
    ID_AA64MMFR3_SNERR_SUPPORT_SYNC,
    // FEAT_ANERR is implemented. SCTLR2_ELx.EnANERR and HCRX_EL2.EnSNERR are
    // implemented.
    ID_AA64MMFR3_SNERR_SUPPORT_FEAT_ANERR,
};

enum id_aa64mmfr3_anerr_support : uint8_t {
    // If FEAT_RASv2 is not implemented and ID_AA64MMFR3_EL1.SNERR is 0b0000,
    // then the behavior is not described. Otherwise, the behavior is describe
    // by ID_AA64MMFR3_EL1.SNERR.
    ID_AA64MMFR3_ANERR_SUPPORT_NONE,
    // Some error exceptions for Normal memory loads are taken asynchronously.
    ID_AA64MMFR3_ANERR_SUPPORT_ASYNC,
    // FEAT_ANERR is implemented. SCTLR2_ELx.EnANERR and HCRX_EL2.EnSNERR are
    // implemented.
    ID_AA64MMFR3_ANERR_SUPPORT_FEAT_ANERR,
};

enum id_aa64mmfr3_sderr_support : uint8_t {
    // If FEAT_RASv2 is not implemented and ID_AA64MMFR3_EL1.ADERR is 0b0000,
    // then the behavior is not described. Otherwise, the behavior is described
    // by ID_AA64MMFR3_EL1.ADERR.
    ID_AA64MMFR3_SDERR_SUPPORT_NONE,
    // All error exceptions for Device memory loads are taken synchronously.
    ID_AA64MMFR3_SDERR_SUPPORT_SYNC,
    // FEAT_ADERR is implemented. SCTLR2_ELx.EnADERR and HCRX_EL2.EnSDERR are
    // implemented.
    ID_AA64MMFR3_SDERR_SUPPORT_FEAT_ADERR,
};

enum id_aa64mmfr3_aderr_support : uint8_t {
    // If FEAT_RASv2 is not implemented and ID_AA64MMFR3_EL1.SDERR is 0b0000,
    // then the behavior is not described. Otherwise, the behavior is described
    // by ID_AA64MMFR3_EL1.SDERR.
    ID_AA64MMFR3_ADERR_SUPPORT_NONE,
    // Some error exceptions for Device memory loads are taken asynchronously.
    ID_AA64MMFR3_ADERR_SUPPORT_ASYNC,
    // FEAT_ADERR is implemented. SCTLR2_ELx.EnADERR and HCRX_EL2.EnSDERR are
    // implemented.
    ID_AA64MMFR3_ADERR_SUPPORT_FEAT_ADERR,
};

enum id_aa64mmfr3_flags : uint64_t {
    // TCR2_EL1, TCR2_EL2 and their associated trap controls are implemented.
    __ID_AA64MMFR3_TCRX = 0b1111ull << 0,
    // SCTLR2_EL1, SCTLR2_EL2 and their associated trap controls are
    // implemented.
    __ID_AA64MMFR3_SCTLRX = 0b1111ull << 4,
    // Permission Indirection at Stage 1 is supported.
    __ID_AA64MMFR3_S1PIE = 0b1111ull << 8,
    // Permission Indirection at Stage 2 is supported.
    __ID_AA64MMFR3_S2PIE = 0b1111ull << 12,
    // Permission Overlay at Stage 1 is supported.
    __ID_AA64MMFR3_S1POE = 0b1111ull << 16,
    // Permission Overlay at Stage 2 is supported.
    __ID_AA64MMFR3_S2POE = 0b1111ull << 20,
    // The Attribute Index Enhancement at Stage 1 is supported.
    __ID_AA64MMFR3_AIE = 0b1111ull << 24,
    // Memory Encryption Contexts is supported for Realm physical address space.
    __ID_AA64MMFR3_MEC = 0b1111ull << 28,
    // 128-bit Page Table Descriptor Extension is supported.
    __ID_AA64MMFR3_D128 = 0b1111ull << 32,
    // 128-bit Page Table Descriptor Extension at stage 2 is supported.
    __ID_AA64MMFR3_D128_STAGE_2 = 0b1111ull << 36,
    __ID_AA64MMFR3_SNERR = 0b1111ull << 40,
    __ID_AA64MMFR3_ANERR = 0b1111ull << 44,
    __ID_AA64MMFR3_SDERR = 0b1111ull << 52,
    __ID_AA64MMFR3_ADERR = 0b1111ull << 56,
    // The speculative use of pointers processed by a PAC Authentication is not
    // materially different in terms of the impact on cached micro-architectural
    // state between passing and failing of the PAC Authentication.
    __ID_AA64MMFR3_SPEC_FPACC = 0b1111ull << 60,
};

enum id_aa64mmfr4_shifts : uint8_t {
    ID_AA64MMFR4_EIESB_SHIFT = 0
};

// EIESB = Early Implicit Error Synchronization event
enum id_aa64mmfr4_eiesb : uint8_t {
    // Behavior is not described.
    ID_AA64MMFR4_EIESB_NO_DESC,
    // When SError exceptions are routed to EL3, and either FEAT_DoubleFault is
    // not implemented or the Effective value of SCR_EL3.NMEA is 1, an implicit
    // Error synchronization event is inserted before an exception taken to EL3.
    ID_AA64MMFR4_EIESB_2 = 1 << 0,
    // When SError exceptions are routed to ELx, and either FEAT_DoubleFault2 is
    // not implemented or the Effective value of the applicable one of
    // SCR_EL3.NMEA or SCTLR2_ELx.NMEA is 1, an implicit Error synchronization
    // event is inserted before an exception taken to ELx.
    ID_AA64MMFR4_EIESB_3 = 1 << 1,
    // Implicit Error synchronization event is always inserted after an
    // exception is taken.
    ID_AA64MMFR4_EIESB_TAKEN = 0b1111,
};

enum id_aa64mmfr4_flags : uint8_t {
    __ID_AA64MMFR4_EIESB = 0b1111 << 0,
};

#define ID_AA64MMFR0_SME_VERSION_SHIFT 56

enum id_aa64mmfr0_sme_version : uint8_t {
    ID_AA64MMFR0_SME_VERSION_FEAT_SME,
    // As 0b0000, and adds the mandatory SME2 instructions.
    ID_AA64MMFR0_SME_VERSION_FEAT_SME2,
    // As 0b0001, and adds the mandatory SME2.1 instructions.
    ID_AA64MMFR0_SME_VERSION_FEAT_SME2p1,
};

enum id_aa64smfr0_flags : uint64_t {
    // The FMOPA and FMOPS instructions that accumulate single-precision outer
    // products into single-precision tiles are implemented.
    __ID_AA64SMFR0_F32F32 = 1ull << 32,
    // The BMOPA and BMOPS instructions that accumulate 1-bit binary outer
    // products into 32-bit integer tiles are implemented.
    __ID_AA64SMFR0_BI32I32 = 1ull << 33,
    // The BFMOPA and BFMOPS instructions that accumulate BFloat16 outer
    // products into single-precision tiles are implemented.
    __ID_AA64SMFR0_B16F32 = 1ull << 34,
    // The FMOPA and FMOPS instructions that accumulate half-precision outer
    // products into single-precision tiles are implemented.
    __ID_AA64SMFR0_F16F32 = 1ull << 35,
    // The SMOPA, SMOPS, SUMOPA, SUMOPS, UMOPA, UMOPS, USMOPA, and USMOPS
    // instructions that accumulate 8-bit outer products into 32-bit tiles are
    // implemented.
    __ID_AA64SMFR0_I8I32 = 1ull << 36,
    // SME2.1 non-widening half-precision floating-point instructions are
    // implemented.
    __ID_AA64SMFR0_F16F16 = 1ull << 42,
    // SME2.1 non-widening BFloat16 instructions are implemented.
    __ID_AA64SMFR0_B16B16 = 1ull << 43,
    // The SMOPA (2-way), SMOPS (2-way), UMOPA (2-way), and UMOPS (2-way)
    // instructions that accumulate 16-bit outer products into 32-bit integer
    // tiles are implemented.
    __ID_AA64SMFR0_I16I32 = 0b1111ull << 44,
    // The variants of the FMOPA and FMOPS instructions that accumulate into
    // double-precision tiles are implemented.
    // When FEAT_SME2 is implemented, the variants of the FADD, FMLA, FMLS, and
    // FSUB instructions that accumulate into double-precision elements in ZA
    // array vectors are implemented.
    __ID_AA64SMFR0_F64F64 = 1ull << 48,
    // 3 bits unused
    // The variants of the ADDHA, ADDVA, SMOPA, SMOPS, SUMOPA, SUMOPS, UMOPA,
    // UMOPS, USMOPA, and USMOPS instructions that accumulate into 64-bit
    // integer tiles are implemented.
    // When FEAT_SME2 is implemented, the variants of the ADD, ADDA, SDOT,
    // SMLALL, SMLSLL, SUB, SUBA, SVDOT, UDOT, UMLALL, UMLSLL, and UVDOT
    // instructions that accumulate into 64-bit integer elements in ZA array
    // vectors are implemented.
    __ID_AA64SMFR0_I16I64 = 0b1111ull << 52,
    __ID_AA64SMFR0_SMEVER = 0b1111ull << 56,
    // 3 bits unused
    // All implemented A64 instructions are legal for execution in Streaming
    // SVE mode, when enabled by SMCR_EL1.FA64, SMCR_EL2.FA64, and
    // SMCR_EL3.FA64.
    __ID_AA64SMFR0_FA64 = 1ull << 57,
};

enum id_aa64zfr0_el1_shifts : uint8_t {
    ID_AA64ZFR0_EL1_SVE_SUPPORT_SHIFT = 0,
    ID_AA64ZFR0_EL1_SVE_AES_SUPPORT_SHIFT = 4,
    ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_SHIFT = 20,
};

enum id_aa64zfr0_el1_sve_support : uint8_t {
    // The SVE instructions are implemented.
    ID_AA64ZFR0_EL1_SVE_SUPPORT_SVE,
    // As 0b0000, and adds the mandatory SVE2 instructions.
    ID_AA64ZFR0_EL1_SVE_SUPPORT_SVE2,
    // As 0b0001, and adds the mandatory SVE2.1 instructions.
    ID_AA64ZFR0_EL1_SVE_SUPPORT_SVE2p1,
};

enum id_aa64zfr0_el1_sve_aes_support : uint8_t {
    // SVE AES* instructions are not implemented.
    ID_AA64ZFR0_EL1_SVE_AVS_SUPPORT_NONE,
    // SVE AESE, AESD, AESMC, and AESIMC instructions are implemented.
    ID_AA64ZFR0_EL1_SVE_AVS_SUPPORT_FEAT_AES,
    // As 0b0001, plus 64-bit source element variants of SVE PMULLB and PMULLT
    // instructions are implemented.
    ID_AA64ZFR0_EL1_SVE_AES_SUPPORT_FEAT_SVE_PMULL128,
};

enum id_aa64zfr0_el1_sve_bf16_support : uint8_t {
    // SVE BFloat16 instructions are not implemented.
    ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_NONE,
    // SVE BFCVT, BFCVTNT, BFDOT, BFMLALB, BFMLALT, and BFMMLA instructions are
    // implemented.
    ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_FEAT_BF16,
    // As 0b0001, but the FPCR.EBF field is also supported.
    ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_FEAT_EBF16,
};

enum id_aa64zfr0_el1_flags : uint64_t {
    __ID_AA64ZFR0_EL1_SVEVER = 0b1111ull << 0,
    __ID_AA64ZFR0_EL1_SVE_AES = 0b1111ull << 4,
    // 8 bits unused
    // SVE BDEP, BEXT, and BGRP instructions are implemented.
    __ID_AA64ZFR0_EL1_BITPERM = 0b1111ull << 16,
    __ID_AA64ZFR0_EL1_BF16 = 0b1111ull << 20,
    // SVE2.1 non-widening BFloat16 instructions are implemented.
    __ID_AA64ZFR0_EL1_B16B16 = 0b1111ull << 24,
    // SVE RAX1 instruction is implemented.
    __ID_AA64ZFR0_EL1_SHA3 = 0b1111ull << 32,
    // 4 bits unused
    // SVE SM4E and SM4EKEY instructions are implemented.
    __ID_AA64ZFR0_EL1_SM4 = 0b1111ull << 40,
    // SVE SMMLA, SUDOT, UMMLA, USMMLA, and USDOT instructions are implemented.
    __ID_AA64ZFR0_EL1_I8MM = 0b1111ull << 44,
    // 4 bits unused
    // Single-precision variant of the FMMLA instruction is implemented.
    __ID_AA64ZFR0_EL1_F32MM = 0b1111ull << 52,
    // Double-precision variant of the FMMLA instruction, and the LD1RO*
    // instructions are implemented. The 128-bit element variants of the SVE
    // TRN1, TRN2, UZP1, UZP2, ZIP1, and ZIP2 instructions are also implemented.
    __ID_AA64ZFR0_EL1_F64MM = 0b1111ull << 56,
};

#define ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_SHIFT 0
#define ID_AA64DFR0_EL1_PMU_SUPPORT_SHIFT 8
#define ID_AA64DFR0_EL1_PMS_SUPPORT_SHIFT 32
#define ID_AA64DFR0_EL1_MTPMU_SUPPORT_SHIFT 48

enum id_aa64dfr0_el1_debug_version_support : uint8_t {
    // Armv8 debug architecture.
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_DEFAULT = 0b110,
    // Armv8 debug architecture with Virtualization Host Extensions.
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_VHE,
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p1 =
        ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_VHE,
    // Armv8.2 debug architecture, FEAT_Debugv8p2.
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p2,
    // Armv8.4 debug architecture, FEAT_Debugv8p4.
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p4,
    // Armv8.8 debug architecture, FEAT_Debugv8p8.
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p8,
    // Armv8.9 debug architecture, FEAT_Debugv8p9.
    ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p9,
};

// PMU = Performance Monitors Extension version.
enum id_aa64dfr0_el1_pmu_support : uint8_t {
    // Performance Monitors Extension not implemented.
    ID_AA64DFR0_EL1_PMU_SUPPORT_NONE,
    // Performance Monitors Extension, PMUv3 implemented.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3,
    // PMUv3 for Armv8.1. As 0b0001, and adds support for:
    //  Extended 16-bit PMEVTYPER<n>_EL0.evtCount field.
    //  If EL2 is implemented, the MDCR_EL2.HPMD control.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p1 = 0b100,
    // PMUv3 for Armv8.4. As 0b0100, and adds support for the PMMIR_EL1
    // register.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p4,
    // PMUv3 for Armv8.5. As 0b0101, and adds support for:
    //   64-bit event counters.
    //   If EL2 is implemented, the MDCR_EL2.HCCD control.
    //   If EL3 is implemented, the MDCR_EL3.SCCD control.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p5,
    // PMUv3 for Armv8.7. As 0b0110, and adds support for:
    //   The PMCR_EL0.FZO and, if EL2 is implemented, MDCR_EL2.HPMFZO controls.
    //   If EL3 is implemented, the MDCR_EL3.{MPMX,MCCD} controls.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p7,
    // PMUv3 for Armv8.8. As 0b0111, and:
    //  Extends the Common event number space to include 0x0040 to 0x00BF and
    //      0x4040 to 0x40BF.
    //  Removes the CONSTRAINED UNPREDICTABLE behaviors if a reserved or
    //      unimplemented PMU event number is selected.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p8,
    // PMUv3 for Armv8.9. As 0b1000, and:
    //  Updates the definitions of existing PMU events.
    //  Adds support for the PMUSERENR_EL0.UEN control and the PMUACR_EL1
    //      register.
    //  Adds support for the EDECR.PME control.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p9,
};

// PMS = Statistical Profiling Extension
enum id_aa64dfr0_el1_pms_support : uint8_t {
    // Statistical Profiling Extension not implemented.
    ID_AA64DFR0_EL1_PMS_SUPPORT_NONE,
    // Statistical Profiling Extension implemented.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_SPE,
    // As 0b0001, and adds:
    //  Support for the Events packet Alignment flag.
    //  If FEAT_SVE is implemented, support for the Scalable Vector extensions
    //      to Statistical Profiling.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_SPEv1p1,
    // As 0b0010, and adds:
    //  Discard mode.
    //  Extended event filtering, including the PMSNEVFR_EL1 System register.
    //  Support for the OPTIONAL previous branch target Address packet.
    //  If FEAT_PMUv3 is implemented, controls to freeze the PMU event counters
    //      after an SPE buffer management event occurs.
    //  If FEAT_PMUv3 is implemented, the SAMPLE_FEED_BR, SAMPLE_FEED_EVENT,
    //      SAMPLE_FEED_LAT, SAMPLE_FEED_LD, SAMPLE_FEED_OP, and SAMPLE_FEED_ST
    //      PMU events.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_SPEv1p2,
    // As 0b0011, and adds:
    //  If FEAT_MOPS is implemented, Operation Type packet encodings for Memory
    //      Copy and Set operations.
    //  If FEAT_MTE is implemented, Operation Type packet encodings for loads
    //      and stores of Allocation Tags.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_SPEv1p3,
    // As 0b0100, and adds:
    //  Support for the Events packet Level 2 Data cache access, Level 2 Data
    //      cache miss, Cached data modified, Recently fetched cache line, and
    //      Cache snoop flags.
    //  Support for Data Source filtering.
    ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_SPEv1p4,
};

// MTPMU = Multi-threaded PMU extension
enum id_aa64dfr0_el1_mtpmu_support : uint8_t {
    ID_AA64DFR0_EL1_MTPMU_SUPPORT_NONE,
    // FEAT_MTPMU and FEAT_PMUv3 implemented. PMEVTYPER<n>_EL0.MT and
    // PMEVTYPER<n>.MT are read/write. When FEAT_MTPMU is disabled, the
    //Effective values of PMEVTYPER<n>_EL0.MT and PMEVTYPER<n>.MT are 0.
    ID_AA64DFR0_EL1_MTPMU_SUPPORT_FEAT_MTPMU,
    // FEAT_MTPMU not implemented. If FEAT_PMUv3 is implemented,
    // PMEVTYPER<n>_EL0.MT and PMEVTYPER<n>.MT are RES0.
    ID_AA64DFR0_EL1_MTPMU_SUPPORT_FEAT_MTPMU_3_MAYBE = 0b1111,
};

// BRBE = Branch Record Buffer Extension
enum id_aa64dfr0_el1_brbe_support : uint8_t {
    ID_AA64DFR0_EL1_BRBE_SUPPORT_NONE,
    // Branch Record Buffer Extension implemented.
    ID_AA64DFR0_EL1_BRBE_SUPPORT_FEAT_BRBE,
    // As 0b0001, and adds support for branch recording at EL3.
    ID_AA64DFR0_EL1_BRBE_SUPPORT_FEAT_BRBEv1p1 = 0b1111,
};

enum id_aa64dfr0_el1_shifts : uint8_t {
    IO_AA64DFR0_EL1_PMUVER_SHIFT = 8,
    IO_AA64DFR0_EL1_BRPS_SHIFT = 12,
    IO_AA64DFR0_EL1_WRPS_SHIFT = 20,
    IO_AA64DFR0_EL1_CTX_CMPS_SHIFT = 28,
    IO_AA64DFR0_EL1_MTPMU_SHIFT = 52,
    IO_AA64DFR0_EL1_BRBE_SHIFT = 52,
};

enum id_aa64dfr0_el1_flags : uint64_t {
    __ID_AA64DFR0_EL1_DEBUG_VERSION = 0b1111ull << 0,
    // Trace unit System registers implemented.
    __ID_AA64DFR0_EL1_TRACE_VERSION = 0b1111ull << 4,
    __ID_AA64DFR0_EL1_PMUVER = 0b1111ull << 8,
    // Number of breakpoints, minus 1.
    // If FEAT_Debugv8p9 is implemented and 16 or more breakpoints are
    // implemented, this field reads as 0b1111.
    // The value of 0b0000 is reserved.
    __ID_AA64DFR0_EL1_BRPS = 0b1111ull << 12,
    // PMU snapshot extension implemented.
    __ID_AA64DFR0_EL1_PMSS = 0b1111ull << 16,
    // Number of watchpoints, minus 1.
    // If FEAT_Debugv8p9 is implemented and 16 or more watchpoints are
    // implemented, this field reads as 0b1111.
    // The value of 0b0000 is reserved.
    __ID_AA64DFR0_EL1_WRPS = 0b1111ull << 20,
    // Synchronous-exception-based event profiling implemented.
    __ID_AA64DFR0_EL1_SEBEP = 0b1111ull << 24,
    // Number of context-aware breakpoints, minus 1.
    //  The value of this field is never greater than ID_AA64DFR0_EL1.BRPs.
    //  If FEAT_Debugv8p9 is implemented and 16 or more context-aware
    //      breakpoints are implemented, this field reads as 0b1111.
    __ID_AA64DFR0_EL1_CTX_CMPS = 0b1111ull << 28,
    __ID_AA64DFR0_EL1_PMSVER = 0b1111ull << 32,
    // S Double Lock implemented. OSDLR_EL1 is RW else not implemented and
    // OSDLR_EL1 is RAZ/WI.
    __ID_AA64DFR0_EL1_DOUBLELOCK = 0b1111ull << 36,
    // Armv8.4 Self-hosted Trace Extension implemented.
    __ID_AA64DFR0_EL1_TRACEFILT = 0b1111ull << 49,
    // Trace Buffer Extension implemented.
    __ID_AA64DFR0_EL1_TRACEBUFFER = 0b1111ull << 44,
    __ID_AA64DFR0_EL1_MTPMU = 0b1111ull << 48,
    __ID_AA64DFR0_EL1_BRBE = 0b1111ull << 52,
    // Trace Buffer External Mode implemented.
    __ID_AA64DFR0_EL1_EXTTRCBUFF = 0b1111ull << 56,
    // Setting MDCR_EL2.HPMN to zero has defined behavior.
    __ID_AA64DFR0_EL1_HPMN0 = 0b1111ull << 60,
};

enum id_aa64dfr1_el1_shifts : uint8_t {
    __ID_AA64DFR1_EL1_BRPS_SHIFT = 8,
    __ID_AA64DFR1_EL1_WRPS_SHIFT = 16,
    __ID_AA64DFR1_EL1_CTX_CMPS_SHIFT = 24,
    __ID_AA64DFR1_EL1_ABL_CMPS_SHIFT = 56,
};

enum id_aa64dfr1_el1_flags : uint64_t {
    // The largest supported value that can be written to SPMSELR_EL0.SYSPMUSEL.
    __ID_AA64DFR1_EL1_SYSPMUID = 0xffull << 0,
    // If 0, ID_AA64DFR0_EL1.BRPs is the number of breakpoints, minus 1,
    // otherwise number of breakpoints minus 1.
    __ID_AA64DFR1_EL1_BRPS = 0xffull << __ID_AA64DFR1_EL1_BRPS_SHIFT,
    // If 0, ID_AA64DFR0_EL1.WRPs is the number of watchpoints, minus 1,
    // otherwise number of watchpoints minus 1.
    __ID_AA64DFR1_EL1_WRPS = 0xffull << __ID_AA64DFR1_EL1_WRPS_SHIFT,
    // If 0, ID_AA64DFR0_EL1.CTX_CMPs is the number of context-aware
    // breakpoints, minus 1, otherwise, number of context-aware breakpoints
    // minus 1.
    __ID_AA64DFR1_EL1_CTX_CMPS = 0xffull << __ID_AA64DFR1_EL1_CTX_CMPS_SHIFT,
    // System PMU extension implemented.
    __ID_AA64DFR1_EL1_SPMU = 0b1111ull << 32,
    // PMU fixed-function instruction counter implemented.
    __ID_AA64DFR1_EL1_PMICNTR = 0b1111ull << 36,
    // Address Breakpoint Linking Extension implemented.
    __ID_AA64DFR1_EL1_ABLE = 0b1111ull << 40,
    // Instrumentation Trace Extension implemented.
    __ID_AA64DFR1_EL1_ITE = 0b1111ull << 44,
    // Exception-based event profiling implemented.
    __ID_AA64DFR1_EL1_EBEP = 0b1111ull << 48,
    // The cycle counter PMCCNTR_EL0 does not count when PMCR_EL0.DP is 1 and
    // counting by event counters accessible to EL1 is frozen by the
    // PMCR_EL0.FZS mechanism.
    __ID_AA64DFR1_EL1_DPFZS = 0b1111ull << 52,
    // When FEAT_ABLE is implemented:
    // Number of breakpoints that support address linking, minus 1. Defined
    // values are: 0x0 - 0xff
    __ID_AA64DFR1_EL1_ABL_CMPS = 0xffull << __ID_AA64DFR1_EL1_ABL_CMPS_SHIFT,
};

static inline uint64_t read_id_aa64isar0_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64isar0_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64isar1_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64isar1_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64isar2_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64isar2_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64pfr0_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64pfr0_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64pfr1_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64pfr1_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64pfr2_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64pfr2_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64mmfr0_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64mmfr1_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64mmfr1_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64mmfr2_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64mmfr2_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64mmfr3_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64mmfr3_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64mmfr4_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64mmfr4_el1" : "=r"(result));

    return result;
}

#if CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64SMFR0_EL1 == 1
static inline uint64_t read_id_aa64smfr0_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64smfr0_el1" : "=r"(result));

    return result;
}
#endif

#if CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64ZFR0_EL1 == 1
static inline uint64_t read_id_aa64zfr0_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64zfr0_el1" : "=r"(result));

    return result;
}
#endif

static inline uint64_t read_id_aa64dfr0_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64dfr0_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_id_aa64dfr1_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, id_aa64dfr1_el1" : "=r"(result));

    return result;
}

static inline uint64_t read_mpidr_el1() {
    uint64_t result = 0;
    asm volatile ("mrs %0, mpidr_el1" : "=r"(result));

    return result;
}

static inline uint64_t cpuid(){
    return read_mpidr_el1() & 0xFF;
}

#define read_ttbr_el1(num,val)                             \
({                                                         \
    asm volatile ("mrs %0, ttbr" #num "_el1" : "=r"(val)); \
})

#define write_ttbr_el1(num, value)                           \
{                                                            \
    asm volatile ("msr ttbr" #num "_el1, %0" :: "r"(value)); \
}