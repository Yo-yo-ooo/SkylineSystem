
#include <klib/klib.h>

#include <stdint.h>
#include <arch/x86_64/cpu.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/simd/simd.h>
#include <arch/x86_64/simd/xsave.h>

static uint8_t alignas(64) initCtx[PAGE_SIZE];

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

    
    


   /*  kinfoln("WRITE CR4");
    uint64_t xcr0 = 0;

    xcr0 = xcr0 | XCR0_XSAVE_SAVE_X87 | XCR0_XSAVE_SAVE_SSE;

    if (cpuid_is_avx_avail())
    {
        xcr0 = xcr0 | XCR0_AVX_ENABLE;

        if (cpuid_is_avx512_avail())
        {
            xcr0 = xcr0 | XCR0_AVX512_ENABLE | XCR0_ZMM0_15_ENABLE | XCR0_ZMM16_32_ENABLE;
        }
    }

    xcr0_write(0, xcr0); */
    
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
    }

    asm volatile("fninit");
    if (cpuid_is_xsave_avail())
    {
        asm volatile("xsave %0" : : "m"(*initCtx), "a"(UINT64_MAX), "d"(UINT64_MAX) : "memory");
    }
    else
    {
        asm volatile("fxsave (%0)" : : "r"(initCtx));
    }

    kpok("cpu simd:");
    if (cpuid_is_xsave_avail())
    {
        kprintf("xsave ");i++;
    }
    if (cpuid_is_avx_avail())
    {
        kprintf("avx ");i++;
    }
    if (cpuid_is_avx512_avail())
    {
        kprintf("avx512 ");i++;
    }
    kprintf("enabled\n");
    if(i < 3)
        cpu->SupportSIMD = false;
    else
        cpu->SupportSIMD = true;
}

uint64_t simd_ctx_init(simd_ctx_t* ctx)
{
    ctx->buffer = PMM::Request();
    if (ctx->buffer == nullptr)
        return -1;

    __memcpy(ctx->buffer, initCtx, PAGE_SIZE);

    return 0;
}

void simd_ctx_deinit(simd_ctx_t* ctx)
{
    PMM::Free(ctx->buffer);
}

void simd_ctx_save(simd_ctx_t* ctx)
{
    if (cpuid_is_xsave_avail())
    {
        asm volatile("xsave %0" : : "m"(*ctx->buffer), "a"(UINT64_MAX), "d"(UINT64_MAX) : "memory");
    }
    else
    {
        asm volatile("fxsave (%0)" : : "r"(ctx->buffer));
    }
}

void simd_ctx_load(simd_ctx_t* ctx)
{
    if (cpuid_is_xsave_avail())
    {
        asm volatile("xrstor %0" : : "m"(*ctx->buffer), "a"(UINT64_MAX), "d"(UINT64_MAX) : "memory");
    }
    else
    {
        asm volatile("fxrstor (%0)" : : "r"(ctx->buffer));
    }
}