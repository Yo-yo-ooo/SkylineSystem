
#include <klib/klib.h>

#include <stdint.h>
#include <arch/x86_64/cpu.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/simd/simd.h>
#include <arch/x86_64/simd/xsave.h>
uint32_t MaxXsaveSize = 0;

void simd_xsave_init(void)
{

    kinfoln("DO XSAVE INIT");
    uint64_t cr4 = cr4_read() | CR4_XSAVE_ENABLE;
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    wrmsr(IA32_MSR_XFD, XSAVE_FEAT_XFD_MASK);

    xsave_feat_mask_t xsave_supervisor_features =
        __XSAVE_FEAT_MASK(XSAVE_FEAT_X87)
      | __XSAVE_FEAT_MASK(XSAVE_FEAT_SSE)
      | __XSAVE_FEAT_MASK(XSAVE_FEAT_MPX_BNDREGS)
      | __XSAVE_FEAT_MASK(XSAVE_FEAT_MPX_BNDCSR)
      | __XSAVE_FEAT_MASK(XSAVE_FEAT_CET_USER)
      | __XSAVE_FEAT_MASK(XSAVE_FEAT_CET_SUPERVISOR);

    uint64_t rax, rbx, rcx, rdx;
    cpuid(13/*CPUID_GET_FEATURES_XSAVE*/,
              /*subleaf=*/0,
              &rax,
              &rbx,
              &rcx,
              &rdx);
    
    uint64_t flags = __XSAVE_FEAT_MASK(XSAVE_FEAT_X87) 
    | __XSAVE_FEAT_MASK(XSAVE_FEAT_SSE) 
    | __XSAVE_FEAT_MASK(XSAVE_FEAT_MPX_BNDREGS) 
    | __XSAVE_FEAT_MASK(XSAVE_FEAT_MPX_BNDCSR);
    xsave_feat_mask_t xsave_user_features = ((uint64_t)rdx << 32 | rax);

    if(cpuid_is_avx_avail()){
        xsave_user_features |= __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX);
        flags |= __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX);
        if(cpuid_is_avx512_avail()){
            flags = flags
            | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_OPMASK) 
            | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_ZMM_HI256) 
            | __XSAVE_FEAT_MASK(XSAVE_FEAT_AVX_512_HI16_ZMM);
        }
    }
    xsave_user_features &= flags;

    xsave_set_supervisor_features(xsave_supervisor_features);
    kinfoln("Read xcr0");
    const uint64_t xcr = read_xcr(XCR_XSTATE_FEATURES_ENABLED);
    kinfoln("Write to xcr0");
    write_xcr(XCR_XSTATE_FEATURES_ENABLED, xcr | xsave_user_features);

    return;
}

void simd_cpu_init(cpu_t *cpu)
{
    uint8_t i = 0;
    cr0_write(cr0_read() & ~((uint64_t)CR0_EMULATION));
    cr0_write(cr0_read() | CR0_MONITOR_CO_PROCESSOR | CR0_NUMERIC_ERROR_ENABLE);

    cr4_write(cr4_read() | CR4_FXSR_ENABLE | CR4_SIMD_EXCEPTION);

    if (cpuid_is_xsave_avail())
    {
        simd_xsave_init();
        if(check_xsaves_support()){
            kinfoln("Checked Xsaves enable.");
            wrmsr(IA32_XSS, 0);
            kinfoln("Xsaves INIT!");
        }
        uint32_t eax, ebx, ecx, edx;
        uint32_t leaf = 0x0D;
        uint32_t subleaf = 0;

        // 调用 CPUID (EAX=0Dh, ECX=0)
        asm volatile("cpuid"
                    : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                    : "a"(leaf), "c"(subleaf));
        //MaxXsaveSize = ebx;
        if(ebx > MaxXsaveSize)
            MaxXsaveSize = ebx;
    }
    

    asm volatile("fninit");

    kpok("cpu simd:");
    if (cpuid_is_xsave_avail()){
        kprintf("xsave ");i++;
        cpu->SupportXSAVE = true;
    }
    if (cpuid_is_avx_avail()){
        kprintf("avx ");i++;
        cpu->SupportAVX = true;
        if((cpu->SupportAVX2 = cpuid_is_avx2_avail()))
            kprintf("avx2 ");
    }
    if (cpuid_is_avx512_avail()){
        kprintf("avx512 ");i++;
        cpu->SupportAVX512 = true;
    }
    if(cpuid_is_sse4_2_avail()){
        cpu->SupportSSE4_2 = true;
    }
    kprintf("enabled\n");
    if(i > 0)
        cpu->SupportSIMD = true;
    else
        cpu->SupportSIMD = false;
}
