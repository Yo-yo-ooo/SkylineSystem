#include <arch/aarch64/cpu/features.h>
#include <arch/aarch64/asm/tcr.h>

bool __aarch64_have_lse_atomics;
struct cpu_features g_cpu_features = {0};

void CollectCPUFeatures(){
    const uint64_t id_aa64isar0 = read_id_aa64isar0_el1();
    const uint64_t id_aa64isar1 = read_id_aa64isar1_el1();
    const uint64_t id_aa64isar2 = read_id_aa64isar2_el1();
    const uint64_t id_aa64pfr0 = read_id_aa64pfr0_el1();
    const uint64_t id_aa64pfr1 = read_id_aa64pfr1_el1();
    const uint64_t id_aa64mmfr0 = read_id_aa64mmfr0_el1();
    const uint64_t id_aa64mmfr1 = read_id_aa64mmfr1_el1();
    const uint64_t id_aa64mmfr2 = read_id_aa64mmfr2_el1();
    const uint64_t id_aa64mmfr3 = read_id_aa64mmfr3_el1();
#if CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64SMFR0_EL1 == 1
    const uint64_t id_aa64smfr0 = read_id_aa64smfr0_el1();
#endif
#if CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64ZFR0_EL1 == 1
    const uint64_t id_aa64zfr0 = read_id_aa64zfr0_el1();
#endif
    const uint64_t id_aa64dfr0 = read_id_aa64dfr0_el1();
    const uint64_t id_aa64dfr1 = read_id_aa64dfr1_el1();
    const uint64_t tcr_el1 = tcr_el1_read();

    const enum id_aa64isar0_el1_aes_support aa64isar0_el1_aes_support =
        (id_aa64isar0 & __ID_AA64ISAR0_EL1_AES) >>
            ID_AA64ISAR0_EL1_AES_SUPPORT_SHIFT;

    switch (aa64isar0_el1_aes_support) {
        case ID_AA64ISAR0_EL1_AES_SUPPORT_NONE:
            break;
        case ID_AA64ISAR0_EL1_AES_SUPPORT_ONLY_AES:
            g_cpu_features.aes = CPU_FEAT_AES;
            break;
        case ID_AA64ISAR0_EL1_AES_SUPPORT_AES_AND_PMULL:
            g_cpu_features.aes = CPU_FEAT_AES_PMULL;
            break;
    }

    g_cpu_features.sha1 = id_aa64isar0 & __ID_AA64ISAR0_EL1_SHA1;
    const enum id_aa64isar0_el1_sha2_support aa64isar0_el1_sha2_support =
        (id_aa64isar0 & __ID_AA64ISAR0_EL1_SHA2) >>
            ID_AA64ISAR0_EL1_SHA2_SUPPORT_SHIFT;

    switch (aa64isar0_el1_sha2_support) {
        case ID_AA64ISAR0_EL1_SHA2_SUPPORT_NONE:
            break;
        case ID_AA64ISAR0_EL1_SHA2_SUPPORT_SHA256:
            g_cpu_features.sha2 = CPU_FEAT_SHA256;
            break;
        case ID_AA64ISAR0_EL1_SHA2_SUPPORT_SHA512:
            g_cpu_features.sha2 = CPU_FEAT_SHA512;
            break;
    }

    g_cpu_features.crc32 = id_aa64isar0 & __ID_AA64ISAR0_EL1_CRC32;
    const enum id_aa64isar0_el1_atomic_support aa64isar0_el1_atomic_support =
        (id_aa64isar0 & __ID_AA64ISAR0_EL1_ATOMIC) >>
            ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_SHIFT;

    switch (aa64isar0_el1_atomic_support) {
        case ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_NONE:
            break;
        case ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_LSE:
            g_cpu_features.atomic = CPU_FEAT_ATOMIC_LSE;
            break;
        case ID_AA64ISAR0_EL1_ATOMIC_SUPPORT_LSE128:
            g_cpu_features.atomic = CPU_FEAT_ATOMIC_LSE128;
            break;
    }

    g_cpu_features.tme = id_aa64isar0 & __ID_AA64ISAR0_EL1_TME;
    g_cpu_features.rdm = id_aa64isar0 & __ID_AA64ISAR0_EL1_RDM;
    g_cpu_features.sha3 = id_aa64isar0 & __ID_AA64ISAR0_EL1_SHA3;
    g_cpu_features.sm3 = id_aa64isar0 & __ID_AA64ISAR0_EL1_SM3;
    g_cpu_features.sm4 = id_aa64isar0 & __ID_AA64ISAR0_EL1_SM4;
    g_cpu_features.dot_product = id_aa64isar0 & __ID_AA64ISAR0_EL1_DP;
    g_cpu_features.fhm = id_aa64isar0 & __ID_AA64ISAR0_EL1_FHM;

    const enum id_aa64isar0_el1_ts_support id_aa64isar0_el1_ts_support =
        (id_aa64isar0 & __ID_AA64ISAR0_EL1_TS) >>
            ID_AA64ISAR0_EL1_TS_SUPPORT_SHIFT;

    switch (id_aa64isar0_el1_ts_support) {
        case ID_AA64ISAR0_EL1_TS_SUPPORT_NONE:
            break;
        case ID_AA64ISAR0_EL1_TS_SUPPORT_FLAG_M:
            g_cpu_features.ts = CPU_FEAT_TS_FLAG_M;
            break;
        case ID_AA64ISAR0_EL1_TS_SUPPORT_FLAG_M2:
            g_cpu_features.ts = CPU_FEAT_TS_FLAG_M2;
            break;
    }

    const enum id_aa64isar0_el1_tlb_support id_aa64isar0_el1_tlb_support =
        (id_aa64isar0 & __ID_AA64ISAR0_EL1_TLB) >>
            ID_AA64ISAR0_EL1_TLB_SUPPORT_SHIFT;

    switch (id_aa64isar0_el1_tlb_support) {
        case ID_AA64ISAR0_EL1_TLB_SUPPORT_NONE:
            break;
        case ID_AA64ISAR0_EL1_TLB_SUPPORT_TLBIOS:
            g_cpu_features.tlb = CPU_FEAT_TLBIOS;
            break;
        case ID_AA64ISAR0_EL1_TLB_SUPPORT_IRANGE:
            g_cpu_features.tlb = CPU_FEAT_TLB_IRANGE;
            break;
    }

    const enum id_aa64isar1_el1_dpb_support id_aa64isar1_el1_dpb_support =
        (id_aa64isar1 & __ID_AA64ISAR1_EL1_DPB) >>
            ID_AA64ISAR1_EL1_DPB_SUPPORT_SHIFT;

    switch (id_aa64isar1_el1_dpb_support) {
        case ID_AA64ISAR1_EL1_DPB_SUPPORT_NONE:
            break;
        case ID_AA64ISAR1_EL1_DPB_SUPPORT_FEAT_DPB:
            g_cpu_features.dpb = CPU_FEAT_DPB;
            break;
        case ID_AA64ISAR1_EL1_DPB_SUPPORT_FEAT_DPB2:
            g_cpu_features.dpb = CPU_FEAT_DPB2;
            break;
    }

    g_cpu_features.rndr = id_aa64isar0 & __ID_AA64ISAR0_EL1_RNDR;
    const enum id_aa64isar1_el1_apa_support aa64isar1_el1_apa_support =
        (id_aa64isar1 & __ID_AA64ISAR1_EL1_APA) >>
            ID_AA64ISAR1_EL1_APA_SUPPORT_SHIFT;
    const enum id_aa64isar1_el1_api_support aa64isar1_el1_api_support =
        (id_aa64isar1 & __ID_AA64ISAR1_EL1_API) >>
            ID_AA64ISAR1_EL1_API_SUPPORT_SHIFT;
    const enum id_aa64isar2_el1_apa3_support aa64isar1_el1_apa3_support =
        (id_aa64isar2 & __ID_AA64ISAR2_EL1_APA3) >>
            ID_AA64ISAR2_EL1_APA3_SUPPORT_SHIFT;

    if (aa64isar1_el1_apa_support ==
            ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_FPACCOMBINE
     && aa64isar1_el1_api_support ==
            ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_FPACCOMBINE
     && aa64isar1_el1_apa3_support ==
            ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_FPACCOMBINE)
    {
        g_cpu_features.pauth = CPU_FEAT_PAUTH_FPACCOMBINE;
    } else if (aa64isar1_el1_apa_support ==
                   ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_PAUTH2
            && aa64isar1_el1_api_support ==
                   ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_PAUTH2
            && aa64isar1_el1_apa3_support ==
                   ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_PAUTH2)
    {
        g_cpu_features.pauth = CPU_FEAT_PAUTH2;
    } else if (aa64isar1_el1_apa_support ==
                    ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_EPAC
            && aa64isar1_el1_api_support ==
                   ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_EPAC
            && aa64isar1_el1_apa3_support ==
                   ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_EPAC)
    {
        g_cpu_features.pauth = CPU_FEAT_PAUTH_EPAC;
    } else if (aa64isar1_el1_apa_support ==
               ID_AA64ISAR1_EL1_APA_SUPPORT_FEAT_PAUTH
            && aa64isar1_el1_api_support ==
                   ID_AA64ISAR1_EL1_API_SUPPORT_FEAT_PAUTH
            && aa64isar1_el1_apa3_support ==
                   ID_AA64ISAR2_EL1_APA3_SUPPORT_FEAT_PAUTH)
    {
        g_cpu_features.pauth = CPU_FEAT_PAUTH;
    }

    g_cpu_features.jscvt = id_aa64isar1 & __ID_AA64ISAR1_EL1_JSCVT;
    g_cpu_features.fcma = id_aa64isar1 & __ID_AA64ISAR1_EL1_FCMA;

    const enum id_aa64isar1_el1_lrcpc_support id_aa64isar1_el1_lrcpc_support =
        (id_aa64isar1 & __ID_AA64ISAR1_EL1_LRCPC) >>
            ID_AA64ISAR1_EL1_LRCPC_SUPPORT_SHIFT;

    switch (id_aa64isar1_el1_lrcpc_support) {
        case ID_AA64ISAR1_EL1_LRCPC_SUPPORT_NONE:
            break;
        case ID_AA64ISAR1_EL1_LRCPC_SUPPORT_FEAT_LRCPC:
            g_cpu_features.lrcpc = CPU_FEAT_LRCPC;
            break;
        case ID_AA64ISAR1_EL1_LRCPC_SUPPORT_FEAT_LRCPC2:
            g_cpu_features.lrcpc = CPU_FEAT_LRCPC2;
            break;
        case ID_AA64ISAR1_EL1_LRCPC_SUPPORT_FEAT_LRCPC3:
            g_cpu_features.lrcpc = CPU_FEAT_LRCPC3;
            break;
    }

    g_cpu_features.pacqarma5 =
        id_aa64isar1 & __ID_AA64ISAR1_EL1_GPA
     && aa64isar1_el1_apa_support > ID_AA64ISAR1_EL1_APA_SUPPORT_NONE;

    g_cpu_features.pacimp =
        id_aa64isar1 & __ID_AA64ISAR1_EL1_GPI
     && aa64isar1_el1_apa_support > ID_AA64ISAR1_EL1_APA_SUPPORT_NONE;

    g_cpu_features.frintts = id_aa64isar1 & __ID_AA64ISAR1_EL1_FRINTTS;
    g_cpu_features.sb = id_aa64isar1 & __ID_AA64ISAR1_EL1_SB;

    const enum id_aa64isar1_el1_specres_support specres_support =
        (id_aa64isar1 & __ID_AA64ISAR1_EL1_SPECRES) >>
            ID_AA64ISAR1_EL1_SPECRES_SUPPORT_SHIFT;

    switch (specres_support) {
        case ID_AA64ISAR1_EL1_SPECRES_SUPPORT_NONE:
            break;
        case ID_AA64ISAR1_EL1_SPECRES_SUPPORT_FEAT_SPECRES2:
            g_cpu_features.specres = CPU_FEAT_SPECRES;
            break;
        case ID_AA64ISAR1_EL1_SPECRES_SUPPORT_FEAT_SPECRES:
            g_cpu_features.specres = CPU_FEAT_SPECRES2;
            break;
    }

    const enum id_aa64isar1_el1_bf16_support id_aa64isar1_el1_bf16_support =
        (id_aa64isar1 & __ID_AA64ISAR1_EL1_BF16) >>
            ID_AA64ISAR1_EL1_BF16_SUPPORT_SHIFT;

    switch (id_aa64isar1_el1_bf16_support) {
        case ID_AA64ISAR1_EL1_BF16_SUPPORT_NONE:
            break;
        case ID_AA64ISAR1_EL1_BF16_SUPPORT_FEAT_BF16:
            g_cpu_features.bf16 = CPU_FEAT_BF16;
            break;
        case ID_AA64ISAR1_EL1_BF16_SUPPORT_FEAT_EBF16:
            g_cpu_features.bf16 = CPU_FEAT_BF16;
            break;
    }

    g_cpu_features.dgh = id_aa64isar1 & __ID_AA64ISAR1_EL1_DGH;
    g_cpu_features.i8mm = id_aa64isar1 & __ID_AA64ISAR1_EL1_I8MM;
    g_cpu_features.xs = id_aa64isar1 & __ID_AA64ISAR1_EL1_XS;

    const enum id_aa64isar1_el1_ls64_support id_aa64isar1_el1_ls64_support =
        (id_aa64isar1 & ID_AA64ISAR1_EL1_LS64_SUPPORT_SHIFT) >>
            ID_AA64ISAR1_EL1_LS64_SUPPORT_SHIFT;

    switch (id_aa64isar1_el1_ls64_support) {
        case ID_AA64ISAR1_EL1_LS64_SUPPORT_NONE:
            break;
        case ID_AA64ISAR1_EL1_LS64_SUPPORT_FEAT_LS64:
            g_cpu_features.ls64 = CPU_FEAT_LS64;
            break;
        case ID_AA64ISAR1_EL1_LS64_SUPPORT_FEAT_LS64_V:
            g_cpu_features.ls64 = CPU_FEAT_LS64V;
            break;
        case ID_AA64ISAR1_EL1_LS64_SUPPORT_FEAT_LS64_ACCDATA:
            g_cpu_features.ls64 = CPU_FEAT_LS64_ACCDATA;
            break;
    }

    g_cpu_features.wfxt = id_aa64isar2 & __ID_AA64ISAR2_EL1_WFxT;
    g_cpu_features.rpres = id_aa64isar2 & __ID_AA64ISAR2_EL1_RPRES;
    g_cpu_features.pacqarma3 =
        id_aa64isar2 & __ID_AA64ISAR2_EL1_GPA3
     && aa64isar1_el1_apa3_support == ID_AA64ISAR2_EL1_APA3_SUPPORT_NONE;

    g_cpu_features.mops = id_aa64isar2 & __ID_AA64ISAR2_EL1_MOPS;
    g_cpu_features.hbc = id_aa64isar2 & __ID_AA64ISAR2_EL1_BC;
    g_cpu_features.clrbhb = id_aa64isar2 & __ID_AA64ISAR2_EL1_CLRBHB;
    g_cpu_features.sysreg128 = id_aa64isar2 & __ID_AA64ISAR2_EL1_SYSREG_128;
    g_cpu_features.sysinstr128 = id_aa64isar2 & __ID_AA64ISAR2_EL1_SYSINSTR_128;
    g_cpu_features.prfmslc = id_aa64isar2 & __ID_AA64ISAR2_EL1_PRFMSLC;

    const enum id_aa64pfr0_el1_fp_support id_aa64pfr0_el1_fp_support =
        (id_aa64pfr0 & __ID_AA64PFR0_EL1_FP) >>
            ID_AA64PFR0_EL1_FP_SUPPORT_SHIFT;

    switch (id_aa64pfr0_el1_fp_support) {
        case ID_AA64PFR0_EL1_FP_SUPPORT_PARTIAL:
            g_cpu_features.fp = CPU_FEAT_FP_PARTIAL;
            g_cpu_features.fp16 = true;

            break;
        case ID_AA64PFR0_EL1_FP_SUPPORT_FULL:
            g_cpu_features.fp = CPU_FEAT_FP_FULL;
            g_cpu_features.fp16 = true;

            break;
        case ID_AA64PFR0_EL1_FP_SUPPORT_NONE:
            break;
    }

    const enum id_aa64pfr0_el1_adv_simd_support adv_simd_support =
        (id_aa64pfr0 & __ID_AA64PFR0_EL1_ADV_SIMD) >>
            ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_SHIFT;

    switch (adv_simd_support) {
        case ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_PARTIAL:
            g_cpu_features.adv_simd = CPU_FEAT_ADV_SIMD_PARTIAL;
            break;
        case ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_FULL:
            g_cpu_features.adv_simd = CPU_FEAT_ADV_SIMD_FULL;
            g_cpu_features.fp16 = true;

            break;
        case ID_AA64PFR0_EL1_ADV_SIMD_SUPPORT_NONE:
            break;
    }

    const enum id_aa64pfr0_el1_gic_support id_aa64pfr0_el1_gic_support =
        (id_aa64pfr0 & ID_AA64PFR0_EL1_GIC_SUPPORT_SHIFT) >>
            ID_AA64PFR0_EL1_GIC_SUPPORT_SHIFT;

    switch (id_aa64pfr0_el1_gic_support) {
        case ID_AA64PFR0_EL1_GIC_SUPPORT_NONE:
            break;
        case ID_AA64PFR0_EL1_GIC_SUPPORT_V4:
            g_cpu_features.gic = CPU_FEAT_GIC_V4;
            break;
        case ID_AA64PFR0_EL1_GIC_SUPPORT_V4p1:
            g_cpu_features.gic = CPU_FEAT_GIC_V4p1;
            break;
    }

    const enum id_aa64pfr0_el1_ras_support id_aa64pfr0_el1_ras_support =
        (id_aa64pfr0 & ID_AA64PFR0_EL1_RAS_SUPPORT_SHIFT) >>
            ID_AA64PFR0_EL1_RAS_SUPPORT_SHIFT;

    switch (id_aa64pfr0_el1_ras_support) {
        case ID_AA64PFR0_EL1_RAS_SUPPORT_NONE:
        case ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RAS:
            g_cpu_features.ras = CPU_FEAT_RAS;
            break;
        case ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RASv1p1:
            g_cpu_features.ras = CPU_FEAT_RASv1p1;
            break;
        case ID_AA64PFR0_EL1_RAS_SUPPORT_FEAT_RASv2:
            g_cpu_features.ras = CPU_FEAT_RASv2;
            break;
    }

#if CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64SMFR0_EL1 == 1 && \
    CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64ZFR0_EL1 == 1
    const enum id_aa64zfr0_el1_sve_support id_aa64zfr0_el1_sve_support =
        (id_aa64zfr0 & __ID_AA64ZFR0_EL1_SVEVER) >>
            ID_AA64ZFR0_EL1_SVE_SUPPORT_SHIFT;

    switch (id_aa64zfr0_el1_sve_support) {
        case ID_AA64ZFR0_EL1_SVE_SUPPORT_SVE:
            g_cpu_features.sve = CPU_FEAT_SVE;
            break;
        case ID_AA64ZFR0_EL1_SVE_SUPPORT_SVE2:
            g_cpu_features.sve = CPU_FEAT_SVE2;
            break;
        case ID_AA64ZFR0_EL1_SVE_SUPPORT_SVE2p1:
            g_cpu_features.sve = CPU_FEAT_SVE2p1;
            break;
    }

    const enum id_aa64zfr0_el1_sve_aes_support id_aa64zfr0_el1_sve_aes_support =
        (id_aa64zfr0 & __ID_AA64ZFR0_EL1_SVE_AES) >>
            ID_AA64ZFR0_EL1_SVE_AES_SUPPORT_SHIFT;

    switch (id_aa64zfr0_el1_sve_aes_support) {
        case ID_AA64ZFR0_EL1_SVE_AVS_SUPPORT_NONE:
            break;
        case ID_AA64ZFR0_EL1_SVE_AVS_SUPPORT_FEAT_AES:
            g_cpu_features.sve_aes = CPU_FEAT_SVE_AES;
            break;
        case ID_AA64ZFR0_EL1_SVE_AES_SUPPORT_FEAT_SVE_PMULL128:
            g_cpu_features.sve_aes = CPU_FEAT_SVE_AES_PMULL128;
            break;
    }


    const enum id_aa64zfr0_el1_sve_bf16_support sve_bf16_support =
        (id_aa64zfr0 & __ID_AA64ZFR0_EL1_BF16) >>
            ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_SHIFT;

    switch (sve_bf16_support) {
        case ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_NONE:
        case ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_FEAT_BF16:
            g_cpu_features.sve_bf16 = CPU_FEAT_SVE_BF16;
            break;
        case ID_AA64ZFR0_EL1_SVE_BF16_SUPPORT_FEAT_EBF16:
            g_cpu_features.sve_bf16 = CPU_FEAT_SVE_EBF16;
            break;
    }
#endif

    g_cpu_features.sel2 = id_aa64pfr0 & __ID_AA64PFR0_EL1_SEL2;
    const enum id_aa64pfr0_el1_amu_support id_aa64pfr0_el1_amu_support =
        (id_aa64pfr0 & __ID_AA64ZFR0_EL1_BF16) >>
            ID_AA64PFR0_EL1_AMU_SUPPORT_SHIFT;

    switch (id_aa64pfr0_el1_amu_support) {
        case ID_AA64PFR0_EL1_AMU_SUPPORT_NONE:
            break;
        case ID_AA64PFR0_EL1_AMU_SUPPORT_FEAT_AMUv1:
            g_cpu_features.amu = CPU_FEAT_AMU_AMUv1;
            break;
        case ID_AA64PFR0_EL1_AMU_SUPPORT_FEAT_AMUv1p1:
            g_cpu_features.amu = CPU_FEAT_AMU_AMUv1p1;
            break;
    }

    g_cpu_features.dit = id_aa64pfr0 & __ID_AA64PFR0_EL1_DIT;
    g_cpu_features.rme = id_aa64pfr0 & __ID_AA64PFR0_EL1_RME;

    const enum id_aa64pfr0_el1_csv2_support id_aa64pfr0_el1_csv2_support =
        (id_aa64pfr0 & __ID_AA64PFR0_EL1_CSV2) >>
            ID_AA64PFR0_EL1_CSV2_SUPPORT_SHIFT;

    switch (id_aa64pfr0_el1_csv2_support) {
        case ID_AA64PFR0_EL1_CSV2_SUPPORT_NONE:
            break;
        case ID_AA64PFR0_EL1_CSV2_SUPPORT_FEAT_CSV2:
            g_cpu_features.csv2 = CPU_FEAT_CSV2;
            break;
        case ID_AA64PFR0_EL1_CSV2_SUPPORT_FEAT_CSV2_2:
            g_cpu_features.csv2 = CPU_FEAT_CSV2_2;
            break;
        case ID_AA64PFR0_EL1_CSV2_SUPPORT_FEAT_CSV2_3:
            g_cpu_features.csv2 = CPU_FEAT_CSV2_3;
            break;
    }

    g_cpu_features.csv3 = id_aa64pfr0 & __ID_AA64PFR0_EL1_CSV3;
    g_cpu_features.bti = id_aa64pfr0 & __ID_AA64PFR1_EL1_BT;

    const enum id_aa64pfr1_el1_ssbs_support id_aa64pfr1_el1_ssbs_support =
        (id_aa64pfr1 & __ID_AA64PFR1_EL1_SSBS) >>
            ID_AA64PFR1_EL1_SSBS_SUPPORT_SHIFT;

    switch (id_aa64pfr1_el1_ssbs_support) {
        case ID_AA64PFR1_EL1_SSBS_SUPPORT_NONE:
            break;
        case ID_AA64PFR1_EL1_SSBS_SUPPORT_FEAT_SSBS:
            g_cpu_features.ssbs = CPU_FEAT_SSBS;
            break;
        case ID_AA64PFR1_EL1_SSBS_SUPPORT_FEAT_SSBS2:
            g_cpu_features.ssbs = CPU_FEAT_SSBS2;
            break;
    }

    const enum id_aa64pfr1_el1_mte_support id_aa64pfr1_el1_mte_support =
        (id_aa64pfr1 & __ID_AA64PFR1_EL1_MTE) >>
            ID_AA64PFR1_EL1_MTE_SUPPORT_SHIFT;

    switch (id_aa64pfr1_el1_mte_support) {
        case ID_AA64PFR1_EL1_MTE_SUPPORT_NONE:
            break;
        case ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE:
            g_cpu_features.mte = CPU_FEAT_MTE;
            break;
        case ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE2:
            g_cpu_features.mte = CPU_FEAT_MTE2;
            break;
        case ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE3:
            g_cpu_features.mte = CPU_FEAT_MTE3;
            break;
        default:
            if (id_aa64pfr1_el1_mte_support >
                    ID_AA64PFR1_EL1_MTE_SUPPORT_FEAT_MTE3)
            {
                if (id_aa64pfr1 & __ID_AA64PFR1_EL1_MTEX) {
                    g_cpu_features.mte = CPU_FEAT_MTEX;
                }
            }

            break;
    }

    const enum id_aa64mmfr0_sme_version id_aa64mmfr0_sme_version =
        (id_aa64mmfr0 & __ID_AA64SMFR0_SMEVER) >>
            ID_AA64MMFR0_SME_VERSION_SHIFT;

    switch (id_aa64mmfr0_sme_version) {
        case ID_AA64MMFR0_SME_VERSION_FEAT_SME:
            g_cpu_features.sme = CPU_FEAT_SME;
            break;
        case ID_AA64MMFR0_SME_VERSION_FEAT_SME2:
            g_cpu_features.sme = CPU_FEAT_SME2;
            break;
        case ID_AA64MMFR0_SME_VERSION_FEAT_SME2p1:
            g_cpu_features.sme = CPU_FEAT_SME2p1;
            break;
    }

    g_cpu_features.rndr_trap = id_aa64pfr1 & __ID_AA64PFR1_EL1_RNDR_TRAP;
    g_cpu_features.nmi = id_aa64pfr1 & __ID_AA64PFR1_EL1_NMI;
    g_cpu_features.gcs = id_aa64pfr1 & __ID_AA64PFR1_EL1_GCS;
    g_cpu_features.the = id_aa64pfr1 & __ID_AA64PFR1_EL1_THE;
    g_cpu_features.pfar = id_aa64pfr1 & __ID_AA64PFR1_EL1_PFAR;
    g_cpu_features.pa_range =
        (id_aa64mmfr0 & __ID_AA64MMFR0_PARANGE) >> ID_AA64MMFR0_PA_RANGE_SHIFT;

    if (g_cpu_features.pa_range == ID_AA64MMFR0_PA_RANGE_52B_4PIB) {
        if (tcr_el1 & __TCR_DS) {
            g_cpu_features.lpa2 = true;
        } else {
            g_cpu_features.lpa = true;
        }
    }

    g_cpu_features.exs = id_aa64mmfr0 & __ID_AA64MMFR0_EXS;
    const enum id_aa64mmfr0_fgt_support id_aa64mmfr0_fgt_support =
        (id_aa64mmfr0 & __ID_AA64MMFR0_FGT) >> ID_AA64MMFR0_FGT_SUPPORT_SHIFT;

    switch (id_aa64mmfr0_fgt_support) {
        case ID_AA64MMFR0_FGT_SUPPORT_NONE:
            break;
        case ID_AA64MMFR0_FGT_SUPPORT_FEAT_FGT:
            g_cpu_features.fgt = CPU_FEAT_FGT;
            break;
        case ID_AA64MMFR0_FGT_SUPPORT_FEAT_FGT2:
            g_cpu_features.fgt = CPU_FEAT_FGT2;
            break;
    }

    if (id_aa64mmfr0 & __ID_AA64MMFR0_ECV) {
        const enum id_aa64mmfr0_ecv_support id_aa64mmfr0_ecv_support =
            (id_aa64mmfr0 & __ID_AA64MMFR0_ECV) >>
                ID_AA64MMFR0_ECV_SUPPORT_SHIFT;

        switch (id_aa64mmfr0_ecv_support) {
            case ID_AA64MMFR0_ECV_SUPPORT_NONE:
                break;
            case ID_AA64MMFR0_ECV_SUPPORT_FEAT_ECV_PARTIAL:
                g_cpu_features.ecv = CPU_FEAT_ECV_PARTIAL;
                break;
            case ID_AA64MMFR0_ECV_SUPPORT_FEAT_ECV_FULL:
                g_cpu_features.ecv = CPU_FEAT_ECV_FULL;
                break;
        }
    }

    const enum id_aa64mmfr1_hafdbs_support id_aa64mmfr1_hafdbs_support =
        (id_aa64mmfr1 & __ID_AA64MMFR1_HAFDBS) >>
            ID_AA64MMFR1_HAFDBS_SUPPORT_SHIFT;

    switch (id_aa64mmfr1_hafdbs_support) {
        case ID_AA64MMFR1_HAFDBS_SUPPORT_NONE:
            break;
        case ID_AA64MMFR1_HAFDBS_SUPPORT_FEAT_HAFDBS_ACCESS_BIT:
            g_cpu_features.hafdbs = CPU_FEAT_HAFDBS_ONLY_ACCESS_BIT;
            break;
        case ID_AA64MMFR1_HAFDBS_SUPPORT_FEAT_HAFDBS_ACCESS_AND_DIRTY:
            g_cpu_features.hafdbs = CPU_FEAT_HAFDBS_ACCESS_AND_DIRTY_BIT;
            break;
        case ID_AA64MMFR1_HAFDBS_SUPPORT_FEAT_HAFT:
            g_cpu_features.hafdbs = CPU_FEAT_HAFDBS_FULL;
            break;
    }

    g_cpu_features.vmid16 = id_aa64mmfr1 & __ID_AA64MMFR1_VMIDBITS;
    g_cpu_features.haft = id_aa64mmfr1 & __ID_AA64MMFR1_VH;

    const enum id_aa64mmfr1_hpds_support id_aa64mmfr1_hpds_support =
        (id_aa64mmfr1 & __ID_AA64MMFR1_HPDS) >> ID_AA64MMFR1_HPDS_SUPPORT_SHIFT;

    switch (id_aa64mmfr1_hpds_support) {
        case ID_AA64MMFR1_HPDS_SUPPORT_NONE:
            break;
        case ID_AA64MMFR1_HPDS_SUPPORT_FEAT_HPDS:
            g_cpu_features.hpds = CPU_FEAT_HPDS;
            break;
        case ID_AA64MMFR1_HPDS_SUPPORT_FEAT_HPDS2:
            g_cpu_features.hpds = CPU_FEAT_HPDS2;
            break;
    }

    g_cpu_features.lor = id_aa64mmfr1 & __ID_AA64MMFR1_LO;

    const enum id_aa64mmfr1_pan_support id_aa64mmfr1_pan_support =
        (id_aa64mmfr1 & __ID_AA64MMFR1_PAN) >> ID_AA64MMFR1_PAN_SUPPORT_SHIFT;

    switch (id_aa64mmfr1_pan_support) {
        case ID_AA64MMFR1_PAN_SUPPORT_NONE:
            break;
        case ID_AA64MMFR1_PAN_SUPPORT_FEAT_PAN:
            g_cpu_features.pan = CPU_FEAT_PAN;
            break;
        case ID_AA64MMFR1_PAN_SUPPORT_FEAT_PAN2:
            g_cpu_features.pan = CPU_FEAT_PAN2;
            break;
        case ID_AA64MMFR1_PAN_SUPPORT_FEAT_PAN3:
            g_cpu_features.pan = CPU_FEAT_PAN3;
            break;
    }

    g_cpu_features.xnx = id_aa64mmfr1 & __ID_AA64MMFR1_XNX;
    g_cpu_features.twed = id_aa64mmfr1 & __ID_AA64MMFR1_TWED;
    g_cpu_features.ets = id_aa64mmfr1 & __ID_AA64MMFR1_ETS;
    g_cpu_features.hcx = id_aa64mmfr1 & __ID_AA64MMFR1_HCX;
    g_cpu_features.afp = id_aa64mmfr1 & __ID_AA64MMFR1_AFP;
    g_cpu_features.ntlbpa = id_aa64mmfr2 & __ID_AA64MMFR1_NTLBPA;
    g_cpu_features.tidcp1 = id_aa64mmfr2 & __ID_AA64MMFR1_TIDCP1;
    g_cpu_features.cmow = id_aa64mmfr2 & __ID_AA64MMFR1_CMOW;
    g_cpu_features.ecbhb = id_aa64mmfr2 & __ID_AA64MMFR1_ECBHB;
    g_cpu_features.ttcnp = id_aa64mmfr2 & __ID_AA64MMFR2_CNP;
    g_cpu_features.uao = id_aa64mmfr2 & __ID_AA64MMFR2_UAO;
    g_cpu_features.lsmaoc = id_aa64mmfr2 & __ID_AA64MMFR2_LSM;
    g_cpu_features.iesb = id_aa64mmfr2 & __ID_AA64MMFR2_IESB;
    g_cpu_features.ccidx = id_aa64mmfr2 & __ID_AA64MMFR2_CCIDX;
    g_cpu_features.lva =
        id_aa64mmfr2 & __ID_AA64MMFR2_VARANGE || tcr_el1 & __TCR_DS;

    const enum id_aa64mmfr2_nv_support id_aa64mmfr2_nv_support =
        (id_aa64mmfr2 & __ID_AA64MMFR2_NV) >> ID_AA64MMFR2_NV_SUPPORT_SHIFT;

    switch (id_aa64mmfr2_nv_support) {
        case ID_AA64MMFR2_NV_SUPPORT_NONE:
            break;
        case ID_AA64MMFR2_NV_SUPPORT_FEAT_NV:
            g_cpu_features.nv = CPU_FEAT_NV;
            break;
        case ID_AA64MMFR2_NV_SUPPORT_FEAT_NV2:
            g_cpu_features.nv = CPU_FEAT_NV2;
            break;
    }

    g_cpu_features.ttst = id_aa64mmfr2 & __ID_AA64MMFR2_ST;
    g_cpu_features.lse2 = id_aa64mmfr2 & __ID_AA64MMFR2_AT;
    g_cpu_features.idst = id_aa64mmfr2 & __ID_AA64MMFR2_IDS;
    g_cpu_features.s2fwb = id_aa64mmfr2 & __ID_AA64MMFR2_FWB;
    g_cpu_features.ttl = id_aa64mmfr2 & __ID_AA64MMFR2_TTL;

    const enum id_aa64mmfr2_bbm_support id_aa64mmfr2_bbm_support =
        (id_aa64mmfr2 & __ID_AA64MMFR2_BBM) >> ID_AA64MMFR2_BBM_SUPPORT_SHIFT;

    switch (id_aa64mmfr2_bbm_support) {
        case ID_AA64MMFR2_BBM_SUPPORT_LEVEL0:
            g_cpu_features.bbm = CPU_FEAT_BBM_LVL0;
            break;
        case ID_AA64MMFR2_BBM_SUPPORT_LEVEL1:
            g_cpu_features.bbm = CPU_FEAT_BBM_LVL1;
            break;
        case ID_AA64MMFR2_BBM_SUPPORT_LEVEL2:
            g_cpu_features.bbm = CPU_FEAT_BBM_LVL2;
            break;
    }

    g_cpu_features.evt = id_aa64mmfr2 & __ID_AA64MMFR2_EVT;
    g_cpu_features.e0pd = id_aa64mmfr2 & __ID_AA64MMFR2_E0PD;

    if (g_cpu_features.e0pd) {
        g_cpu_features.csv3 = true;
    }

    const enum id_aa64mmfr3_anerr_support id_aa64mmfr3_anerr_support =
        (id_aa64mmfr3 & __ID_AA64MMFR3_ANERR) >>
            ID_AA64MMFR3_ANERR_SUPPORT_SHIFT;
    const enum id_aa64mmfr3_snerr_support id_aa64mmfr3_snerr_support =
        (id_aa64mmfr3 & __ID_AA64MMFR3_SNERR) >>
            ID_AA64MMFR3_SNERR_SUPPORT_SHIFT;

    if (id_aa64mmfr3_anerr_support == ID_AA64MMFR3_ANERR_SUPPORT_FEAT_ANERR
     && id_aa64mmfr3_snerr_support == ID_AA64MMFR3_SNERR_SUPPORT_FEAT_ANERR)
    {
        g_cpu_features.anerr = true;
    }

    const enum id_aa64mmfr3_aderr_support id_aa64mmfr3_aderr_support =
        (id_aa64mmfr3 & __ID_AA64MMFR3_ADERR) >>
            ID_AA64MMFR3_ADERR_SUPPORT_SHIFT;
    const enum id_aa64mmfr3_sderr_support id_aa64mmfr3_sderr_support =
        (id_aa64mmfr3 & __ID_AA64MMFR3_SDERR) >>
            ID_AA64MMFR3_SDERR_SUPPORT_SHIFT;

    if (id_aa64mmfr3_aderr_support == ID_AA64MMFR3_ADERR_SUPPORT_FEAT_ADERR
     && id_aa64mmfr3_sderr_support == ID_AA64MMFR3_SDERR_SUPPORT_FEAT_ADERR)
    {
        g_cpu_features.aderr = true;
    }

    g_cpu_features.aie = id_aa64mmfr3 & __ID_AA64MMFR3_AIE;
    g_cpu_features.mec = id_aa64mmfr3 & __ID_AA64MMFR3_MEC;
    g_cpu_features.s1pie = id_aa64mmfr3 & __ID_AA64MMFR3_S1PIE;
    g_cpu_features.s2pie = id_aa64mmfr3 & __ID_AA64MMFR3_S2PIE;
    g_cpu_features.s1poe = id_aa64mmfr3 & __ID_AA64MMFR3_S1POE;
    g_cpu_features.s2poe = id_aa64mmfr3 & __ID_AA64MMFR3_S2POE;
#if CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64SMFR0_EL1 == 1 && \
    CONFIG_DOSENT_SUPPORT_SYS_REG_ID_AA64ZFR0_EL1 == 1
    g_cpu_features.sve_bitperm = id_aa64zfr0 & __ID_AA64ZFR0_EL1_BITPERM;

    g_cpu_features.sme_f16f16 = id_aa64smfr0 & __ID_AA64SMFR0_F16F16;
    g_cpu_features.sme_f64f64 = id_aa64smfr0 & __ID_AA64SMFR0_F64F64;
    g_cpu_features.sme_i16i64 = id_aa64smfr0 & __ID_AA64SMFR0_I16I64;

    if (g_cpu_features.sme != CPU_FEAT_NONE) {
        g_cpu_features.sme_fa64 = id_aa64smfr0 & __ID_AA64SMFR0_FA64;
    }

    g_cpu_features.b16b16 =
        id_aa64zfr0 & __ID_AA64ZFR0_EL1_B16B16
     && id_aa64smfr0 & __ID_AA64SMFR0_B16B16;

    g_cpu_features.sve_sha3 = id_aa64zfr0 & __ID_AA64ZFR0_EL1_SHA3;
    g_cpu_features.sve_sm4 = id_aa64zfr0 & __ID_AA64ZFR0_EL1_SM4;
#endif

    g_cpu_features.i8mm = id_aa64isar1 & __ID_AA64ISAR1_EL1_I8MM;
    g_cpu_features.xs = id_aa64isar1 & __ID_AA64ISAR1_EL1_XS;
    g_cpu_features.ls64 = id_aa64isar1 & __ID_AA64ISAR1_EL1_LS64;

    const enum id_aa64dfr0_el1_debug_version_support debug_version_support =
        (id_aa64dfr0 & __ID_AA64DFR0_EL1_DEBUG_VERSION) >>
            ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_SHIFT;

    switch (debug_version_support) {
        case ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_DEFAULT:
            g_cpu_features.debug = CPU_FEAT_DEBUG_DEFAULT;
            break;
        case ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_VHE:
            g_cpu_features.debug = CPU_FEAT_DEBUG_VHE;
            break;
        case ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p2:
            g_cpu_features.debug = CPU_FEAT_DEBUG_v8p2;
            break;
        case ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p4:
            g_cpu_features.debug = CPU_FEAT_DEBUG_v8p4;
            break;
        case ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p8:
            g_cpu_features.debug = CPU_FEAT_DEBUG_v8p8;
            break;
        case ID_AA64DFR0_EL1_DEBUG_VERSION_SUPPORT_FEAT_Debugv8p9:
            g_cpu_features.debug = CPU_FEAT_DEBUG_v8p9;
            break;
    }

    const enum id_aa64dfr0_el1_pmu_support id_aa64dfr0_el1_pmu_support =
        (id_aa64dfr0 & __ID_AA64DFR0_EL1_PMUVER) >>
            IO_AA64DFR0_EL1_PMUVER_SHIFT;

    switch (id_aa64dfr0_el1_pmu_support) {
        case ID_AA64DFR0_EL1_PMU_SUPPORT_NONE:
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3;
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p1:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3p1;
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p4:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3p4;
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p5:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3p5;
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p7:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3p7;
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p8:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3p8;
            break;
        case ID_AA64DFR0_EL1_PMU_SUPPORT_FEAT_PMUv3p9:
            g_cpu_features.pmu = CPU_FEAT_PMU_FEAT_PMUv3p9;
            break;
    }

    g_cpu_features.pmuv3_ss = id_aa64dfr0 & __ID_AA64DFR0_EL1_PMSS;
    g_cpu_features.sebep = id_aa64dfr0 & __ID_AA64DFR0_EL1_SEBEP;
    g_cpu_features.double_lock =
        id_aa64dfr0 & __ID_AA64DFR0_EL1_DOUBLELOCK;

    g_cpu_features.trf = id_aa64dfr0 & __ID_AA64DFR0_EL1_TRACEFILT;
    g_cpu_features.trbe = id_aa64dfr0 & __ID_AA64DFR0_EL1_TRACEBUFFER;

    const enum id_aa64dfr0_el1_mtpmu_support id_aa64dfr0_el1_mtpmu_support =
        (id_aa64dfr0 & __ID_AA64DFR0_EL1_MTPMU) >> IO_AA64DFR0_EL1_MTPMU_SHIFT;

    switch (id_aa64dfr0_el1_mtpmu_support) {
        case ID_AA64DFR0_EL1_MTPMU_SUPPORT_NONE:
            break;
        case ID_AA64DFR0_EL1_MTPMU_SUPPORT_FEAT_MTPMU:
            g_cpu_features.mtpmu = CPU_FEAT_MTPMU;
            break;
        case ID_AA64DFR0_EL1_MTPMU_SUPPORT_FEAT_MTPMU_3_MAYBE:
            g_cpu_features.mtpmu = CPU_FEAT_MTPMU_3_MAYBE;
            break;
    }

    const enum id_aa64dfr0_el1_brbe_support id_aa64dfr0_el1_brbe_support =
        (id_aa64dfr0 & __ID_AA64DFR0_EL1_BRBE) >> IO_AA64DFR0_EL1_MTPMU_SHIFT;

    switch (id_aa64dfr0_el1_brbe_support) {
        case ID_AA64DFR0_EL1_BRBE_SUPPORT_NONE:
            break;
        case ID_AA64DFR0_EL1_BRBE_SUPPORT_FEAT_BRBE:
            g_cpu_features.brbe = CPU_FEAT_BRBE;
            break;
        case ID_AA64DFR0_EL1_BRBE_SUPPORT_FEAT_BRBEv1p1:
            g_cpu_features.brbe = CPU_FEAT_BRBEv1p1;
            break;
    }

    g_cpu_features.trbe_ext =
        id_aa64dfr0 & __ID_AA64DFR0_EL1_EXTTRCBUFF
      & id_aa64dfr0 & __ID_AA64DFR0_EL1_TRACEBUFFER;

    g_cpu_features.hpmn0 = id_aa64dfr0 & __ID_AA64DFR0_EL1_HPMN0;
    g_cpu_features.able = id_aa64dfr1 & __ID_AA64DFR1_EL1_ABLE;
    g_cpu_features.ebep = id_aa64dfr1 & __ID_AA64DFR1_EL1_EBEP;
    g_cpu_features.ite = id_aa64dfr1 & __ID_AA64DFR1_EL1_ITE;
    g_cpu_features.pmuv3_icntr = id_aa64dfr1 & __ID_AA64DFR1_EL1_PMICNTR;
    g_cpu_features.spmu = id_aa64dfr1 & __ID_AA64DFR1_EL1_SPMU;

    if (!g_cpu_features.b16b16) {
        g_cpu_features.sve = CPU_FEAT_SVE;
    }

    if (!g_cpu_features.sme_f16f16) {
        if (g_cpu_features.sme > CPU_FEAT_SME2) {
            g_cpu_features.sme = CPU_FEAT_SME2;
        }
    }

    g_cpu_features.mixed_endian_support = id_aa64mmfr0 & __ID_AA64MMFR0_BIGEND;
    g_cpu_features.granule_16k_supported =
        id_aa64mmfr0 & __ID_AA64MMFR0_TGRAN16;
    g_cpu_features.granule_64k_supported =
        id_aa64mmfr0 & __ID_AA64MMFR0_TGRAN64;
    
    g_cpu_features.lse2 == 1 ? \
    __aarch64_have_lse_atomics = true : \
    __aarch64_have_lse_atomics = false;
}