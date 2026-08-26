// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <klib/kio.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>


void enable_smep_smap() {
    uint32_t eax, ebx, ecx, edx;
    eax = 7; ecx = 0;
    cpuid(7,0,&eax, &ebx, &ecx, &edx);
    cpu_t *cpu = this_cpu();
    uint64_t cr4 = 0;
    if (ebx & (1 << 7)) { // SMEP
        asm volatile("mov %%cr4, %0" :"=r"(cr4) : : "memory");
        asm volatile("mov %0, %%cr4" : : "r"(cr4 | (1 << 20)) : "memory");
        cpu->ISSMEP_ENABLEED = true;
    }
    /*if (ebx & (1 << 20)) { // SMAP
        //write_cr4(read_cr4() | (1 << 21));
        asm volatile("mov %%cr4, %0" :"=r"(cr4) : : "memory");
        asm volatile("mov %0, %%cr4" : : "r"(cr4 | (1 << 21)) : "memory");
        cpu->ISSMAP_ENABLEED = true;
    }*/
}

int has_rdrand(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1) : );
    return (ecx >> 30) & 1;   // ECX bit 30 = RDRAND
}

uint64_t rdrand64_retry(int retries) {
    uint64_t val;
    int ok;
    for (int i = 0; i < retries; i++) {
        __asm__ volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(val), "=qm"(ok)
            :
            : "cc"
        );
        if (ok) return val;
    }
    return 0; // 失败返回 0
}

/* GCC -mstack-protector-guard=global 直接引用此符号 */
unsigned long __stack_chk_guard = 0x000A0D0BEEFCAFE0UL;

/* GCC 在 canary 不匹配时调用此函数 — 必须不返回 */
__attribute__((noreturn, noinline))
extern "C" void __stack_chk_fail(void) {
    /* 栈已损坏 — 尽量少用栈 */
    asm volatile("cli");
    /* 直接写串口或帧缓冲, 不走 kerrorln (它有栈开销) */
    /* 这里用 Panic 作为兜底 — 如果 Panic 也崩了, 最后有 hlt */
    Serial::Init();
    Serial::Writeln("STACK SMASHING DETECTED");
    __builtin_unreachable();
}