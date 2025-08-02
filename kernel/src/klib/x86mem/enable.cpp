#include <klib/sysflag.h>

#include <klib/x86mem/enable.h>

uint8_t BYTE_ALIGNMENT = 0x0F;
static uint64_t xcr0;

uint32_t avx_enable(void) {
    uint64_t rf = get_rflags();

    xcr0 = 0x007; // x87, sse, avx
    xsetbv(0, xcr0);

    set_rflags(rf);

    sysflag_g.AVXEnabled = 1;
    BYTE_ALIGNMENT = 0x1F;

    return 0;
}

uint32_t avx512_enable(void) {
    uint64_t rf = get_rflags();

    xcr0 = 0x0e7; // x87, sse, avx, avx512
    xsetbv(0, xcr0);

    if(xgetbv(0) == xcr0) return 1;


    set_rflags(rf);

    sysflag_g.AVX512Enabled = 1;
    BYTE_ALIGNMENT = 0x3F;

    return 0;
}

