// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <klib/kio.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/cpu/smap.h>
#include <klib/klib.h>

/* Global SMAP gate: true only after CR4.SMAP is set (SMP is homogeneous). */
bool g_smap_enabled = false;


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
    if (ebx & (1 << 20)) { // SMAP
        asm volatile("mov %%cr4, %0" :"=r"(cr4) : : "memory");
        asm volatile("mov %0, %%cr4" : : "r"(cr4 | (1 << 21)) : "memory");
        cpu->ISSMAP_ENABLEED = true;
        g_smap_enabled = true;
        /* Clear any stale RFLAGS.AC left by firmware/bootloader. */
        asm volatile("clac" ::: "memory");
    }
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

/* 生成随机 canary — 高字节清零 (null 终止, 防 strcpy 穿透) */
__attribute__((no_stack_protector))
void init_stack_canary(void) {
    uint64_t val = 0;

    /* 优先 RDRAND */
    if (has_rdrand()) {
        val = rdrand64_retry(10);
        if(val == 0){
            uint64_t tsc;
            __asm__ volatile("rdtsc" : "=A"(tsc));
            val = tsc ^ (uint64_t)&val ^ 0xDEADBEEFCAFEBABE;
        }
    } else {
        /* 回退: TSC + CPUID 混合熵 */
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        val = ((uint64_t)hi << 32) | lo;

        /* CPUID.0H 给点额外熵 */
        uint32_t a, b, c, d;
        asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
        val ^= ((uint64_t)b << 16) ^ ((uint64_t)d << 8) ^ (uint64_t)c;
    }

    /* 防止全零 (全零 = 无保护) */
    if ((val & 0x00FFFFFFFFFFFFFFUL) == 0)
        val = 0x0043414E41525900UL;  /* "\0RANAC\0" */

    /* LSB 清零: little-endian 下 canary 最低地址字节为 0x00,
    栈溢出方向最先命中 → strcpy 立即停, canary 不被破坏 */
    ((unsigned char *)&val)[0] = 0;
    __stack_chk_guard = val;
}

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