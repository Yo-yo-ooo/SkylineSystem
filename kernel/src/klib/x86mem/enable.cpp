#include <klib/sysflag.h>
#include <klib/klib.h>
#include <klib/x86/enable.h>
#include <klib/x86/xstate.h>

#if 0

uint8_t BYTE_ALIGNMENT = 0x0F;
static volatile uint64_t xcr0;
static volatile int32_t x86_avx_enable_lock;
static uint64_t xstate_support_mask;

#define CR4_OSXMMEXCPT (1 << 10)
#define CR4_OSXSAVE (1 << 18) // cr4


static uint64_t xsave_bitmask(int what) {
    uint64_t bitmask = 0;
    if(what & XSAVE_X87)
        bitmask &= 0x01;
    if(what & XSAVE_SSE)
        bitmask &= 0x02;
    if(what & XSAVE_AVX)
        bitmask &= 0x04;
    if(what & XSAVE_AVX512)
        bitmask &= 0xe0;

    return bitmask & xstate_support_mask;
}

void xsave(int what, struct xstate_buffer* to) {
    uint64_t mask = xsave_bitmask(what);
    xsaves((uint8_t*)to->begin, mask);
}


void xrstor(int what, struct xstate_buffer* from) {
    uint64_t mask = xsave_bitmask(what);
    xsaves((uint8_t*)from->begin, mask);
}

int32_t xstate_enable(void) {
    spinlock_lock(&x86_avx_enable_lock);
    uint64_t rf = get_rflags();
    volatile uint64_t cr4 = get_cr4();
    kinfoln("Hello");
    cr4 = cr4 | CR4_OSXSAVE | CR4_OSXMMEXCPT;
    kinfoln("OK!");
    __asm__ volatile("mov %0,%%cr4" : :"r"(cr4) : "memory");
    kinfoln("OK!2");
    set_rflags(rf);
    spinlock_unlock(&x86_avx_enable_lock);

    return 0;   
}

int32_t avx_enable(void) {
    xcr0 = 0x007; // x87, sse, avx


    xsetbv(0, xcr0);


    sysflag_g.AVXEnabled = 1;
    BYTE_ALIGNMENT = 0x1F;


    return 0;
}

int32_t avx512_enable(void) {
    uint64_t rf = get_rflags();

    xcr0 = 0x0e7; // x87, sse, avx, avx512

    asm("cli");

    xsetbv(0, xcr0);

    if(xgetbv(0) == xcr0) return 1;


    set_rflags(rf);

    sysflag_g.AVX512Enabled = 1;
    BYTE_ALIGNMENT = 0x3F;

    asm("sti");

    return 0;
}

#endif
