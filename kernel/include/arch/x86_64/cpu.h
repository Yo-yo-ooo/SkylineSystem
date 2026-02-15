#pragma once

#include <arch/x86_64/cpuid.h>
#include <stdint.h>
#include <klib/kio.h>

//Do NOT write any C++ things in this file!!!

#define IA32_GS_MSR 0xC0000101
#define IA32_GS_KERNEL_MSR 0xC0000102

inline uint64_t read_cpu_gs() {
    return rdmsr(IA32_GS_MSR);
}

inline void write_cpu_gs(uint64_t value) {
    wrmsr(IA32_GS_MSR, value);
}

inline uint64_t read_kernel_gs() {
    return rdmsr(IA32_GS_KERNEL_MSR);
}

inline void write_kernel_gs(uint64_t value) {
    wrmsr(IA32_GS_KERNEL_MSR, value);
}

inline uint64_t fpu_init() {
    u32 eax, unused, edx;
    __get_cpuid(1, &eax, &unused, &unused, &edx);
    if (!(edx & (1 << 0)))
        return 1; // CPU Doesnt support FPU.

    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0) : : "memory");
    cr0 &= ~(1 << 3); // Clear TS bit
    cr0 &= ~(1 << 2); // Clear EM bit
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("fninit");
    return 0;
}

inline void sse_enable() {
    uint64_t cr0;
    uint64_t cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0) : : "memory");
    cr0 &= ~((uint64_t)1 << 2);
    cr0 |= (uint64_t)1 << 1;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" :"=r"(cr4) : : "memory");
    cr4 |= (uint64_t)3 << 9;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

}

#define XCR0_XSAVE_SAVE_X87 (1 << 0)
#define XCR0_XSAVE_SAVE_SSE (1 << 1)
#define XCR0_AVX_ENABLE (1 << 2)
#define XCR0_AVX512_ENABLE (1 << 5)
#define XCR0_ZMM0_15_ENABLE (1 << 6)
#define XCR0_ZMM16_32_ENABLE (1 << 7)

#define MSR_LAPIC 0x1B
#define MSR_CPU_ID 0xC0000103 // IA32_TSC_AUX
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SYSCALL_FLAG_MASK 0xC0000084
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xc0000102

#define EFER_SYSCALL_ENABLE 1

#define RFLAGS_CARRY (1 << 0)
#define RFLAGS_ALWAYS_SET (1 << 1)
#define RFLAGS_PARITY (1 << 2)
#define RFLAGS_RESERVED1 (1 << 3)
#define RFLAGS_AUX_CARRY (1 << 4)
#define RFLAGS_RESERVED2 (1 << 5)
#define RFLAGS_ZERO (1 << 6)
#define RFLAGS_SIGN (1 << 7)
#define RFLAGS_TRAP (1 << 8)
#define RFLAGS_INTERRUPT_ENABLE (1 << 9)
#define RFLAGS_DIRECTION (1 << 10)
#define RFLAGS_OVERFLOW (1 << 11)
#define RFLAGS_IOPL (1 << 12 | 1 << 13)
#define RFLAGS_NESTED_TASK (1 << 14)
#define RFLAGS_MODE (1 << 15)

#define CR0_PROTECTED_MODE_ENABLE (1 << 0)
#define CR0_MONITOR_CO_PROCESSOR (1 << 1)
#define CR0_EMULATION (1 << 2)
#define CR0_TASK_SWITCHED (1 << 3)
#define CR0_EXTENSION_TYPE (1 << 4)
#define CR0_NUMERIC_ERROR_ENABLE (1 << 5)
#define CR0_WRITE_PROTECT (1 << 16)
#define CR0_ALIGNMENT_MASK (1 << 18)
#define CR0_NOT_WRITE_THROUGH (1 << 29)
#define CR0_CACHE_DISABLE (1 << 30)
#define CR0_PAGING_ENABLE (1 << 31)

#define CR4_PAGE_GLOBAL_ENABLE (1 << 7)
#define CR4_FXSR_ENABLE (1 << 9)
#define CR4_SIMD_EXCEPTION (1 << 10)
#define CR4_XSAVE_ENABLE (1 << 18)

#define CPUID_FEATURE_ID 0x1
#define CPUID_FEATURE_EXTENDED_ID 0x7
#define CPUID_EXTENDED_STATE_ENUMERATION 0xD
#define CPUID_EBX_AVX512_AVAIL (1 << 16)
#define CPUID_ECX_XSAVE_AVAIL (1 << 26)
#define CPUID_ECX_AVX_AVAIL (1 << 28)



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

static inline bool check_xsaves_support() {
    uint32_t eax, ebx, ecx, edx;

    // 2. 访问 0x0D 叶，子叶 1
    eax = 0x0D;
    ecx = 1;
    //__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
    cpuid(0x0D,1,&eax,&ebx,&ecx,&edx);
    // 3. 检查 EAX 的第 3 位 (XSAVES support)
    return (eax & (0x1 << 3));
}

static inline bool cpuid_is_avx512_avail(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t unused;
    cpuid(CPUID_FEATURE_EXTENDED_ID, 0, &eax, &ebx, &unused, &unused);
    return (eax != 0) && (ebx & CPUID_EBX_AVX512_AVAIL);
}


static inline bool cpuid_is_avx2_avail(void){
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 5)) != 0;
}