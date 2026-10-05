//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * x86mem 运行时参数 —— NT (非时序) 存储阈值的 CPUID 探测
 *
 * 背景: 阈值原来是 x86mem.h 里的编译期常量 3MB。不同机器的 L3 从 1MB 到
 * 上百 MB 不等, 固定值必然两头不讨好: L3 大的机器过早改用 streaming store
 * (数据本来还装得下), L3 小的机器又太晚才切换。改成启动时用 CPUID 探测
 * 真实 L3 容量填进去。
 *
 * 初始化入口 (二选一, 各管各的链接单元):
 *   - 用户态: lib/base/arch/x86_64/init.c      _init_runtime_and_global_variables()
 *   - 内核态: kernel/src/arch/x86_64/init.cpp  x86_64_init()
 *
 * !! 符号唯一性 !!
 * 本目录下的每个 .c 都会被 4 个指令集 tier (base/avx/avx2/avx512) 各编译一遍,
 * 再 ld -r 合成一个 memops.o。所以这里任何全局符号都会出 4 份 —— 强定义会在
 * 链接期撞车 (重复的 weak 定义则会被 PE-COFF 的 ld -r 改名为
 * .weak.<sym>.<file> 而失去全局定义)。
 * 因此全局符号一律用 X86MEM_NOT_COMPILE_AVX 圈起来: 这个宏只有 base tier
 * 才有, 而 base tier 恒被编译 (Makefile: TARGET_ARCHS := base)。
 * ==========================================================================*/
#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__
#ifdef X86MEM_NOT_COMPILE_AVX

size_t x86mem_cache_limit = X86MEM_CACHE_LIMIT_DEFAULT;

/* x86-64 下 rbx 不是 PIC 保留寄存器, 可以直接当输出操作数用
 * (与 lib/base/arch/x86_64/init.c 里的 cpuid 包装一致)。 */
static inline void xm_cpuid(uint32_t leaf, uint32_t sub,
                            uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t ea, eb, ec, ed;
    asm volatile("cpuid"
                 : "=a"(ea), "=b"(eb), "=c"(ec), "=d"(ed)
                 : "a"(leaf), "c"(sub));
    *a = ea; *b = eb; *c = ec; *d = ed;
}

/* ============================================================================
 * 探测 L3 容量 (字节)。探测不到返回 0 —— 由调用方决定回退策略。
 *
 * 两条路:
 *  1) CPUID leaf 4 (确定性 cache 参数): Intel 与 AMD 现代 CPU 都支持。
 *     容量 = (ways+1) * (partitions+1) * (line+1) * (sets+1)
 *     leaf 4 报的是该级 cache 的**整块**容量 (L3 通常是整个 package 共享),
 *     正是"超过它就别指望 cache 装下"想要的那个数, 不需要再乘共享线程数。
 *  2) AMD Fn8000_0006_EDX[31:18]: L3 容量, 单位 512KB。
 *     用在没有 leaf 4 的老 CPU 上。
 * ==========================================================================*/
size_t x86mem_detect_l3_size(void)
{
    uint32_t a, b, c, d;
    uint32_t i;

    xm_cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 4u) {
        for (i = 0; i < 64u; i++) {
            xm_cpuid(4, i, &a, &b, &c, &d);
            if ((a & 0x1Fu) == 0u) break;              /* cache type 0 = 结束 */
            if (((a >> 5) & 0x7u) != 3u) continue;     /* 只要 level 3 */

            size_t line = (size_t)((b & 0xFFFu) + 1u);
            size_t part = (size_t)(((b >> 12) & 0x3FFu) + 1u);
            size_t ways = (size_t)(((b >> 22) & 0x3FFu) + 1u);
            size_t sets = (size_t)(c + 1u);
            size_t sz   = ways * part * line * sets;
            if (sz != 0) return sz;
        }
    }

    /* AMD / 老 CPU 回退 */
    xm_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000006u) {
        xm_cpuid(0x80000006u, 0, &a, &b, &c, &d);
        size_t sz = (size_t)((d >> 18) & 0x3FFFu) * 512u * 1024u;
        if (sz != 0) return sz;
    }

    return 0;    /* 探测失败 */
}

void x86mem_init_cache_limit(void)
{
    size_t v = x86mem_detect_l3_size();

    if (v == 0) return;                                  /* 保持默认值 */

    v = v * 3 / 4;

    if (v < X86MEM_CACHE_LIMIT_MIN) v = X86MEM_CACHE_LIMIT_MIN;
    if (v > X86MEM_CACHE_LIMIT_MAX) v = X86MEM_CACHE_LIMIT_MAX;

    x86mem_cache_limit = v;
}

#endif /* X86MEM_NOT_COMPILE_AVX */
#endif /* __x86_64__ */
