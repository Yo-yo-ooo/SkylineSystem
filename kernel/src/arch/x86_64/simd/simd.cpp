
#include <klib/klib.h>

#include <stdint.h>
#include <arch/x86_64/cpu.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/simd/simd.h>

static uint8_t alignas(64) initCtx[PAGE_SIZE];

static void simd_xsave_init(void)
{
    kinfoln("DO XSAVE INIT");
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