
#include <klib/klib.h>

#include <stdint.h>
#include <arch/x86_64/cpu.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/x86/simd.h>

static inline bool cpuid_is_xsave_avail(void)
{
    uint32_t ecx;
    uint32_t unused;
    cpuid(CPUID_FEATURE_ID, 0, &unused, &unused, &ecx, &unused);
    return ecx & CPUID_ECX_XSAVE_AVAIL;
}

static inline bool cpuid_is_avx_avail(void)
{
    uint32_t ecx;
    uint32_t unused;
    cpuid(CPUID_FEATURE_ID, 0, &unused, &unused, &ecx, &unused);
    return ecx & CPUID_ECX_AVX_AVAIL;
}

static inline bool cpuid_is_avx512_avail(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t unused;
    cpuid(CPUID_FEATURE_EXTENDED_ID, 0, &eax, &ebx, &unused, &unused);
    return (eax != 0) && (ebx & CPUID_EBX_AVX512_AVAIL);
}

static uint8_t alignas(64) initCtx[PAGE_SIZE];

static void simd_xsave_init(void)
{
    cr4_write(cr4_read() | CR4_XSAVE_ENABLE);

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

    xcr0_write(0, xcr0);
}

void simd_cpu_init(void)
{
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

    kinfo("cpu%d simd ", this_cpu()->id);
    if (cpuid_is_xsave_avail())
    {
        kinfo("xsave ");
    }
    if (cpuid_is_avx_avail())
    {
        kinfo("avx ");
    }
    if (cpuid_is_avx512_avail())
    {
        kinfo("avx512 ");
    }
    kinfo("enabled\n");
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